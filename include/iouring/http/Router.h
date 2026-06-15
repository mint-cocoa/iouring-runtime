#pragma once

#include <iouring/http/HttpRequest.h>
#include <iouring/http/HttpResponse.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iouring::http {

class HttpSession;
class RadixTree;

struct RequestContext;
using HttpHandler = std::function<void(RequestContext&)>;
using HttpMiddleware = std::function<void(RequestContext&)>;

struct HttpStreamHandler {
    std::function<bool(RequestContext&)> on_headers;
    std::function<bool(RequestContext&, std::span<const std::byte>)> on_body;
    std::function<void(RequestContext&)> on_complete;
    std::function<void(RequestContext&)> on_abort;
};

class DeferredResponse {
public:
    DeferredResponse() = default;
    ~DeferredResponse();

    DeferredResponse(const DeferredResponse&) = delete;
    DeferredResponse& operator=(const DeferredResponse&) = delete;
    DeferredResponse(DeferredResponse&& other) noexcept;
    DeferredResponse& operator=(DeferredResponse&& other) noexcept;

    HttpResponse& Response();
    void Complete();
    void Abort();
    explicit operator bool() const noexcept {
        return session_ != nullptr;
    }

private:
    friend struct RequestContext;

    DeferredResponse(HttpSession& session, core::buffer::BufferPool& pool);
    void Reset(bool send_if_open);

    HttpSession* session_ = nullptr;
    std::unique_ptr<HttpResponse> response_;
};

struct RequestContext {
    HttpSession& session;
    HttpRequest& request;
    HttpResponse& response;
    core::buffer::BufferPool& pool;
    std::string_view remote_addr;
    std::string_view request_id;

    DeferredResponse DeferResponse();
};

class Router {
public:
    Router();
    ~Router();
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&) noexcept;
    Router& operator=(Router&&) noexcept;

    void Route(HttpMethod method, std::string path, HttpHandler handler);
    void RouteStream(HttpMethod method, std::string path,
                     HttpStreamHandler handler);
    void Use(HttpMiddleware middleware);
    void Dispatch(RequestContext& ctx) const;

private:
    friend class HttpSession;

    struct MatchResult {
        HttpHandler* handler = nullptr;
        HttpStreamHandler* stream_handler = nullptr;
        std::vector<std::pair<std::string_view, std::string_view>> params;
        bool path_exists = false;
        std::string allow_header;
    };

    MatchResult Match(HttpMethod method, std::string_view path) const;

    std::vector<HttpMiddleware> middlewares_;
    std::unique_ptr<RadixTree> tree_;
};

} // namespace iouring::http
