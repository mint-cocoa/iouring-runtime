#include <iouring/http/HttpResponse.h>

#include <iouring/http/HttpSession.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

namespace iouring::http {

namespace {

constexpr std::size_t kInlineResponseMaxBytes = 256 * 1024;
constexpr std::uint32_t kBodyStreamChunkSize = 256 * 1024;
constexpr std::uint32_t kBodyStreamMaxChunksPerWrite = 4;

std::string_view CurrentHttpDate() {
    const std::time_t now = std::time(nullptr);
    thread_local std::time_t cached_time = 0;
    thread_local char cached_value[32] = {};
    if (now != cached_time) {
        cached_time = now;
        std::tm tm{};
        gmtime_r(&now, &tm);
        std::strftime(cached_value, sizeof(cached_value),
                      "%a, %d %b %Y %H:%M:%S GMT", &tm);
    }
    return cached_value;
}

void AppendDateHeader(std::string& out) {
    out += "Date: ";
    out += CurrentHttpDate();
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

core::buffer::SendBufferRef BufferFromBytes(core::buffer::BufferPool& pool,
                                            std::string_view bytes) {
    auto result = pool.Allocate(static_cast<std::uint32_t>(bytes.size()));
    if (!result) {
        return nullptr;
    }
    auto buffer = std::move(*result);
    auto writable = buffer->Writable();
    std::memcpy(writable.data(), bytes.data(), bytes.size());
    buffer->Commit(static_cast<std::uint32_t>(bytes.size()));
    return buffer;
}

} // namespace

HttpResponse::HttpResponse(HttpSession& session, core::buffer::BufferPool& pool)
    : session_(&session)
    , pool_(&pool) {}

HttpResponse& HttpResponse::Status(HttpStatus status) {
    status_ = status;
    return *this;
}

HttpResponse& HttpResponse::Header(std::string name, std::string value) {
    headers_.push_back({std::move(name), std::move(value)});
    return *this;
}

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

HttpResponse& HttpResponse::ContentType(std::string_view type) {
    content_type_ = type;
    return *this;
}

HttpResponse& HttpResponse::Body(std::string body) {
    body_ = std::move(body);
    return *this;
}

HttpResponse& HttpResponse::Text(std::string text) {
    content_type_ = "text/plain";
    body_ = std::move(text);
    return *this;
}

HttpResponse& HttpResponse::Json(std::string json_body) {
    content_type_ = "application/json";
    body_ = std::move(json_body);
    return *this;
}

HttpResponse& HttpResponse::Error(HttpStatus status, std::string message) {
    status_ = status;
    content_type_ = "text/plain";
    body_ = std::move(message);
    return *this;
}

HttpResponse& HttpResponse::KeepAlive(bool keep_alive) {
    keep_alive_ = keep_alive;
    return *this;
}

HttpResponse& HttpResponse::SuppressBody(bool suppress) {
    suppress_body_ = suppress;
    return *this;
}

HttpResponse& HttpResponse::Redirect(std::string url, HttpStatus status) {
    status_ = status;
    body_.clear();
    RemoveHeader("Location");
    Header("Location", std::move(url));
    return *this;
}

HttpStatus HttpResponse::StatusCode() const {
    return status_;
}

const std::string& HttpResponse::GetBody() const {
    return body_;
}

bool HttpResponse::IsSent() const {
    return state_ == State::kSent;
}

bool HttpResponse::IsDeferred() const {
    return state_ == State::kDeferred;
}

bool HttpResponse::GetKeepAlive() const {
    return keep_alive_;
}

std::size_t HttpResponse::LastBuiltBytes() const {
    return last_built_bytes_;
}

void HttpResponse::MarkDeferred() {
    if (state_ == State::kOpen) {
        state_ = State::kDeferred;
    }
}

std::string HttpResponse::SerializeHeaders(std::size_t body_size) const {
    std::string headers;
    headers.reserve(256);
    const bool status_no_body = StatusMustNotHaveBody(status_);

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
        headers += "Server: iouring_web\r\n";
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
        headers += std::to_string(body_size);
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
    return headers;
}

core::buffer::SendBufferRef HttpResponse::Build(
    core::buffer::BufferPool& pool) const {
    const bool status_no_body = StatusMustNotHaveBody(status_);
    const bool no_body = status_no_body || suppress_body_;
    const auto headers = SerializeHeaders(body_.size());

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

std::vector<core::buffer::SendBufferRef> HttpResponse::BuildBuffers(
    core::buffer::BufferPool& pool,
    std::uint32_t body_chunk_size) const {
    const bool status_no_body = StatusMustNotHaveBody(status_);
    const bool no_body = status_no_body || suppress_body_;
    const auto headers = SerializeHeaders(body_.size());
    const auto chunk_size = std::max<std::uint32_t>(body_chunk_size, 1);

    std::vector<core::buffer::SendBufferRef> buffers;
    buffers.reserve(no_body ? 1 : 1 + ((body_.size() + chunk_size - 1) /
                                       chunk_size));

    auto header = BufferFromBytes(pool, headers);
    if (!header) {
        return {};
    }
    buffers.push_back(std::move(header));

    if (!no_body) {
        std::size_t offset = 0;
        while (offset < body_.size()) {
            const auto next_size = std::min<std::size_t>(chunk_size,
                                                         body_.size() - offset);
            auto body_buffer =
                BufferFromBytes(pool, std::string_view(body_).substr(offset,
                                                                     next_size));
            if (!body_buffer) {
                return {};
            }
            buffers.push_back(std::move(body_buffer));
            offset += next_size;
        }
    }

    last_built_bytes_ = headers.size() + (no_body ? 0 : body_.size());
    return buffers;
}

void HttpResponse::Send() {
    if (state_ == State::kSent) {
        return;
    }
    state_ = State::kSent;
    if (!session_ || !pool_) {
        return;
    }

    const bool status_no_body = StatusMustNotHaveBody(status_);
    const bool no_body = status_no_body || suppress_body_;
    const auto headers = SerializeHeaders(body_.size());
    const auto total_size = headers.size() + (no_body ? 0 : body_.size());
    last_built_bytes_ = total_size;

    if (total_size <= kInlineResponseMaxBytes) {
        auto buffer = Build(*pool_);
        if (buffer) {
            session_->SendResponse(std::move(buffer));
            return;
        }
    }

    auto header = BufferFromBytes(*pool_, headers);
    if (!header) {
        return;
    }
    if (no_body || body_.empty()) {
        session_->SendResponse(std::move(header));
        return;
    }

    auto body = std::make_shared<std::string>(std::move(body_));
    session_->StartBodyStream(std::move(header), std::move(body),
                              kBodyStreamChunkSize,
                              kBodyStreamMaxChunksPerWrite);
}

bool HttpResponse::SendFile(std::string path, std::string_view content_type,
                            std::uint32_t chunk_size,
                            std::uint32_t max_chunks_per_write) {
    if (state_ == State::kSent) {
        return false;
    }
    if (!session_ || !pool_) {
        state_ = State::kSent;
        return false;
    }

    const bool status_no_body = StatusMustNotHaveBody(status_);
    keep_alive_ = false;
    if (content_type_.empty() && !content_type.empty()) {
        content_type_ = content_type;
    }

    int file_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file_fd < 0) {
        Error(errno == ENOENT ? HttpStatus::kNotFound
                              : HttpStatus::kInternalServerError,
              errno == ENOENT ? "Not Found" : "Internal Server Error");
        Send();
        return false;
    }

    struct stat st {};
    if (::fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(file_fd);
        Error(HttpStatus::kInternalServerError, "Internal Server Error");
        Send();
        return false;
    }

    const std::uint64_t file_size =
        static_cast<std::uint64_t>(st.st_size < 0 ? 0 : st.st_size);
    const bool no_body = status_no_body || suppress_body_;
    const auto headers = SerializeHeaders(static_cast<std::size_t>(file_size));

    auto result =
        pool_->Allocate(static_cast<std::uint32_t>(headers.size()));
    if (!result) {
        ::close(file_fd);
        return false;
    }

    auto header = std::move(*result);
    auto writable = header->Writable();
    std::memcpy(writable.data(), headers.data(), headers.size());
    header->Commit(static_cast<std::uint32_t>(headers.size()));
    last_built_bytes_ = headers.size();
    state_ = State::kSent;

    if (no_body) {
        ::close(file_fd);
        session_->SendResponse(std::move(header));
        return true;
    }

    if (!session_->StartFileStream(std::move(header), file_fd, file_size,
                                   chunk_size, max_chunks_per_write)) {
        ::close(file_fd);
        return false;
    }
    return true;
}

core::buffer::SendBufferRef HttpResponse::NoContent(
    core::buffer::BufferPool& pool, bool keep_alive) {
    return HttpResponse()
        .Status(HttpStatus::kNoContent)
        .KeepAlive(keep_alive)
        .Build(pool);
}

} // namespace iouring::http
