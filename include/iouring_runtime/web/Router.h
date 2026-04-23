#pragma once

#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/web/HttpRequest.h>
#include <iouring_runtime/web/HttpResponse.h>
#include <iouring_runtime/web/HttpSession.h>
#include <iouring_runtime/web/HttpStatus.h>
#include <iouring_runtime/web/RadixTree.h>

#include <string_view>

namespace iouring_runtime::web {

struct RequestContext {
    HttpSession& session;
    HttpRequest& request;
    HttpResponse& response;
    core::buffer::BufferPool& pool;
    std::string_view remote_addr;
    std::string_view request_id;
};

class Router {
public:
    void Route(HttpMethod method, std::string path, HttpHandler handler);
    void Dispatch(RequestContext& ctx) const;

private:
    RadixTree tree_;
};

} // namespace iouring_runtime::web
