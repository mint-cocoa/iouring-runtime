#include <iouring/http/HttpSession.h>

#include <iouring/http/HttpResponse.h>

#include <iouring/observability/Logging.h>

namespace obs = iouring::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kHttp;
}

namespace iouring::http {

void HttpSession::BeginDeferredResponse() {
    ++async_work_count_;
    BeginAppIo();
}

void HttpSession::EndDeferredResponse() {
    if (async_work_count_ > 0) {
        --async_work_count_;
    }
    EndAppIo();
}

bool HttpSession::HasPendingAppWork() const {
    return async_work_count_ != 0 || file_stream_.active || body_stream_.active;
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

    obs::LogWarn(kLogCategory,
        "HttpSession[fd={}]: [DISC:REQUEST_TIMEOUT] elapsed={}ms timeout={}ms",
        Fd(), elapsed.count(), request_timeout_.count());
    AbortStreamingRequest();
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

} // namespace iouring::http
