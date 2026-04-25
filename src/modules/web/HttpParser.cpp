#include <iouring_runtime/web/HttpParser.h>

#include <llhttp.h>

namespace iouring_runtime::web {

HttpParser::HttpParser()
    : HttpParser(HttpParserOptions{}) {}

HttpParser::HttpParser(HttpParserOptions options)
    : parser_(std::make_unique<llhttp_t>())
    , settings_(std::make_unique<llhttp_settings_t>())
    , options_(options) {
    llhttp_settings_init(settings_.get());

    settings_->on_message_begin = OnMessageBegin;
    settings_->on_url = OnUrl;
    settings_->on_url_complete = OnUrlComplete;
    settings_->on_header_field = OnHeaderField;
    settings_->on_header_field_complete = OnHeaderFieldComplete;
    settings_->on_header_value = OnHeaderValue;
    settings_->on_header_value_complete = OnHeaderValueComplete;
    settings_->on_headers_complete = OnHeadersComplete;
    settings_->on_body = OnBody;
    settings_->on_message_complete = OnMessageComplete;

    llhttp_init(parser_.get(), HTTP_REQUEST, settings_.get());
    parser_->data = this;
}

HttpParser::~HttpParser() = default;

std::uint32_t HttpParser::Feed(const char* data, std::uint32_t len) {
    const char* current = data;
    std::uint32_t remaining = len;

    while (remaining > 0 && status_ == HttpParseStatus::kOk) {
        const auto err = llhttp_execute(parser_.get(), current, remaining);

        if (err == HPE_OK) {
            return len;
        }

        if (err == HPE_PAUSED) {
            const char* error_pos = llhttp_get_error_pos(parser_.get());
            const auto consumed =
                static_cast<std::uint32_t>(error_pos - current);
            current += consumed;
            remaining -= consumed;

            if (stop_requested_) {
                return static_cast<std::uint32_t>(current - data);
            }

            llhttp_resume(parser_.get());
            continue;
        }

        if (status_ == HttpParseStatus::kOk) {
            status_ = HttpParseStatus::kBadRequest;
        }
        const char* error_pos = llhttp_get_error_pos(parser_.get());
        return static_cast<std::uint32_t>(error_pos - data);
    }

    return static_cast<std::uint32_t>(current - data);
}

bool HttpParser::HasError() const {
    return status_ != HttpParseStatus::kOk;
}

void HttpParser::Reset() {
    llhttp_init(parser_.get(), HTTP_REQUEST, settings_.get());
    parser_->data = this;
    request_.Clear();
    current_header_field_.clear();
    current_header_value_.clear();
    raw_url_.clear();
    status_ = HttpParseStatus::kOk;
    stop_requested_ = false;
    body_mode_ = HttpBodyMode::kBuffer;
    header_bytes_used_ = 0;
    body_bytes_seen_ = 0;
}

int HttpParser::OnMessageBegin(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->request_.Clear();
    self->request_.ReserveHeaders(16);
    self->current_header_field_.clear();
    self->current_header_value_.clear();
    self->raw_url_.clear();
    self->raw_url_.reserve(
        std::min<std::size_t>(self->options_.max_url_bytes, 1024));
    self->stop_requested_ = false;
    self->body_mode_ = HttpBodyMode::kBuffer;
    self->header_bytes_used_ = 0;
    self->body_bytes_seen_ = 0;
    return 0;
}

int HttpParser::OnUrl(llhttp_t* parser, const char* at, std::size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    if (self->raw_url_.size() + len > self->options_.max_url_bytes) {
        self->status_ = HttpParseStatus::kUrlTooLong;
        return HPE_USER;
    }
    self->raw_url_.append(at, len);
    return 0;
}

int HttpParser::OnUrlComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->ParseUrl(self->raw_url_);
    return 0;
}

int HttpParser::OnHeaderField(llhttp_t* parser, const char* at, std::size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    if (self->header_bytes_used_ + len > self->options_.max_header_bytes) {
        self->status_ = HttpParseStatus::kHeadersTooLarge;
        return HPE_USER;
    }
    self->header_bytes_used_ += static_cast<std::uint32_t>(len);
    self->current_header_field_.append(at, len);
    return 0;
}

int HttpParser::OnHeaderFieldComplete(llhttp_t* /*parser*/) {
    return 0;
}

int HttpParser::OnHeaderValue(llhttp_t* parser, const char* at, std::size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    if (self->header_bytes_used_ + len > self->options_.max_header_bytes) {
        self->status_ = HttpParseStatus::kHeadersTooLarge;
        return HPE_USER;
    }
    self->header_bytes_used_ += static_cast<std::uint32_t>(len);
    self->current_header_value_.append(at, len);
    return 0;
}

int HttpParser::OnHeaderValueComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->request_.AddHeader(std::move(self->current_header_field_),
                             std::move(self->current_header_value_));
    self->current_header_field_.clear();
    self->current_header_value_.clear();
    return 0;
}

int HttpParser::OnHeadersComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    self->request_.method = HttpMethodFromLlhttp(llhttp_get_method(parser));
    self->request_.keep_alive = llhttp_should_keep_alive(parser) != 0;
    if (self->on_headers_) {
        self->body_mode_ = self->on_headers_(self->request_);
    } else {
        self->body_mode_ = HttpBodyMode::kBuffer;
    }
    const auto content_length = self->request_.ContentLength();
    if (content_length > self->options_.max_body_bytes) {
        self->status_ = HttpParseStatus::kBodyTooLarge;
        return HPE_USER;
    }
    if (content_length > 0 && self->body_mode_ == HttpBodyMode::kBuffer) {
        self->request_.ReserveBody(static_cast<std::size_t>(content_length));
    }
    return 0;
}

int HttpParser::OnBody(llhttp_t* parser, const char* at, std::size_t len) {
    auto* self = static_cast<HttpParser*>(parser->data);
    if (self->body_bytes_seen_ + len > self->options_.max_body_bytes) {
        self->status_ = HttpParseStatus::kBodyTooLarge;
        return HPE_USER;
    }
    self->body_bytes_seen_ += static_cast<std::uint32_t>(len);
    if (self->body_mode_ == HttpBodyMode::kBuffer) {
        self->request_.body.append(at, len);
    } else if (self->body_mode_ == HttpBodyMode::kStream && self->on_body_chunk_) {
        const auto* bytes = reinterpret_cast<const std::byte*>(at);
        if (!self->on_body_chunk_(self->request_, {bytes, len})) {
            self->status_ = HttpParseStatus::kBadRequest;
            return HPE_USER;
        }
    }
    return 0;
}

int HttpParser::OnMessageComplete(llhttp_t* parser) {
    auto* self = static_cast<HttpParser*>(parser->data);
    if (self->on_request_) {
        const bool should_continue = self->on_request_(self->request_);
        if (!should_continue) {
            self->stop_requested_ = true;
        }
    }
    return HPE_PAUSED;
}

void HttpParser::ParseUrl(const std::string& url) {
    const auto query_pos = url.find('?');
    if (query_pos == std::string::npos) {
        request_.path = url;
        request_.query.clear();
        return;
    }
    request_.path = url.substr(0, query_pos);
    request_.query = url.substr(query_pos + 1);
}

} // namespace iouring_runtime::web
