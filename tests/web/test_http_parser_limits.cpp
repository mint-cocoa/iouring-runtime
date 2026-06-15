#include <iouring/http/HttpParser.h>

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <vector>

using namespace iouring::http;

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

TEST(HttpParserLimits, HeaderCallbackCanDiscardBodyWithoutBuffering) {
    HttpParser parser{kTinyLimits};
    std::string captured_body = "unset";
    std::uint64_t captured_length = 0;
    parser.SetOnHeaders([](HttpRequest& req) {
        return req.path == "/upload" ? HttpBodyMode::kDiscard : HttpBodyMode::kBuffer;
    });
    parser.SetOnRequest([&](HttpRequest& req) {
        captured_body = req.body;
        captured_length = req.ContentLength();
        return true;
    });

    Feed(parser,
         "POST /upload HTTP/1.1\r\n"
         "Host: x\r\n"
         "Content-Length: 16\r\n"
         "\r\n"
         "0123456789abcdef");

    EXPECT_FALSE(parser.HasError());
    EXPECT_TRUE(captured_body.empty());
    EXPECT_EQ(captured_length, 16u);
}

TEST(HttpParserLimits, StreamBodyPathDeliversChunksWithoutBuffering) {
    HttpParser parser{kTinyLimits};
    std::string streamed;
    std::string completed_body = "unset";
    parser.SetOnHeaders([](HttpRequest& req) {
        return req.path == "/upload" ? HttpBodyMode::kStream : HttpBodyMode::kBuffer;
    });
    parser.SetOnBodyChunk([&](HttpRequest&, std::span<const std::byte> chunk) {
        streamed.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        return true;
    });
    parser.SetOnRequest([&](HttpRequest& req) {
        completed_body = req.body;
        return true;
    });

    Feed(parser,
         "POST /upload HTTP/1.1\r\n"
         "Host: x\r\n"
         "Content-Length: 11\r\n"
         "\r\n"
         "hello world");

    EXPECT_FALSE(parser.HasError());
    EXPECT_EQ(streamed, "hello world");
    EXPECT_TRUE(completed_body.empty());
}

TEST(HttpParserLimits, DiscardBodyModeStillEnforcesMaxBodyBytes) {
    HttpParser parser{kTinyLimits};
    bool handler_fired = false;
    parser.SetOnHeaders([](HttpRequest&) {
        return HttpBodyMode::kDiscard;
    });
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
