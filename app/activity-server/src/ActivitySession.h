#pragma once

#include "ActivityHttp.h"

#include <iouring_runtime/core/Session.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <filesystem>
#include <string>
#include <string_view>

namespace activity_server {

class ActivityHub;

class ActivitySession : public iouring_runtime::core::io::Session {
public:
    ActivitySession(int fd, iouring_runtime::core::ring::IoRing& io_ring,
                    iouring_runtime::core::buffer::BufferPool& pool,
                    ActivityHub& hub);

    void SendTextAsync(std::string text);

protected:
    void OnRecv(std::span<const std::byte> data) override;
    void OnDisconnected() override;

private:
    void ProcessHttpBuffer();
    std::optional<HttpRequest> TryParseHttp();
    void HandleHttp(const HttpRequest& req);

    void UpgradeWebSocket(const HttpRequest& req);
    void ProcessWsBuffer();
    void HandleWsText(std::string_view text);
    void ApplySync(std::string_view text, std::string_view client_id);

    void AdvanceQueue(const std::string& instance_id);
    void RemoveQueue(std::string_view body);
    void ServeHls(const HttpRequest& req);
    bool ServeStaticFrontend(const HttpRequest& req);
    void ProxyThumbnail(const HttpRequest& req);
    void ProxyHls(const HttpRequest& req);
    void ExchangeDiscordToken(std::string_view body);

    void SendHttp(int status, std::string_view content_type, std::string body);
    void SendFileResponse(const HttpRequest& req, const std::filesystem::path& path,
                          std::string_view content_type);
    bool PumpFileStream();
    void ResetFileStream();
    void SendRaw(const std::string& data);
    void SendWsTextOnRing(const std::string& text);
    void SendWsFrame(std::uint8_t opcode, std::string_view payload);

    bool HasPendingAppWork() const override;
    void OnSocketDrained() override;

    ActivityHub& hub_;
    std::string http_buffer_;
    std::string ws_buffer_;
    std::string instance_id_ = "default";
    std::string client_id_;
    bool websocket_ = false;

    struct FileStreamState {
        iouring_runtime::core::io::SocketHandle fd;
        std::uint64_t remaining_bytes = 0;
        std::uint32_t chunk_size = 256 * 1024;
        std::uint32_t max_chunks_per_write = 4;
        bool active = false;
    };
    FileStreamState file_stream_;

    static std::mutex sync_mu_;
    static double last_sync_time_;
    static bool last_sync_playing_;
};

} // namespace activity_server
