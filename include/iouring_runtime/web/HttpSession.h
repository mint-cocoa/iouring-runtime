#pragma once

#include <iouring_runtime/core/RecvBuffer.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/web/HttpParser.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace iouring_runtime::web {

class DeferredResponse;
class HttpResponse;
class Router;
struct HttpStreamHandler;

class HttpSession : public core::io::Session {
public:
    HttpSession(int fd, core::ring::IoRing& ring,
                core::buffer::BufferPool& pool, const Router& router,
                std::uint32_t send_queue_max_pending = 4096,
                HttpParserOptions parser_options = {},
                std::chrono::milliseconds request_timeout = {});

    void SendResponse(core::buffer::SendBufferRef buf);
    bool StartFileStream(core::buffer::SendBufferRef header,
                         int file_fd,
                         std::uint64_t file_size,
                         std::uint32_t chunk_size,
                         std::uint32_t max_chunks_per_write);
    static std::string GenerateRequestId();

protected:
    void OnRecv(std::span<const std::byte> data) final;
    void OnDisconnected() final;
    bool HasPendingAppWork() const final;
    bool OnTimeoutTick(std::chrono::steady_clock::time_point now) final;
    void OnSocketDrained() final;

private:
    friend class DeferredResponse;

    struct StreamingRequestState {
        const HttpStreamHandler* handler = nullptr;
        HttpRequest* request = nullptr;
        std::unique_ptr<HttpResponse> response;
        bool rejected = false;
    };

    struct FileStreamState {
        core::io::SocketHandle fd;
        std::uint64_t remaining_bytes = 0;
        std::uint32_t chunk_size = 256 * 1024;
        std::uint32_t max_chunks_per_write = 4;
        bool active = false;
    };

    void HandleHttpRecv(const std::byte* data, std::uint32_t len);
    HttpBodyMode PrepareRequestBody(HttpRequest& request);
    bool HandleBodyChunk(HttpRequest& request, std::span<const std::byte> chunk);
    bool HandleRequest(HttpRequest& request);
    bool CompleteStreamingRequest(HttpRequest& request);
    void AbortStreamingRequest();
    bool StartOrFailRequestDeadline();
    bool FailRequestDeadlineIfExpired(std::chrono::steady_clock::time_point now);
    void SendRequestTimeoutResponse();
    void BeginDeferredResponse();
    void EndDeferredResponse();
    bool ActiveStreamResponseSent() const;
    bool PumpFileStream();
    void ResetFileStream();

    const Router& router_;
    HttpParser parser_;
    core::buffer::RecvBuffer recv_buffer_;
    FileStreamState file_stream_;
    std::chrono::milliseconds request_timeout_{0};
    std::chrono::steady_clock::time_point request_started_at_{};
    StreamingRequestState active_stream_;
    std::uint32_t async_work_count_ = 0;
    bool request_in_progress_ = false;
};

} // namespace iouring_runtime::web
