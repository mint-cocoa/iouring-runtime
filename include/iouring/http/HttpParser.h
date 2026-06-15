#pragma once

#include <iouring/http/HttpRequest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

struct llhttp__internal_s;
typedef struct llhttp__internal_s llhttp_t;
struct llhttp_settings_s;
typedef struct llhttp_settings_s llhttp_settings_t;

namespace iouring::http {

using OnRequestCallback = std::function<bool(HttpRequest&)>;

enum class HttpBodyMode : std::uint8_t {
    kBuffer,
    kDiscard,
    kStream,
};

using OnHeadersCallback = std::function<HttpBodyMode(HttpRequest&)>;
using OnBodyChunkCallback =
    std::function<bool(HttpRequest&, std::span<const std::byte>)>;

enum class HttpParseStatus : std::uint8_t {
    kOk,
    kBadRequest,
    kUrlTooLong,
    kHeadersTooLarge,
    kBodyTooLarge,
};

struct HttpParserOptions {
    std::uint32_t max_url_bytes = 8 * 1024;
    std::uint32_t max_header_bytes = 32 * 1024;
    std::uint32_t max_body_bytes = 10 * 1024 * 1024;
};

class HttpParser {
public:
    HttpParser();
    explicit HttpParser(HttpParserOptions options);
    ~HttpParser();

    HttpParser(const HttpParser&) = delete;
    HttpParser& operator=(const HttpParser&) = delete;

    void SetOnRequest(OnRequestCallback cb) {
        on_request_ = std::move(cb);
    }

    void SetOnHeaders(OnHeadersCallback cb) {
        on_headers_ = std::move(cb);
    }

    void SetOnBodyChunk(OnBodyChunkCallback cb) {
        on_body_chunk_ = std::move(cb);
    }

    std::uint32_t Feed(const char* data, std::uint32_t len);

    bool HasError() const;
    HttpParseStatus Status() const {
        return status_;
    }
    void Reset();

private:
    static int OnMessageBegin(llhttp_t* parser);
    static int OnUrl(llhttp_t* parser, const char* at, std::size_t len);
    static int OnUrlComplete(llhttp_t* parser);
    static int OnHeaderField(llhttp_t* parser, const char* at, std::size_t len);
    static int OnHeaderFieldComplete(llhttp_t* parser);
    static int OnHeaderValue(llhttp_t* parser, const char* at, std::size_t len);
    static int OnHeaderValueComplete(llhttp_t* parser);
    static int OnHeadersComplete(llhttp_t* parser);
    static int OnBody(llhttp_t* parser, const char* at, std::size_t len);
    static int OnMessageComplete(llhttp_t* parser);

    void ParseUrl(const std::string& url);

    std::unique_ptr<llhttp_t> parser_;
    std::unique_ptr<llhttp_settings_t> settings_;
    HttpRequest request_;
    OnRequestCallback on_request_;
    OnHeadersCallback on_headers_;
    OnBodyChunkCallback on_body_chunk_;
    HttpParserOptions options_;
    HttpParseStatus status_ = HttpParseStatus::kOk;
    bool stop_requested_ = false;
    HttpBodyMode body_mode_ = HttpBodyMode::kBuffer;
    std::uint32_t header_bytes_used_ = 0;
    std::uint64_t body_bytes_seen_ = 0;
    std::string current_header_field_;
    std::string current_header_value_;
    std::string raw_url_;
};

} // namespace iouring::http
