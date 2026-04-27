#include <iouring_runtime/web/Router.h>
#include <iouring_runtime/web/HttpSession.h>
#include <iouring_runtime/web/RadixTree.h>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

bool LineStartsWith(std::string_view line, std::string_view name) {
    if (line.size() < name.size() + 1 || line[name.size()] != ':') {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        char a = line[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::string FirstHeaderValue(std::string_view payload, std::string_view name) {
    const auto status_end = payload.find("\r\n");
    if (status_end == std::string_view::npos) {
        return {};
    }

    std::size_t pos = status_end + 2;
    while (true) {
        const auto line_end = payload.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            return {};
        }
        const auto line = payload.substr(pos, line_end - pos);
        if (line.empty()) {
            return {};
        }
        if (LineStartsWith(line, name)) {
            auto value = line.substr(name.size() + 1);
            while (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
            return std::string(value);
        }
        pos = line_end + 2;
    }
}

} // namespace

class RouterTest : public ::testing::Test {
protected:
    Router router_;
    bool handler_called_ = false;

    void SetUp() override {
        router_.Use([](RequestContext& ctx) {
            ctx.response.Header("X-Middleware", "ran");
        });
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

TEST_F(RouterTest, ParamDecodedDecodesPercentEscapes) {
    router_.Route(HttpMethod::kGet, "/people/:name", [](RequestContext& ctx) {
        ctx.response.Text(ctx.request.ParamDecoded("name"));
    });

    HttpRequest request;
    request.method = HttpMethod::kGet;
    request.path = "/people/Jane%20Doe";

    HttpResponse response;
    iouring_runtime::core::buffer::BufferPool pool;
    auto ctx = MakeContext(request, response, pool);

    router_.Dispatch(ctx);

    EXPECT_EQ(response.GetBody(), "Jane Doe");
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

    const auto out = Serialize(response, pool);
    EXPECT_EQ(FirstHeaderValue(out, "Allow"), "GET, HEAD");
}

TEST_F(RouterTest, MiddlewareCanAnnotateResponseBeforeHandler) {
    HttpRequest request;
    request.method = HttpMethod::kGet;
    request.path = "/users/42";

    HttpResponse response;
    iouring_runtime::core::buffer::BufferPool pool;
    auto ctx = MakeContext(request, response, pool);

    router_.Dispatch(ctx);

    const auto out = Serialize(response, pool);
    EXPECT_EQ(FirstHeaderValue(out, "X-Middleware"), "ran");
}

TEST(RouterMiddleware, MiddlewareCanShortCircuitDispatch) {
    Router router;
    router.Use([](RequestContext& ctx) {
        ctx.response
            .Status(HttpStatus::kUnauthorized)
            .Text("blocked")
            .Send();
    });

    bool handler_called = false;
    router.Route(HttpMethod::kGet, "/private", [&](RequestContext&) {
        handler_called = true;
    });

    HttpRequest request;
    request.method = HttpMethod::kGet;
    request.path = "/private";

    HttpResponse response;
    iouring_runtime::core::buffer::BufferPool pool;
    auto ctx = MakeContext(request, response, pool);

    router.Dispatch(ctx);

    EXPECT_FALSE(handler_called);
    EXPECT_TRUE(response.IsSent());
    EXPECT_EQ(response.StatusCode(), HttpStatus::kUnauthorized);
    EXPECT_EQ(response.GetBody(), "blocked");
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

TEST(RouterStreamTest, MatchesStreamRouteWithParams) {
    RadixTree tree;
    RouteEntry entry;
    entry.stream_handler.on_headers = [](RequestContext&) { return true; };
    tree.Insert(HttpMethod::kPut, "/files/*path", std::move(entry));

    auto result = tree.Match(HttpMethod::kPut, "/files/a/b.txt");

    ASSERT_NE(result.entry, nullptr);
    EXPECT_TRUE(result.entry->HasStreamHandler());
    ASSERT_EQ(result.params.size(), 1u);
    EXPECT_EQ(result.params[0].first, "path");
    EXPECT_EQ(result.params[0].second, "a/b.txt");
    EXPECT_TRUE(result.path_exists);
}

TEST(HttpSessionIds, GeneratedRequestIdsAreUnique) {
    const auto first = HttpSession::GenerateRequestId();
    const auto second = HttpSession::GenerateRequestId();

    EXPECT_NE(first, second);
}
