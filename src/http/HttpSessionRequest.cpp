#include <iouring/http/HttpSession.h>

#include <iouring/http/HttpResponse.h>
#include <iouring/http/Router.h>

#include <iouring/observability/Logging.h>

namespace obs = iouring::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kHttp;
}

namespace iouring::http {

bool HttpSession::HandleRequest(HttpRequest& request) {
    if (active_stream_.handler) {
        return CompleteStreamingRequest(request);
    }

    request_in_progress_ = false;
    const bool log_request = obs::ShouldLog(obs::LogLevel::kInfo);
    const bool log_slow_request = slow_request_threshold_.count() > 0 &&
                                  obs::ShouldLog(obs::LogLevel::kWarn);
    const bool measure_request = log_request || log_slow_request;
    if (measure_request) {
        request.request_id = GenerateRequestId();
    } else {
        request.request_id.clear();
    }
    const auto started_at = measure_request ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};

    HttpResponse response(*this, Pool());
    response.KeepAlive(request.keep_alive);

    RequestContext ctx{
        .session = *this,
        .request = request,
        .response = response,
        .pool = Pool(),
        .remote_addr = log_request ? RemoteAddr() : std::string_view{},
        .request_id = request.request_id,
    };
    router_.Dispatch(ctx);

    if (!response.IsSent() && !response.IsDeferred()) {
        response.Send();
    }

    if (measure_request) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at);
        if (log_slow_request && elapsed >= slow_request_threshold_) {
            obs::LogWarn(kLogCategory,
                "HttpSession: [SLOW_REQUEST] request_id={} method={} path={} status={} bytes={} duration_ms={} threshold_ms={}",
                request.request_id,
                HttpMethodToString(request.method),
                request.path,
                static_cast<int>(response.StatusCode()),
                response.LastBuiltBytes(),
                elapsed.count(),
                slow_request_threshold_.count());
        }
        if (log_request) {
            obs::LogInfo(kLogCategory,
                "HttpSession: request_id={} remote={} method={} path={} status={} bytes={} duration_ms={}",
                request.request_id,
                ctx.remote_addr.empty() ? "-" : std::string(ctx.remote_addr),
                HttpMethodToString(request.method),
                request.path,
                static_cast<int>(response.StatusCode()),
                response.LastBuiltBytes(),
                elapsed.count());
        }
    }

    if (!request.keep_alive) {
        DisconnectAfterFlush();
        return false;
    }

    return true;
}

} // namespace iouring::http
