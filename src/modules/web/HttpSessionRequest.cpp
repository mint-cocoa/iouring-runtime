#include <iouring_runtime/web/HttpSession.h>

#include <iouring_runtime/web/HttpResponse.h>
#include <iouring_runtime/web/Router.h>

#include <iouring_runtime/observability/Logging.h>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kHttp;
}

namespace iouring_runtime::web {

bool HttpSession::HandleRequest(HttpRequest& request) {
    if (active_stream_.handler) {
        return CompleteStreamingRequest(request);
    }

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

    if (!response.IsSent() && !response.IsDeferred()) {
        response.Send();
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    obs::LogInfo(kLogCategory,
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

} // namespace iouring_runtime::web
