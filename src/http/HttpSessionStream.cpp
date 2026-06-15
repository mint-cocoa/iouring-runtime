#include <iouring/http/HttpSession.h>

#include <iouring/http/HttpResponse.h>
#include <iouring/http/Router.h>

#include <iouring/observability/Logging.h>

namespace obs = iouring::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kHttp;
}

namespace iouring::http {

bool HttpSession::ActiveStreamResponseSent() const {
    return active_stream_.response && active_stream_.response->IsSent();
}

HttpBodyMode HttpSession::PrepareRequestBody(HttpRequest& request) {
    auto result = router_.Match(request.method, request.path);
    if (!result.stream_handler) {
        return HttpBodyMode::kBuffer;
    }

    request.request_id = GenerateRequestId();
    request.SetParams(std::move(result.params));
    active_stream_.handler = result.stream_handler;
    active_stream_.request = &request;
    active_stream_.response = std::make_unique<HttpResponse>(*this, Pool());
    active_stream_.response->KeepAlive(request.keep_alive);
    active_stream_.rejected = false;

    RequestContext ctx{
        .session = *this,
        .request = request,
        .response = *active_stream_.response,
        .pool = Pool(),
        .remote_addr = RemoteAddr(),
        .request_id = request.request_id,
    };

    try {
        if (active_stream_.handler->on_headers &&
            !active_stream_.handler->on_headers(ctx)) {
            active_stream_.rejected = true;
            if (!active_stream_.response->IsSent()) {
                active_stream_.response->Status(HttpStatus::kBadRequest)
                    .ContentType("text/plain")
                    .Body("Bad Request")
                    .Send();
            }
            return HttpBodyMode::kDiscard;
        }
    } catch (const std::exception& ex) {
        obs::LogError(kLogCategory, "HttpSession: stream headers exception for {} {}: {}",
                      HttpMethodToString(request.method), request.path, ex.what());
        active_stream_.response->Status(HttpStatus::kInternalServerError)
            .ContentType("text/plain")
            .Body("Internal Server Error")
            .Send();
        active_stream_.rejected = true;
        return HttpBodyMode::kDiscard;
    } catch (...) {
        obs::LogError(kLogCategory, "HttpSession: stream headers unknown exception for {} {}",
                      HttpMethodToString(request.method), request.path);
        active_stream_.response->Status(HttpStatus::kInternalServerError)
            .ContentType("text/plain")
            .Body("Internal Server Error")
            .Send();
        active_stream_.rejected = true;
        return HttpBodyMode::kDiscard;
    }

    return HttpBodyMode::kStream;
}

bool HttpSession::HandleBodyChunk(HttpRequest& request,
                                  std::span<const std::byte> chunk) {
    if (!active_stream_.handler) {
        return true;
    }

    RequestContext ctx{
        .session = *this,
        .request = request,
        .response = *active_stream_.response,
        .pool = Pool(),
        .remote_addr = RemoteAddr(),
        .request_id = request.request_id,
    };

    try {
        if (active_stream_.handler->on_body) {
            if (!active_stream_.handler->on_body(ctx, chunk)) {
                if (!active_stream_.response->IsSent()) {
                    active_stream_.response->Status(HttpStatus::kBadRequest)
                        .ContentType("text/plain")
                        .Body("Bad Request")
                        .Send();
                }
                return false;
            }
        }
        return true;
    } catch (const std::exception& ex) {
        obs::LogError(kLogCategory, "HttpSession: stream body exception for {} {}: {}",
                      HttpMethodToString(request.method), request.path, ex.what());
    } catch (...) {
        obs::LogError(kLogCategory, "HttpSession: stream body unknown exception for {} {}",
                      HttpMethodToString(request.method), request.path);
    }

    if (!active_stream_.response->IsSent()) {
        active_stream_.response->Status(HttpStatus::kInternalServerError)
            .ContentType("text/plain")
            .Body("Internal Server Error")
            .Send();
    }
    return false;
}

bool HttpSession::CompleteStreamingRequest(HttpRequest& request) {
    request_in_progress_ = false;
    const auto started_at = request_started_at_;

    RequestContext ctx{
        .session = *this,
        .request = request,
        .response = *active_stream_.response,
        .pool = Pool(),
        .remote_addr = RemoteAddr(),
        .request_id = request.request_id,
    };

    try {
        if (!active_stream_.rejected && active_stream_.handler->on_complete) {
            active_stream_.handler->on_complete(ctx);
        }
    } catch (const std::exception& ex) {
        obs::LogError(kLogCategory, "HttpSession: stream complete exception for {} {}: {}",
                      HttpMethodToString(request.method), request.path, ex.what());
        if (!active_stream_.response->IsSent() &&
            !active_stream_.response->IsDeferred()) {
            active_stream_.response->Status(HttpStatus::kInternalServerError)
                .ContentType("text/plain")
                .Body("Internal Server Error")
                .Send();
        }
    } catch (...) {
        obs::LogError(kLogCategory, "HttpSession: stream complete unknown exception for {} {}",
                      HttpMethodToString(request.method), request.path);
        if (!active_stream_.response->IsSent() &&
            !active_stream_.response->IsDeferred()) {
            active_stream_.response->Status(HttpStatus::kInternalServerError)
                .ContentType("text/plain")
                .Body("Internal Server Error")
                .Send();
        }
    }

    const bool deferred = active_stream_.response->IsDeferred();
    if (!active_stream_.response->IsSent() && !deferred) {
        active_stream_.response->Send();
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    obs::LogInfo(kLogCategory,
        "HttpSession: request_id={} remote={} method={} path={} status={} bytes={} duration_ms={}",
        request.request_id,
        ctx.remote_addr.empty() ? "-" : std::string(ctx.remote_addr),
        HttpMethodToString(request.method),
        request.path,
        static_cast<int>(active_stream_.response->StatusCode()),
        active_stream_.response->LastBuiltBytes(),
        elapsed.count());

    request.ClearParams();
    active_stream_ = {};

    if (deferred) {
        active_stream_ = {};
        return false;
    }

    if (!request.keep_alive) {
        DisconnectAfterFlush();
        return false;
    }

    return true;
}

void HttpSession::AbortStreamingRequest() {
    if (!active_stream_.handler || !active_stream_.request ||
        !active_stream_.response) {
        return;
    }

    RequestContext ctx{
        .session = *this,
        .request = *active_stream_.request,
        .response = *active_stream_.response,
        .pool = Pool(),
        .remote_addr = RemoteAddr(),
        .request_id = active_stream_.request->request_id,
    };

    try {
        if (active_stream_.handler->on_abort) {
            active_stream_.handler->on_abort(ctx);
        }
    } catch (const std::exception& ex) {
        obs::LogError(kLogCategory, "HttpSession: stream abort exception for {} {}: {}",
                      HttpMethodToString(active_stream_.request->method),
                      active_stream_.request->path, ex.what());
    } catch (...) {
        obs::LogError(kLogCategory, "HttpSession: stream abort unknown exception for {} {}",
                      HttpMethodToString(active_stream_.request->method),
                      active_stream_.request->path);
    }

    active_stream_.request->ClearParams();
    active_stream_ = {};
}

} // namespace iouring::http
