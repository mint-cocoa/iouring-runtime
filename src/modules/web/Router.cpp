#include <iouring_runtime/web/Router.h>

#include <iouring_runtime/web/RadixTree.h>

#include <iouring_runtime/observability/Logging.h>

#include <exception>
#include <utility>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kRouter;
}

namespace iouring_runtime::web {

Router::Router()
    : tree_(std::make_unique<RadixTree>()) {}

Router::~Router() = default;
Router::Router(Router&&) noexcept = default;
Router& Router::operator=(Router&&) noexcept = default;

void Router::Route(HttpMethod method, std::string path, HttpHandler handler) {
    RouteEntry entry;
    entry.handler = std::move(handler);
    tree_->Insert(method, path, std::move(entry));
}

void Router::RouteStream(HttpMethod method, std::string path,
                         HttpStreamHandler handler) {
    RouteEntry entry;
    entry.stream_handler = std::move(handler);
    tree_->Insert(method, path, std::move(entry));
}

Router::MatchResult Router::Match(HttpMethod method, std::string_view path) const {
    auto match = tree_->Match(method, path);
    MatchResult result;
    if (match.entry) {
        if (match.entry->handler) {
            result.handler = &match.entry->handler;
        }
        if (match.entry->HasStreamHandler()) {
            result.stream_handler = &match.entry->stream_handler;
        }
        result.params = std::move(match.params);
    }
    result.path_exists = match.path_exists;
    return result;
}

void Router::Dispatch(RequestContext& ctx) const {
    auto result = Match(ctx.request.method, ctx.request.path);
    if (ctx.request.method == HttpMethod::kHead) {
        ctx.response.SuppressBody();
        if (!result.handler) {
            auto get_result = Match(HttpMethod::kGet, ctx.request.path);
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
            obs::LogError(kLogCategory, "Router: handler exception for {} {}: {}",
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
            obs::LogError(kLogCategory, "Router: unknown handler exception for {} {}",
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
