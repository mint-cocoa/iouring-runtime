#include <iouring_runtime/web/HttpParser.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace iouring_runtime::web;

namespace {

constexpr HttpParserOptions kTinyLimits{
    .max_url_bytes = 64,
    .max_header_bytes = 64,
    .max_body_bytes = 32,
};

std::uint32_t Feed(HttpParser& parser, const std::string& data) {
    return parser.Feed(data.data(), static_cast<std::uint32_t>(data.size()));
}

} // namespace

TEST(HttpParserLimits, WithinLimitsParsesOk) {
    HttpParser parser{kTinyLimits};
    bool handler_fired = false;
    parser.SetOnRequest([&](HttpRequest&) {
        handler_fired = true;
        return true;
    });

    Feed(parser, "GET /short HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_FALSE(parser.HasError());
    EXPECT_EQ(parser.Status(), HttpParseStatus::kOk);
    EXPECT_TRUE(handler_fired);
}

TEST(HttpParserLimits, UrlExceedingMaxBytesReportsUriTooLong) {
    HttpParser parser{kTinyLimits};
    std::string path(100, 'a');
    Feed(parser, "GET /" + path + " HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_TRUE(parser.HasError());
    EXPECT_EQ(parser.Status(), HttpParseStatus::kUrlTooLong);
}

TEST(HttpParserLimits, HeadersExceedingTotalBytesReportsHeadersTooLarge) {
    HttpParser parser{kTinyLimits};
    Feed(parser,
         "GET / HTTP/1.1\r\n"
         "Host: x\r\n"
         "X-Pad-1: aaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
         "X-Pad-2: bbbbbbbbbbbbbbbbbbbbbbbbbb\r\n"
         "X-Pad-3: cccccccccccccccccccccccccc\r\n"
         "\r\n");

    EXPECT_TRUE(parser.HasError());
    EXPECT_EQ(parser.Status(), HttpParseStatus::kHeadersTooLarge);
}

TEST(HttpParserLimits, BodyExceedingMaxBytesReportsPayloadTooLarge) {
    HttpParser parser{kTinyLimits};
    const std::string body(100, 'x');
    Feed(parser,
         "POST /upload HTTP/1.1\r\n"
         "Host: x\r\n"
         "Content-Length: 100\r\n"
         "\r\n" + body);

    EXPECT_TRUE(parser.HasError());
    EXPECT_EQ(parser.Status(), HttpParseStatus::kBodyTooLarge);
}

TEST(HttpParserLimits, ContentLengthOverLimitFailsBeforeBodyArrives) {
    HttpParser parser{kTinyLimits};
    bool handler_fired = false;
    parser.SetOnRequest([&](HttpRequest&) {
        handler_fired = true;
        return true;
    });

    Feed(parser,
         "POST /upload HTTP/1.1\r\n"
         "Host: x\r\n"
         "Content-Length: 100\r\n"
         "\r\n");

    EXPECT_TRUE(parser.HasError());
    EXPECT_EQ(parser.Status(), HttpParseStatus::kBodyTooLarge);
    EXPECT_FALSE(handler_fired);
}

TEST(HttpParserLimits, ChunkedRequestBodyIsDecodedIntoBody) {
    HttpParser parser{kTinyLimits};
    std::string captured_body;
    parser.SetOnRequest([&](HttpRequest& req) {
        captured_body = req.body;
        return true;
    });

    Feed(parser,
         "POST /upload HTTP/1.1\r\n"
         "Host: x\r\n"
         "Transfer-Encoding: chunked\r\n"
         "\r\n"
         "4\r\nWiki\r\n"
         "5\r\npedia\r\n"
         "0\r\n\r\n");

    EXPECT_FALSE(parser.HasError());
    EXPECT_EQ(captured_body, "Wikipedia");
}

TEST(HttpParserLimits, PipelinedRequestsWithBodiesBothDispatch) {
    HttpParser parser{kTinyLimits};
    std::vector<std::string> bodies;
    parser.SetOnRequest([&](HttpRequest& req) {
        bodies.push_back(req.body);
        return true;
    });

    Feed(parser,
         "POST /a HTTP/1.1\r\n"
         "Host: x\r\n"
         "Content-Length: 3\r\n"
         "\r\n"
         "one"
         "POST /b HTTP/1.1\r\n"
         "Host: x\r\n"
         "Content-Length: 3\r\n"
         "\r\n"
         "two");

    EXPECT_FALSE(parser.HasError());
    ASSERT_EQ(bodies.size(), 2u);
    EXPECT_EQ(bodies[0], "one");
    EXPECT_EQ(bodies[1], "two");
}
