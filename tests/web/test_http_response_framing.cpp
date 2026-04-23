#include <iouring_runtime/web/HttpResponse.h>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace iouring_runtime::web;
using iouring_runtime::core::buffer::BufferPool;

namespace {

std::string Serialize(const HttpResponse& response, BufferPool& pool) {
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

template <typename Fn>
void ForEachHeaderLine(std::string_view payload, Fn&& fn) {
    const auto status_end = payload.find("\r\n");
    if (status_end == std::string_view::npos) {
        return;
    }
    std::size_t pos = status_end + 2;
    while (true) {
        const auto line_end = payload.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            break;
        }
        const auto line = payload.substr(pos, line_end - pos);
        if (line.empty()) {
            break;
        }
        fn(line);
        pos = line_end + 2;
    }
}

std::size_t CountHeaderLines(std::string_view payload, std::string_view name) {
    std::size_t count = 0;
    ForEachHeaderLine(payload, [&](std::string_view line) {
        if (LineStartsWith(line, name)) {
            ++count;
        }
    });
    return count;
}

std::string FirstHeaderValue(std::string_view payload, std::string_view name) {
    std::string found;
    ForEachHeaderLine(payload, [&](std::string_view line) {
        if (!found.empty() || !LineStartsWith(line, name)) {
            return;
        }
        auto value = line.substr(name.size() + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        found = std::string(value);
    });
    return found;
}

std::string_view BodyBytes(std::string_view payload) {
    const auto marker = payload.find("\r\n\r\n");
    if (marker == std::string_view::npos) {
        return {};
    }
    return payload.substr(marker + 4);
}

} // namespace

class FramingFixture : public ::testing::Test {
protected:
    BufferPool pool;
};

TEST_F(FramingFixture, DefaultResponseHasOneFramingHeaderEach) {
    HttpResponse response;
    response.Status(HttpStatus::kOk).Body("hello");

    const auto out = Serialize(response, pool);

    EXPECT_EQ(CountHeaderLines(out, "Server"), 1u);
    EXPECT_EQ(CountHeaderLines(out, "Date"), 1u);
    EXPECT_EQ(CountHeaderLines(out, "Content-Length"), 1u);
    EXPECT_EQ(CountHeaderLines(out, "Connection"), 1u);
    EXPECT_EQ(FirstHeaderValue(out, "Content-Length"), "5");
    EXPECT_EQ(FirstHeaderValue(out, "Connection"), "keep-alive");
    EXPECT_EQ(response.LastBuiltBytes(), out.size());
}

TEST_F(FramingFixture, ExplicitContentLengthReplacesDefault) {
    HttpResponse response;
    response.Status(HttpStatus::kOk).Header("Content-Length", "4096");

    const auto out = Serialize(response, pool);

    EXPECT_EQ(CountHeaderLines(out, "Content-Length"), 1u);
    EXPECT_EQ(FirstHeaderValue(out, "Content-Length"), "4096");
}

TEST_F(FramingFixture, SuppressBodyKeepsComputedContentLength) {
    HttpResponse response;
    response.Status(HttpStatus::kOk)
        .ContentType("text/plain")
        .Body("hello")
        .SuppressBody();

    const auto out = Serialize(response, pool);

    EXPECT_EQ(FirstHeaderValue(out, "Content-Length"), "5");
    EXPECT_TRUE(BodyBytes(out).empty());
}

TEST_F(FramingFixture, RedirectSetsLocationAndEmptyBody) {
    HttpResponse response;
    response.Body("stale").Header("Location", "/old").Redirect("/login");

    const auto out = Serialize(response, pool);

    EXPECT_EQ(response.StatusCode(), HttpStatus::kFound);
    EXPECT_EQ(CountHeaderLines(out, "Location"), 1u);
    EXPECT_EQ(FirstHeaderValue(out, "Location"), "/login");
    EXPECT_EQ(FirstHeaderValue(out, "Content-Length"), "0");
    EXPECT_TRUE(BodyBytes(out).empty());
}

TEST_F(FramingFixture, JsonSetsContentType) {
    HttpResponse response;
    response.Json(R"({"ok":true})");

    const auto out = Serialize(response, pool);

    EXPECT_EQ(FirstHeaderValue(out, "Content-Type"), "application/json");
    EXPECT_EQ(BodyBytes(out), R"({"ok":true})");
}

TEST_F(FramingFixture, NoContentSuppressesBodyAndContentLength) {
    HttpResponse response;
    response.Status(HttpStatus::kNoContent)
        .ContentType("text/plain")
        .Body("must not be serialized");

    const auto out = Serialize(response, pool);

    EXPECT_EQ(CountHeaderLines(out, "Content-Length"), 0u);
    EXPECT_TRUE(BodyBytes(out).empty());
}
