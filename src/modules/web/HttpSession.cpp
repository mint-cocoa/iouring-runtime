#include <iouring_runtime/web/HttpSession.h>
#include <iouring_runtime/web/HttpResponse.h>

#include <cstddef>
#include <cstdio>
#include <utility>

namespace iouring_runtime::web {

std::string HttpSession::GenerateRequestId() {
    thread_local std::uint64_t counter = 0;
    ++counter;
    char buffer[24];
    const int written = std::snprintf(
        buffer, sizeof(buffer), "req-%013llx",
        static_cast<unsigned long long>(counter));
    return std::string(buffer, written > 0 ? static_cast<std::size_t>(written) : 0);
}

HttpSession::HttpSession(int fd, core::ring::IoRing& ring,
                         core::buffer::BufferPool& pool,
                         const Router& router,
                         std::uint32_t send_queue_max_pending,
                         HttpParserOptions parser_options,
                         std::chrono::milliseconds request_timeout)
    : Session(fd, ring, pool, send_queue_max_pending)
    , router_(router)
    , parser_(parser_options)
    , request_timeout_(request_timeout) {
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

void HttpSession::OnRecv(std::span<const std::byte> data) {
    HandleHttpRecv(data.data(), static_cast<std::uint32_t>(data.size()));
}

void HttpSession::OnDisconnected() {
    AbortStreamingRequest();
}

bool HttpSession::OnTimeoutTick(std::chrono::steady_clock::time_point now) {
    return FailRequestDeadlineIfExpired(now);
}

} // namespace iouring_runtime::web
