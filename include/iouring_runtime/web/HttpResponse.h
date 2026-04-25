#pragma once

#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/web/HttpStatus.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iouring_runtime::web {

class HttpSession;
struct RequestContext;

class HttpResponse {
public:
    HttpResponse() = default;
    HttpResponse(HttpSession& session, core::buffer::BufferPool& pool);

    HttpResponse& Status(HttpStatus status) {
        status_ = status;
        return *this;
    }

    HttpResponse& Header(std::string name, std::string value) {
        headers_.push_back({std::move(name), std::move(value)});
        return *this;
    }

    bool HasHeader(std::string_view name) const;
    HttpResponse& RemoveHeader(std::string_view name);

    HttpResponse& ContentType(std::string_view type) {
        content_type_ = type;
        return *this;
    }

    HttpResponse& Body(std::string body) {
        body_ = std::move(body);
        return *this;
    }

    HttpResponse& Json(std::string json_body) {
        content_type_ = "application/json";
        body_ = std::move(json_body);
        return *this;
    }

    HttpResponse& KeepAlive(bool keep_alive) {
        keep_alive_ = keep_alive;
        return *this;
    }

    HttpResponse& SuppressBody(bool suppress = true) {
        suppress_body_ = suppress;
        return *this;
    }

    HttpResponse& Redirect(std::string url,
                           HttpStatus status = HttpStatus::kFound);

    core::buffer::SendBufferRef Build(core::buffer::BufferPool& pool) const;
    void Send();

    static core::buffer::SendBufferRef NoContent(core::buffer::BufferPool& pool,
                                                 bool keep_alive = true);

    HttpStatus StatusCode() const {
        return status_;
    }

    const std::string& GetBody() const {
        return body_;
    }

    bool IsSent() const {
        return state_ == State::kSent;
    }

    bool IsDeferred() const {
        return state_ == State::kDeferred;
    }

    bool GetKeepAlive() const {
        return keep_alive_;
    }

    std::size_t LastBuiltBytes() const {
        return last_built_bytes_;
    }

private:
    friend struct RequestContext;

    enum class State : std::uint8_t {
        kOpen,
        kSent,
        kDeferred,
    };

    void MarkDeferred() {
        if (state_ == State::kOpen) {
            state_ = State::kDeferred;
        }
    }

    struct HeaderPair {
        std::string name;
        std::string value;
    };

    HttpStatus status_ = HttpStatus::kOk;
    std::vector<HeaderPair> headers_;
    std::string content_type_;
    std::string body_;
    bool keep_alive_ = true;
    State state_ = State::kOpen;
    bool suppress_body_ = false;
    mutable std::size_t last_built_bytes_ = 0;
    HttpSession* session_ = nullptr;
    core::buffer::BufferPool* pool_ = nullptr;
};

} // namespace iouring_runtime::web
