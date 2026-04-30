#include <iouring_runtime/web/HttpSession.h>
#include <iouring_runtime/web/HttpResponse.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <utility>

namespace iouring_runtime::web {

namespace {
std::atomic<std::uint64_t> g_request_id_counter{1};
} // namespace

std::string HttpSession::GenerateRequestId() {
    char buffer[24];
    const int written = std::snprintf(
        buffer, sizeof(buffer), "req-%013llx",
        static_cast<unsigned long long>(
            g_request_id_counter.fetch_add(1, std::memory_order_relaxed)));
    return std::string(buffer, written > 0 ? static_cast<std::size_t>(written) : 0);
}

HttpSession::HttpSession(int fd, core::ring::IoRing& ring,
                         core::buffer::BufferPool& pool,
                         const Router& router,
                         std::uint32_t send_queue_max_pending,
                         HttpParserOptions parser_options,
                         std::chrono::milliseconds request_timeout,
                         std::chrono::milliseconds slow_request_threshold)
    : Session(fd, ring, pool, send_queue_max_pending)
    , router_(router)
    , parser_(parser_options)
    , request_timeout_(request_timeout)
    , slow_request_threshold_(slow_request_threshold) {
    if (request_timeout_.count() > 0) {
        SetTimeoutCheckInterval(request_timeout_ / 4);
    }
    parser_.SetOnRequest([this](HttpRequest& request) {
        return HandleRequest(request);
    });
    parser_.SetOnHeaders([this](HttpRequest& request) {
        return PrepareRequestBody(request);
    });
    parser_.SetOnBodyChunk([this](HttpRequest& request,
                                  std::span<const std::byte> chunk) {
        return HandleBodyChunk(request, chunk);
    });
}

void HttpSession::SendResponse(core::buffer::SendBufferRef buf) {
    Send(std::move(buf));
}

bool HttpSession::StartFileStream(core::buffer::SendBufferRef header,
                                  int file_fd,
                                  std::uint64_t file_size,
                                  std::uint32_t chunk_size,
                                  std::uint32_t max_chunks_per_write) {
    if (!header || file_fd < 0 || file_stream_.active) {
        return false;
    }

    file_stream_.fd.Reset(file_fd);
    file_stream_.remaining_bytes = file_size;
    file_stream_.chunk_size = std::max<std::uint32_t>(chunk_size, 4 * 1024);
    file_stream_.max_chunks_per_write =
        std::max<std::uint32_t>(max_chunks_per_write, 1);
    file_stream_.active = true;

    if (!Send(std::move(header)).has_value()) {
        ResetFileStream();
        return false;
    }

    return PumpFileStream();
}

void HttpSession::OnRecv(std::span<const std::byte> data) {
    HandleHttpRecv(data.data(), static_cast<std::uint32_t>(data.size()));
}

void HttpSession::OnDisconnected() {
    ResetFileStream();
    AbortStreamingRequest();
}

void HttpSession::OnSocketDrained() {
    if (file_stream_.active) {
        PumpFileStream();
    }
}

bool HttpSession::OnTimeoutTick(std::chrono::steady_clock::time_point now) {
    return FailRequestDeadlineIfExpired(now);
}

} // namespace iouring_runtime::web
