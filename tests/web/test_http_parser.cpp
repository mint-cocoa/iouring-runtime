#include <iouring_runtime/web/HttpParser.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace iouring_runtime::web;

class HttpParserTest : public ::testing::Test {
protected:
    HttpParser parser_;
    std::vector<HttpRequest> requests_;

    void SetUp() override {
        parser_.SetOnRequest([this](HttpRequest& req) {
            HttpRequest copy;
            copy.method = req.method;
            copy.path = req.path;
            copy.query = req.query;
            copy.body = req.body;
            copy.keep_alive = req.keep_alive;
            for (const auto& header : req.headers()) {
                copy.AddHeader(header.name, header.value);
            }
            requests_.push_back(std::move(copy));
            return true;
        });
    }

    std::uint32_t Feed(const std::string& data) {
        return parser_.Feed(data.data(), static_cast<std::uint32_t>(data.size()));
    }
};

TEST_F(HttpParserTest, SimpleGet) {
    Feed("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

    ASSERT_EQ(requests_.size(), 1u);
    EXPECT_EQ(requests_[0].method, HttpMethod::kGet);
    EXPECT_EQ(requests_[0].path, "/");
    EXPECT_TRUE(requests_[0].query.empty());
    EXPECT_TRUE(requests_[0].body.empty());
    EXPECT_TRUE(requests_[0].keep_alive);
    EXPECT_EQ(requests_[0].GetHeader("Host"), "localhost");
}

TEST_F(HttpParserTest, QueryString) {
    Feed("GET /search?q=hello&page=1 HTTP/1.1\r\nHost: localhost\r\n\r\n");

    ASSERT_EQ(requests_.size(), 1u);
    EXPECT_EQ(requests_[0].path, "/search");
    EXPECT_EQ(requests_[0].query, "q=hello&page=1");
}

TEST_F(HttpParserTest, QueryHelpersDecodePercentEscapesAndPlus) {
    Feed("GET /search?q=hello%20world&lang=ko+KR HTTP/1.1\r\nHost: localhost\r\n\r\n");

    ASSERT_EQ(requests_.size(), 1u);
    EXPECT_EQ(requests_[0].QueryParamDecoded("q"), "hello world");
    EXPECT_EQ(requests_[0].QueryParamDecoded("lang"), "ko KR");
}

TEST_F(HttpParserTest, PostWithBody) {
    Feed(
        "POST /api HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"key\":\"val\"}");

    ASSERT_EQ(requests_.size(), 1u);
    EXPECT_EQ(requests_[0].method, HttpMethod::kPost);
    EXPECT_EQ(requests_[0].path, "/api");
    EXPECT_EQ(requests_[0].body, R"({"key":"val"})");
    EXPECT_EQ(requests_[0].GetHeader("Content-Type"), "application/json");
}

TEST_F(HttpParserTest, ChunkedDelivery) {
    const std::string raw = "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n";
    for (const char ch : raw) {
        parser_.Feed(&ch, 1);
    }

    ASSERT_EQ(requests_.size(), 1u);
    EXPECT_EQ(requests_[0].path, "/test");
}

TEST_F(HttpParserTest, Pipelining) {
    Feed(
        "GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /third HTTP/1.1\r\nHost: localhost\r\n\r\n");

    ASSERT_EQ(requests_.size(), 3u);
    EXPECT_EQ(requests_[0].path, "/first");
    EXPECT_EQ(requests_[1].path, "/second");
    EXPECT_EQ(requests_[2].path, "/third");
}

TEST(HttpParserCallback, StopRequestIsNotParseError) {
    HttpParser parser;
    std::vector<std::string> paths;
    parser.SetOnRequest([&](HttpRequest& req) {
        paths.push_back(req.path);
        return false;
    });

    const std::string raw =
        "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: localhost\r\n\r\n";

    const auto consumed = parser.Feed(raw.data(), static_cast<std::uint32_t>(raw.size()));

    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], "/first");
    EXPECT_FALSE(parser.HasError());
    EXPECT_LT(consumed, raw.size());
}

TEST_F(HttpParserTest, InvalidRequest) {
    Feed("INVALID GARBAGE\r\n\r\n");

    EXPECT_TRUE(parser_.HasError());
    EXPECT_EQ(requests_.size(), 0u);
}

TEST_F(HttpParserTest, ConnectionClose) {
    Feed(
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n");

    ASSERT_EQ(requests_.size(), 1u);
    EXPECT_FALSE(requests_[0].keep_alive);
    EXPECT_FALSE(parser_.HasError());
}

TEST_F(HttpParserTest, CookieHelpersExposeNamedCookieValues) {
    Feed(
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Cookie: theme=light; session=abc123; encoded=hello%20world\r\n"
        "\r\n");

    ASSERT_EQ(requests_.size(), 1u);
    EXPECT_EQ(requests_[0].Cookie("theme"), "light");
    EXPECT_EQ(requests_[0].Cookie("session"), "abc123");
    EXPECT_EQ(requests_[0].CookieDecoded("encoded"), "hello world");
    EXPECT_TRUE(requests_[0].HasCookie("theme"));
    EXPECT_FALSE(requests_[0].HasCookie("missing"));
}

TEST_F(HttpParserTest, ResetAfterError) {
    Feed("INVALID GARBAGE\r\n\r\n");
    EXPECT_TRUE(parser_.HasError());

    parser_.Reset();
    EXPECT_FALSE(parser_.HasError());

    Feed("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    ASSERT_EQ(requests_.size(), 1u);
    EXPECT_EQ(requests_[0].path, "/");
}
