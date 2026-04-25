#include <iouring_runtime/web/HttpResponse.h>

#include <iouring_runtime/web/HttpSession.h>

#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

namespace iouring_runtime::web {

namespace {

void AppendDateHeader(std::string& out) {
    out += "Date: ";
    const std::time_t now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT",
                  std::gmtime(&now));
    out += buf;
    out += "\r\n";
}

bool HeaderNameEquals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        char a = lhs[i];
        char b = rhs[i];
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

bool StatusMustNotHaveBody(HttpStatus status) {
    return status == HttpStatus::kNoContent ||
           status == HttpStatus::kNotModified;
}

} // namespace

HttpResponse::HttpResponse(HttpSession& session, core::buffer::BufferPool& pool)
    : session_(&session)
    , pool_(&pool) {}

bool HttpResponse::HasHeader(std::string_view name) const {
    if (HeaderNameEquals(name, "Content-Type") && !content_type_.empty()) {
        return true;
    }
    for (const auto& header : headers_) {
        if (HeaderNameEquals(header.name, name)) {
            return true;
        }
    }
    return false;
}

HttpResponse& HttpResponse::RemoveHeader(std::string_view name) {
    if (HeaderNameEquals(name, "Content-Type")) {
        content_type_.clear();
    }
    for (auto it = headers_.begin(); it != headers_.end();) {
        if (HeaderNameEquals(it->name, name)) {
            it = headers_.erase(it);
        } else {
            ++it;
        }
    }
    return *this;
}

HttpResponse& HttpResponse::Redirect(std::string url, HttpStatus status) {
    status_ = status;
    body_.clear();
    RemoveHeader("Location");
    Header("Location", std::move(url));
    return *this;
}

core::buffer::SendBufferRef HttpResponse::Build(
    core::buffer::BufferPool& pool) const {
    std::string headers;
    headers.reserve(256);

    const bool status_no_body = StatusMustNotHaveBody(status_);
    const bool no_body = status_no_body || suppress_body_;

    headers += "HTTP/1.1 ";
    headers += std::to_string(static_cast<int>(status_));
    headers += ' ';
    headers += HttpStatusToString(status_);
    headers += "\r\n";

    bool caller_has_server = false;
    bool caller_has_date = false;
    bool caller_has_content_type = false;
    bool caller_has_content_length = false;
    bool caller_has_connection = false;
    for (const auto& header : headers_) {
        if (HeaderNameEquals(header.name, "Server")) {
            caller_has_server = true;
        } else if (HeaderNameEquals(header.name, "Date")) {
            caller_has_date = true;
        } else if (HeaderNameEquals(header.name, "Content-Type")) {
            caller_has_content_type = true;
        } else if (HeaderNameEquals(header.name, "Content-Length")) {
            caller_has_content_length = true;
        } else if (HeaderNameEquals(header.name, "Connection")) {
            caller_has_connection = true;
        }
    }

    if (!caller_has_server) {
        headers += "Server: iouring_runtime_web\r\n";
    }
    if (!caller_has_date) {
        AppendDateHeader(headers);
    }
    if (!caller_has_content_type && !content_type_.empty()) {
        headers += "Content-Type: ";
        headers += content_type_;
        headers += "\r\n";
    }
    if (!caller_has_content_length && !status_no_body) {
        headers += "Content-Length: ";
        headers += std::to_string(body_.size());
        headers += "\r\n";
    }
    if (!caller_has_connection) {
        headers += "Connection: ";
        headers += keep_alive_ ? "keep-alive" : "close";
        headers += "\r\n";
    }

    for (const auto& header : headers_) {
        if (status_no_body && HeaderNameEquals(header.name, "Content-Length")) {
            continue;
        }
        headers += header.name;
        headers += ": ";
        headers += header.value;
        headers += "\r\n";
    }

    headers += "\r\n";

    const auto total_size = headers.size() + (no_body ? 0 : body_.size());
    auto result = pool.Allocate(static_cast<std::uint32_t>(total_size));
    if (!result) {
        return nullptr;
    }

    auto buffer = std::move(*result);
    auto writable = buffer->Writable();
    std::memcpy(writable.data(), headers.data(), headers.size());
    if (!no_body) {
        std::memcpy(writable.data() + headers.size(), body_.data(), body_.size());
    }
    buffer->Commit(static_cast<std::uint32_t>(total_size));
    last_built_bytes_ = total_size;
    return buffer;
}

void HttpResponse::Send() {
    if (state_ == State::kSent) {
        return;
    }
    state_ = State::kSent;
    if (!session_ || !pool_) {
        return;
    }
    auto buffer = Build(*pool_);
    if (buffer) {
        session_->SendResponse(std::move(buffer));
    }
}

core::buffer::SendBufferRef HttpResponse::NoContent(
    core::buffer::BufferPool& pool, bool keep_alive) {
    return HttpResponse()
        .Status(HttpStatus::kNoContent)
        .KeepAlive(keep_alive)
        .Build(pool);
}

} // namespace iouring_runtime::web
