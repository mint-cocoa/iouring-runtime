#include "AcmeChallengeSession.h"

#include "ProxyCommon.h"

#include <iouring_runtime/core/RecvBuffer.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

namespace iouring_runtime::proxy::detail {

class AcmeChallengeSession final : public core::io::Session {
public:
    AcmeChallengeSession(int fd, core::ring::IoRing& ring,
                         core::buffer::BufferPool& pool,
                         std::string challenge_webroot,
                         std::uint32_t send_queue_max_pending)
        : Session(fd, ring, pool, send_queue_max_pending)
        , challenge_webroot_(std::move(challenge_webroot))
        , recv_buffer_(16 * 1024) {}

protected:
    void OnRecv(std::span<const std::byte> data) final {
        if (request_complete_) {
            return;
        }

        auto append = recv_buffer_.Append(data);
        if (!append) {
            SendSimpleResponse("413 Payload Too Large", "text/plain",
                               "Request Too Large");
            return;
        }

        const auto region = recv_buffer_.ReadRegion();
        std::string_view text(reinterpret_cast<const char*>(region.data()),
                              region.size());
        const auto header_end = text.find("\r\n\r\n");
        if (header_end == std::string_view::npos) {
            return;
        }

        request_complete_ = true;
        HandleRequest(text.substr(0, header_end));
    }

private:
    std::optional<std::string> ReadChallengeBody(std::string_view token) const {
        const auto open_file = [](const std::filesystem::path& path)
            -> std::optional<std::string> {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return std::nullopt;
            }

            std::ostringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        };

        const std::filesystem::path webroot(challenge_webroot_);
        if (auto body = open_file(
                webroot / ".well-known" / "acme-challenge" / std::string(token))) {
            return body;
        }

        // Keep supporting the earlier flat layout to avoid breaking existing setups.
        return open_file(webroot / std::string(token));
    }

    static bool IsValidToken(std::string_view token) {
        if (token.empty()) {
            return false;
        }
        for (char ch : token) {
            const bool allowed =
                (ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.';
            if (!allowed) {
                return false;
            }
        }
        return true;
    }

    static std::string BuildHttpResponse(std::string_view status,
                                         std::string_view content_type,
                                         std::string_view body) {
        std::ostringstream out;
        out << "HTTP/1.1 " << status << "\r\n";
        out << "Content-Type: " << content_type << "\r\n";
        out << "Content-Length: " << body.size() << "\r\n";
        out << "Connection: close\r\n";
        out << "Cache-Control: no-cache\r\n";
        out << "\r\n";
        out << body;
        return out.str();
    }

    void HandleRequest(std::string_view headers) {
        const auto line_end = headers.find("\r\n");
        const auto request_line = headers.substr(0, line_end);

        const auto first_space = request_line.find(' ');
        const auto second_space =
            first_space == std::string_view::npos
                ? std::string_view::npos
                : request_line.find(' ', first_space + 1);
        if (first_space == std::string_view::npos ||
            second_space == std::string_view::npos) {
            SendSimpleResponse("400 Bad Request", "text/plain", "Bad Request");
            return;
        }

        const auto method = request_line.substr(0, first_space);
        const auto target =
            request_line.substr(first_space + 1, second_space - first_space - 1);
        if (method != "GET") {
            SendSimpleResponse("405 Method Not Allowed", "text/plain",
                               "Method Not Allowed");
            return;
        }

        constexpr std::string_view kPrefix = "/.well-known/acme-challenge/";
        if (!target.starts_with(kPrefix)) {
            SendSimpleResponse("404 Not Found", "text/plain", "Not Found");
            return;
        }

        const auto token = target.substr(kPrefix.size());
        if (!IsValidToken(token)) {
            SendSimpleResponse("400 Bad Request", "text/plain", "Bad Request");
            return;
        }

        auto body = ReadChallengeBody(token);
        if (!body) {
            SendSimpleResponse("404 Not Found", "text/plain", "Not Found");
            return;
        }
        SendSimpleResponse("200 OK", "text/plain", *body);
    }

    void SendSimpleResponse(std::string_view status,
                            std::string_view content_type,
                            std::string_view body) {
        auto response = BuildHttpResponse(status, content_type, body);
        auto buffer_result = CopyToSendBuffer(Pool(), response);
        if (!buffer_result) {
            Disconnect();
            return;
        }
        Send(std::move(*buffer_result));
        DisconnectAfterFlush();
    }

    std::string challenge_webroot_;
    core::buffer::RecvBuffer recv_buffer_;
    bool request_complete_{false};
};

core::io::SessionRef CreateAcmeChallengeSession(
    int fd, core::ring::IoRing& ring, core::buffer::BufferPool& pool,
    std::string challenge_webroot, std::uint32_t send_queue_max_pending) {
    return std::make_shared<AcmeChallengeSession>(
        fd, ring, pool, std::move(challenge_webroot), send_queue_max_pending);
}

} // namespace iouring_runtime::proxy::detail
