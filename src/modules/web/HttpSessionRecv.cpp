#include <iouring_runtime/web/HttpSession.h>

#include <iouring_runtime/web/HttpResponse.h>

#include <iouring_runtime/observability/Logging.h>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kHttp;
}

namespace iouring_runtime::web {

namespace {

struct ParseErrorResponse {
    HttpStatus status;
    std::string_view body;
};

ParseErrorResponse MapParseError(HttpParseStatus status) {
    switch (status) {
        case HttpParseStatus::kUrlTooLong:
            return {HttpStatus::kUriTooLong, "URI Too Long"};
        case HttpParseStatus::kHeadersTooLarge:
            return {HttpStatus::kRequestHeaderFieldsTooLarge,
                    "Request Header Fields Too Large"};
        case HttpParseStatus::kBodyTooLarge:
            return {HttpStatus::kPayloadTooLarge, "Payload Too Large"};
        case HttpParseStatus::kBadRequest:
        case HttpParseStatus::kOk:
            break;
    }
    return {HttpStatus::kBadRequest, "Bad Request"};
}

} // namespace

void HttpSession::HandleHttpRecv(const std::byte* data, std::uint32_t len) {
    if (StartOrFailRequestDeadline()) {
        return;
    }

    if (recv_buffer_.IsEmpty()) {
        const auto* chars = reinterpret_cast<const char*>(data);
        const auto consumed = parser_.Feed(chars, len);

        if (parser_.HasError()) {
            if (ActiveStreamResponseSent()) {
                AbortStreamingRequest();
                Disconnect();
                return;
            }
            AbortStreamingRequest();
            const auto error = MapParseError(parser_.Status());
            auto buffer = HttpResponse()
                .Status(error.status)
                .ContentType("text/plain")
                .Body(std::string(error.body))
                .KeepAlive(false)
                .Build(Pool());
            if (buffer) {
                Send(std::move(buffer));
            }
            Disconnect();
            return;
        }

        if (consumed < len) {
            auto append = recv_buffer_.Append(
                std::span<const std::byte>(data + consumed, len - consumed));
            if (!append) {
                obs::LogError(kLogCategory, "HttpSession[fd={}]: recv buffer overflow", Fd());
                Disconnect();
                return;
            }
        }
        return;
    }

    auto append = recv_buffer_.Append(std::span<const std::byte>(data, len));
    if (!append) {
        obs::LogError(kLogCategory, "HttpSession[fd={}]: recv buffer overflow", Fd());
        Disconnect();
        return;
    }

    auto region = recv_buffer_.ReadRegion();
    const auto* chars = reinterpret_cast<const char*>(region.data());
    const auto region_len = static_cast<std::uint32_t>(region.size());
    const auto consumed = parser_.Feed(chars, region_len);

    if (parser_.HasError()) {
        if (ActiveStreamResponseSent()) {
            AbortStreamingRequest();
            Disconnect();
            return;
        }
        AbortStreamingRequest();
        const auto error = MapParseError(parser_.Status());
        auto buffer = HttpResponse()
            .Status(error.status)
            .ContentType("text/plain")
            .Body(std::string(error.body))
            .KeepAlive(false)
            .Build(Pool());
        if (buffer) {
            Send(std::move(buffer));
        }
        Disconnect();
        return;
    }

    recv_buffer_.OnRead(consumed);
    if (recv_buffer_.ShouldCompact()) {
        recv_buffer_.Compact();
    }
}

} // namespace iouring_runtime::web
