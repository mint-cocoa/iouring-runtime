#include <iouring_runtime/web/HttpSession.h>

#include <iouring_runtime/web/HttpResponse.h>
#include <iouring_runtime/web/Router.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

namespace iouring_runtime::web {

namespace {

struct ParseErrorResponse {
    HttpStatus status;
    std::string_view body;
};

ParseErrorResponse MapParseError(HttpParseStatus status) {
    switch (status) {
        case HttpParseStatus::kUrlTooLong:
            return {HttpStatus::kUriTooLong, "URI Too Long"};
        case HttpParseStatus::kHeadersTooLarge:
            return {HttpStatus::kRequestHeaderFieldsTooLarge,
                    "Request Header Fields Too Large"};
        case HttpParseStatus::kBodyTooLarge:
            return {HttpStatus::kPayloadTooLarge, "Payload Too Large"};
        case HttpParseStatus::kBadRequest:
        case HttpParseStatus::kOk:
            break;
    }
    return {HttpStatus::kBadRequest, "Bad Request"};
}

} // namespace

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
}

void HttpSession::SendResponse(core::buffer::SendBufferRef buf) {
    Send(std::move(buf));
}

void HttpSession::OnRecv(std::span<const std::byte> data) {
    HandleHttpRecv(data.data(), static_cast<std::uint32_t>(data.size()));
}

bool HttpSession::OnTimeoutTick(std::chrono::steady_clock::time_point now) {
    return FailRequestDeadlineIfExpired(now);
}

void HttpSession::HandleHttpRecv(const std::byte* data, std::uint32_t len) {
    if (StartOrFailRequestDeadline()) {
        return;
    }

    if (recv_buffer_.IsEmpty()) {
        const auto* chars = reinterpret_cast<const char*>(data);
        const auto consumed = parser_.Feed(chars, len);

        if (parser_.HasError()) {
            const auto error = MapParseError(parser_.Status());
            auto buffer = HttpResponse()
                .Status(error.status)
                .ContentType("text/plain")
                .Body(std::string(error.body))
                .KeepAlive(false)
                .Build(Pool());
            if (buffer) {
                Send(std::move(buffer));
            }
            Disconnect();
            return;
        }

        if (consumed < len) {
            auto append = recv_buffer_.Append(
                std::span<const std::byte>(data + consumed, len - consumed));
            if (!append) {
                spdlog::error("HttpSession[fd={}]: recv buffer overflow", Fd());
                Disconnect();
                return;
            }
        }
        return;
    }

    auto append = recv_buffer_.Append(std::span<const std::byte>(data, len));
    if (!append) {
        spdlog::error("HttpSession[fd={}]: recv buffer overflow", Fd());
        Disconnect();
        return;
    }

    auto region = recv_buffer_.ReadRegion();
    const auto* chars = reinterpret_cast<const char*>(region.data());
    const auto region_len = static_cast<std::uint32_t>(region.size());
    const auto consumed = parser_.Feed(chars, region_len);

    if (parser_.HasError()) {
        const auto error = MapParseError(parser_.Status());
        auto buffer = HttpResponse()
            .Status(error.status)
            .ContentType("text/plain")
            .Body(std::string(error.body))
            .KeepAlive(false)
            .Build(Pool());
        if (buffer) {
            Send(std::move(buffer));
        }
        Disconnect();
        return;
    }

    recv_buffer_.OnRead(consumed);
    if (recv_buffer_.ShouldCompact()) {
        recv_buffer_.Compact();
    }
}

bool HttpSession::HandleRequest(HttpRequest& request) {
    request_in_progress_ = false;
    request.request_id = GenerateRequestId();
    const auto started_at = std::chrono::steady_clock::now();

    HttpResponse response(*this, Pool());
    response.KeepAlive(request.keep_alive);

    RequestContext ctx{
        .session = *this,
        .request = request,
        .response = response,
        .pool = Pool(),
        .remote_addr = RemoteAddr(),
        .request_id = request.request_id,
    };
    router_.Dispatch(ctx);

    if (!response.IsSent()) {
        response.Send();
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    spdlog::info(
        "HttpSession: request_id={} remote={} method={} path={} status={} bytes={} duration_ms={}",
        request.request_id,
        ctx.remote_addr.empty() ? "-" : std::string(ctx.remote_addr),
        HttpMethodToString(request.method),
        request.path,
        static_cast<int>(response.StatusCode()),
        response.LastBuiltBytes(),
        elapsed.count());

    if (!request.keep_alive) {
        DisconnectAfterFlush();
        return false;
    }

    return true;
}

bool HttpSession::StartOrFailRequestDeadline() {
    if (request_timeout_.count() == 0) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (request_in_progress_) {
        return FailRequestDeadlineIfExpired(now);
    }

    request_in_progress_ = true;
    request_started_at_ = now;
    return false;
}

bool HttpSession::FailRequestDeadlineIfExpired(
    std::chrono::steady_clock::time_point now) {
    if (request_timeout_.count() == 0 || !request_in_progress_) {
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - request_started_at_);
    if (elapsed < request_timeout_) {
        return false;
    }

    spdlog::warn(
        "HttpSession[fd={}]: [DISC:REQUEST_TIMEOUT] elapsed={}ms timeout={}ms",
        Fd(), elapsed.count(), request_timeout_.count());
    SendRequestTimeoutResponse();
    Disconnect();
    return true;
}

void HttpSession::SendRequestTimeoutResponse() {
    auto buffer = HttpResponse()
        .Status(HttpStatus::kRequestTimeout)
        .ContentType("text/plain")
        .Body("Request Timeout")
        .KeepAlive(false)
        .Build(Pool());
    if (buffer) {
        Send(std::move(buffer));
    }
}

} // namespace iouring_runtime::web
