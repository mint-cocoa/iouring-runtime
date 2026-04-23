#pragma once

#include <iouring_runtime/core/RecvBuffer.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/web/HttpParser.h>

#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace iouring_runtime::web {

class Router;

class HttpSession : public core::io::Session {
public:
    HttpSession(int fd, core::ring::IoRing& ring,
                core::buffer::BufferPool& pool, const Router& router,
                std::uint32_t send_queue_max_pending = 4096,
                HttpParserOptions parser_options = {},
                std::chrono::milliseconds request_timeout = {});

    void SendResponse(core::buffer::SendBufferRef buf);
    static std::string GenerateRequestId();

protected:
    void OnRecv(std::span<const std::byte> data) final;
    bool OnTimeoutTick(std::chrono::steady_clock::time_point now) final;

private:
    void HandleHttpRecv(const std::byte* data, std::uint32_t len);
    bool HandleRequest(HttpRequest& request);
    bool StartOrFailRequestDeadline();
    bool FailRequestDeadlineIfExpired(std::chrono::steady_clock::time_point now);
    void SendRequestTimeoutResponse();

    const Router& router_;
    HttpParser parser_;
    core::buffer::RecvBuffer recv_buffer_;
    std::chrono::milliseconds request_timeout_{0};
    std::chrono::steady_clock::time_point request_started_at_{};
    bool request_in_progress_ = false;
};

} // namespace iouring_runtime::web
