#include <iouring_runtime/web/Router.h>

#include <spdlog/spdlog.h>

#include <exception>

namespace iouring_runtime::web {

void Router::Route(HttpMethod method, std::string path, HttpHandler handler) {
    tree_.Insert(method, path, std::move(handler));
}

void Router::Dispatch(RequestContext& ctx) const {
    auto result = tree_.Match(ctx.request.method, ctx.request.path);
    if (ctx.request.method == HttpMethod::kHead) {
        ctx.response.SuppressBody();
        if (!result.handler) {
            auto get_result = tree_.Match(HttpMethod::kGet, ctx.request.path);
            if (get_result.handler) {
                result = std::move(get_result);
            }
        }
    }

    if (result.handler) {
        ctx.request.SetParams(std::move(result.params));
        try {
            (*result.handler)(ctx);
        } catch (const std::exception& ex) {
            spdlog::error("Router: handler exception for {} {}: {}",
                          HttpMethodToString(ctx.request.method),
                          ctx.request.path, ex.what());
            if (!ctx.response.IsSent()) {
                ctx.response
                    .Status(HttpStatus::kInternalServerError)
                    .ContentType("text/plain")
                    .Body("Internal Server Error")
                    .Send();
            }
        } catch (...) {
            spdlog::error("Router: unknown handler exception for {} {}",
                          HttpMethodToString(ctx.request.method),
                          ctx.request.path);
            if (!ctx.response.IsSent()) {
                ctx.response
                    .Status(HttpStatus::kInternalServerError)
                    .ContentType("text/plain")
                    .Body("Internal Server Error")
                    .Send();
            }
        }
        ctx.request.ClearParams();
        return;
    }

    if (result.path_exists) {
        ctx.response
            .Status(HttpStatus::kMethodNotAllowed)
            .ContentType("text/plain")
            .Body("Method Not Allowed")
            .Send();
        return;
    }

    ctx.response
        .Status(HttpStatus::kNotFound)
        .ContentType("text/plain")
        .Body("Not Found")
        .Send();
}

} // namespace iouring_runtime::web
