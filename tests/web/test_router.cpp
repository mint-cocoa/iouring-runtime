#include <iouring_runtime/web/Router.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

using namespace iouring_runtime::web;

namespace {

RequestContext MakeContext(HttpRequest& request, HttpResponse& response,
                           iouring_runtime::core::buffer::BufferPool& pool) {
    auto* session = reinterpret_cast<HttpSession*>(0x1);
    return RequestContext{
        .session = *session,
        .request = request,
        .response = response,
        .pool = pool,
        .remote_addr = {},
        .request_id = request.request_id,
    };
}

std::string Serialize(const HttpResponse& response,
                      iouring_runtime::core::buffer::BufferPool& pool) {
    auto buffer = response.Build(pool);
    if (!buffer) {
        return {};
    }
    const auto data = buffer->Data();
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

} // namespace

class RouterTest : public ::testing::Test {
protected:
    Router router_;
    bool handler_called_ = false;

    void SetUp() override {
        router_.Route(HttpMethod::kGet, "/", [this](RequestContext&) {
            handler_called_ = true;
        });
        router_.Route(HttpMethod::kGet, "/users/:id", [](RequestContext& ctx) {
            ctx.response.ContentType("text/plain")
                .Body(std::string(ctx.request.Param("id")));
        });
        router_.Route(HttpMethod::kGet, "/throw", [this](RequestContext&) {
            handler_called_ = true;
            throw std::runtime_error("boom");
        });
    }
};

TEST_F(RouterTest, ExactMatchGet) {
    HttpRequest request;
    request.method = HttpMethod::kGet;
    request.path = "/";

    HttpResponse response;
    iouring_runtime::core::buffer::BufferPool pool;
    auto ctx = MakeContext(request, response, pool);

    router_.Dispatch(ctx);

    EXPECT_TRUE(handler_called_);
    EXPECT_FALSE(response.IsSent());
}

TEST_F(RouterTest, PathParamsAreExposedToHandlers) {
    HttpRequest request;
    request.method = HttpMethod::kGet;
    request.path = "/users/42";

    HttpResponse response;
    iouring_runtime::core::buffer::BufferPool pool;
    auto ctx = MakeContext(request, response, pool);

    router_.Dispatch(ctx);

    EXPECT_EQ(response.GetBody(), "42");
}

TEST_F(RouterTest, HeadFallsBackToGetAndSuppressesBody) {
    HttpRequest request;
    request.method = HttpMethod::kHead;
    request.path = "/users/42";

    HttpResponse response;
    iouring_runtime::core::buffer::BufferPool pool;
    auto ctx = MakeContext(request, response, pool);

    router_.Dispatch(ctx);

    const auto out = Serialize(response, pool);
    EXPECT_NE(out.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(out.find("Content-Length: 2\r\n"), std::string::npos);
    EXPECT_TRUE(out.ends_with("\r\n\r\n"));
}

TEST_F(RouterTest, UnknownPathReturns404) {
    HttpRequest request;
    request.method = HttpMethod::kGet;
    request.path = "/missing";

    HttpResponse response;
    iouring_runtime::core::buffer::BufferPool pool;
    auto ctx = MakeContext(request, response, pool);

    router_.Dispatch(ctx);

    EXPECT_TRUE(response.IsSent());
    EXPECT_EQ(response.StatusCode(), HttpStatus::kNotFound);
}

TEST_F(RouterTest, WrongMethodReturns405) {
    HttpRequest request;
    request.method = HttpMethod::kPost;
    request.path = "/users/42";

    HttpResponse response;
    iouring_runtime::core::buffer::BufferPool pool;
    auto ctx = MakeContext(request, response, pool);

    router_.Dispatch(ctx);

    EXPECT_TRUE(response.IsSent());
    EXPECT_EQ(response.StatusCode(), HttpStatus::kMethodNotAllowed);
}

TEST_F(RouterTest, HandlerExceptionReturns500) {
    HttpRequest request;
    request.method = HttpMethod::kGet;
    request.path = "/throw";

    HttpResponse response;
    iouring_runtime::core::buffer::BufferPool pool;
    auto ctx = MakeContext(request, response, pool);

    router_.Dispatch(ctx);

    EXPECT_TRUE(handler_called_);
    EXPECT_TRUE(response.IsSent());
    EXPECT_EQ(response.StatusCode(), HttpStatus::kInternalServerError);
    EXPECT_EQ(response.GetBody(), "Internal Server Error");
}
