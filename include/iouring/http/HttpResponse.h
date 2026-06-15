#pragma once

#include <iouring/core/SendBuffer.h>
#include <iouring/http/HttpStatus.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iouring::http {

class HttpSession;
struct RequestContext;

class HttpResponse {
public:
    HttpResponse() = default;
    HttpResponse(HttpSession& session, core::buffer::BufferPool& pool);

    HttpResponse& Status(HttpStatus status);

    HttpResponse& Header(std::string name, std::string value);

    bool HasHeader(std::string_view name) const;
    HttpResponse& RemoveHeader(std::string_view name);

    HttpResponse& ContentType(std::string_view type);

    HttpResponse& Body(std::string body);

    HttpResponse& Text(std::string text);

    HttpResponse& Json(std::string json_body);

    HttpResponse& Error(HttpStatus status, std::string message);

    HttpResponse& KeepAlive(bool keep_alive);

    HttpResponse& SuppressBody(bool suppress = true);

    HttpResponse& Redirect(std::string url,
                           HttpStatus status = HttpStatus::kFound);

    core::buffer::SendBufferRef Build(core::buffer::BufferPool& pool) const;
    std::vector<core::buffer::SendBufferRef> BuildBuffers(
        core::buffer::BufferPool& pool,
        std::uint32_t body_chunk_size = 256 * 1024) const;
    void Send();
    bool SendFile(std::string path,
                  std::string_view content_type = "application/octet-stream",
                  std::uint32_t chunk_size = 256 * 1024,
                  std::uint32_t max_chunks_per_write = 4);

    static core::buffer::SendBufferRef NoContent(core::buffer::BufferPool& pool,
                                                 bool keep_alive = true);

    HttpStatus StatusCode() const;

    const std::string& GetBody() const;

    bool IsSent() const;

    bool IsDeferred() const;

    bool GetKeepAlive() const;

    std::size_t LastBuiltBytes() const;

private:
    friend struct RequestContext;

    enum class State : std::uint8_t {
        kOpen,
        kSent,
        kDeferred,
    };

    void MarkDeferred();

    std::string SerializeHeaders(std::size_t body_size) const;

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

} // namespace iouring::http
