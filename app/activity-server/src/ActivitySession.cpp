#include "ActivitySession.h"

#include "ActivityHub.h"
#include "ActivityUtils.h"

#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/media/Hls.h>
#include <iouring_runtime/observability/Logging.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace activity_server {

namespace core = iouring_runtime::core;
namespace media = iouring_runtime::media;
namespace obs = iouring_runtime::observability;

std::mutex ActivitySession::sync_mu_;
double ActivitySession::last_sync_time_ = 0.0;
bool ActivitySession::last_sync_playing_ = true;

namespace {

bool IsBackendPrefix(std::string_view path) {
    return path == "/api" || path.starts_with("/api/") ||
           path == "/ws" ||
           path == "/hls" || path.starts_with("/hls/") ||
           path == "/files" || path.starts_with("/files/") ||
           path == "/local" || path.starts_with("/local/") ||
           path == "/proxy" || path.starts_with("/proxy/");
}

std::string StaticContentType(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".wasm") return "application/wasm";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool IsWithinRoot(const std::filesystem::path& root, const std::filesystem::path& target) {
    auto root_it = root.begin();
    auto target_it = target.begin();
    for (; root_it != root.end(); ++root_it, ++target_it) {
        if (target_it == target.end() || *root_it != *target_it) {
            return false;
        }
    }
    return true;
}

} // namespace

ActivitySession::ActivitySession(int fd, core::ring::IoRing& io_ring,
                                 core::buffer::BufferPool& pool,
                                 ActivityHub& hub)
    : Session(fd, io_ring, pool, 4096)
    , hub_(hub) {
    SetInactivityTimeout(std::chrono::minutes{10});
}

void ActivitySession::SendTextAsync(std::string text) {
    auto self = std::static_pointer_cast<ActivitySession>(shared_from_this());
    Ring().RunOnRing([self, text = std::move(text)] {
        self->SendWsTextOnRing(text);
    });
}

void ActivitySession::OnRecv(std::span<const std::byte> data) {
    const auto* ptr = reinterpret_cast<const char*>(data.data());
    if (websocket_) {
        ws_buffer_.append(ptr, data.size());
        ProcessWsBuffer();
        return;
    }
    http_buffer_.append(ptr, data.size());
    ProcessHttpBuffer();
}

void ActivitySession::OnDisconnected() {
    if (websocket_ && !client_id_.empty()) {
        obs::LogInfo(obs::LogCategory::kSession,
                     "activity websocket disconnected client_id={} instance_id={} fd={}",
                     client_id_, instance_id_, Fd());
    }
    hub_.Remove(*this);
}

void ActivitySession::ProcessHttpBuffer() {
    for (;;) {
        auto parsed = TryParseHttp();
        if (!parsed) {
            return;
        }
        HandleHttp(*parsed);
        if (websocket_ || Disconnecting()) {
            return;
        }
    }
}

std::optional<HttpRequest> ActivitySession::TryParseHttp() {
    const auto header_end = http_buffer_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        if (http_buffer_.size() > 64 * 1024) {
            SendHttp(431, "text/plain", "Request Header Fields Too Large");
            DisconnectAfterFlush();
        }
        return std::nullopt;
    }

    const std::string head = http_buffer_.substr(0, header_end);
    std::istringstream in(head);
    HttpRequest req;
    std::string version;
    in >> req.method >> req.target >> version;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        req.headers[Lower(line.substr(0, colon))] =
            Trim(std::string_view(line).substr(colon + 1));
    }

    const auto query_pos = req.target.find('?');
    req.path = query_pos == std::string::npos ? req.target : req.target.substr(0, query_pos);
    req.query = query_pos == std::string::npos ? std::string{} : req.target.substr(query_pos + 1);

    std::size_t body_len = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
        try {
            body_len = static_cast<std::size_t>(std::stoull(it->second));
        } catch (...) {
            SendHttp(400, "text/plain", "Bad Request");
            DisconnectAfterFlush();
            return std::nullopt;
        }
    }

    const auto total = header_end + 4 + body_len;
    if (http_buffer_.size() < total) {
        return std::nullopt;
    }
    req.body = http_buffer_.substr(header_end + 4, body_len);
    http_buffer_.erase(0, total);
    return req;
}

void ActivitySession::HandleHttp(const HttpRequest& req) {
    if (req.method == "GET" && req.path == "/ws") {
        UpgradeWebSocket(req);
        return;
    }
    if (req.method == "GET" && req.path == "/api/queue") {
        const auto instance_id = QueryParam(req.query, "instance_id").empty()
            ? "default"
            : QueryParam(req.query, "instance_id");
        SendHttp(200, "application/json", hub_.QueueJson(instance_id));
        return;
    }
    if (req.method == "POST" && req.path == "/api/download") {
        const auto url = JsonString(req.body, "url").value_or("");
        const auto instance_id = JsonString(req.body, "instance_id").value_or("default");
        if (url.empty()) {
            SendHttp(400, "application/json", "{\"detail\":\"url is required\"}");
            return;
        }
        const auto task_id = hub_.CreateDownload(url, instance_id.empty() ? "default" : instance_id);
        SendHttp(200, "application/json",
                 "{\"status\":\"pending\",\"task_id\":\"" + JsonEscape(task_id) +
                 "\",\"message\":\"Download started in background\"}");
        return;
    }
    if (req.method == "POST" && req.path == "/api/play") {
        const auto url = JsonString(req.body, "url").value_or("");
        const auto instance_id = JsonString(req.body, "instance_id").value_or("default");
        if (url.empty()) {
            SendHttp(400, "application/json", "{\"detail\":\"url is required\"}");
            return;
        }
        const auto task_id = hub_.CreateDownload(
            url, instance_id.empty() ? "default" : instance_id, true);
        SendHttp(200, "application/json",
                 "{\"status\":\"pending\",\"task_id\":\"" + JsonEscape(task_id) +
                 "\",\"message\":\"Playback download started in background\"}");
        return;
    }
    if (req.method == "POST" && req.path == "/api/discord/token") {
        ExchangeDiscordToken(req.body);
        return;
    }
    if (req.method == "GET" && req.path.rfind("/api/download/status/", 0) == 0) {
        const auto task_id = req.path.substr(std::string("/api/download/status/").size());
        const auto body = hub_.TaskJson(task_id);
        SendHttp(body.empty() ? 404 : 200, "application/json",
                 body.empty() ? "{\"detail\":\"Task not found\"}" : body);
        return;
    }
    if (req.method == "POST" && req.path == "/api/next") {
        const auto instance_id = JsonString(req.body, "instance_id").value_or("default");
        AdvanceQueue(instance_id.empty() ? "default" : instance_id);
        SendHttp(200, "application/json", "{\"ok\":true}");
        return;
    }
    if (req.method == "POST" && req.path == "/api/queue/remove") {
        RemoveQueue(req.body);
        SendHttp(200, "application/json", "{\"ok\":true}");
        return;
    }
    if (req.method == "GET" && req.path.rfind("/hls/", 0) == 0) {
        ServeHls(req.path);
        return;
    }
    if ((req.method == "GET" || req.method == "HEAD") && req.path == "/api/thumb") {
        ProxyThumbnail(req);
        return;
    }
    if ((req.method == "GET" || req.method == "HEAD") && req.path == "/proxy/hls") {
        ProxyHls(req);
        return;
    }
    if (req.path.rfind("/proxy/netflix/", 0) == 0 ||
        req.path.rfind("/proxy/webrtc/", 0) == 0 ||
        req.path.rfind("/api/tving/", 0) == 0) {
        SendHttp(501, "application/json",
                 "{\"detail\":\"This FastAPI route is not ported to the C++ activity server yet\"}");
        return;
    }
    if (req.method == "GET" && req.path == "/healthz") {
        SendHttp(200, "application/json", "{\"status\":\"ok\",\"runtime\":\"cpp\"}");
        return;
    }
    if ((req.method == "GET" || req.method == "HEAD") && !IsBackendPrefix(req.path) &&
        ServeStaticFrontend(req)) {
        return;
    }

    SendHttp(404, "application/json", "{\"detail\":\"Not found\"}");
}

void ActivitySession::UpgradeWebSocket(const HttpRequest& req) {
    auto key_it = req.headers.find("sec-websocket-key");
    if (key_it == req.headers.end()) {
        SendHttp(400, "text/plain", "Missing Sec-WebSocket-Key");
        return;
    }
    const auto accept = WebSocketAccept(key_it->second);
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "\r\n";
    SendRaw(response);
    websocket_ = true;
}

void ActivitySession::ProcessWsBuffer() {
    for (;;) {
        if (ws_buffer_.size() < 2) return;
        const auto b0 = static_cast<unsigned char>(ws_buffer_[0]);
        const auto b1 = static_cast<unsigned char>(ws_buffer_[1]);
        const auto opcode = b0 & 0x0f;
        const bool masked = (b1 & 0x80) != 0;
        std::uint64_t len = b1 & 0x7f;
        std::size_t pos = 2;
        if (len == 126) {
            if (ws_buffer_.size() < pos + 2) return;
            len = (static_cast<unsigned char>(ws_buffer_[pos]) << 8) |
                  static_cast<unsigned char>(ws_buffer_[pos + 1]);
            pos += 2;
        } else if (len == 127) {
            if (ws_buffer_.size() < pos + 8) return;
            len = 0;
            for (int i = 0; i < 8; ++i) {
                len = (len << 8) | static_cast<unsigned char>(ws_buffer_[pos + i]);
            }
            pos += 8;
        }
        if (len > 1024 * 1024) {
            Disconnect();
            return;
        }
        unsigned char mask[4] = {};
        if (masked) {
            if (ws_buffer_.size() < pos + 4) return;
            for (int i = 0; i < 4; ++i) mask[i] = static_cast<unsigned char>(ws_buffer_[pos + i]);
            pos += 4;
        }
        if (ws_buffer_.size() < pos + len) return;
        std::string payload = ws_buffer_.substr(pos, static_cast<std::size_t>(len));
        if (masked) {
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(
                    static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
            }
        }
        ws_buffer_.erase(0, pos + static_cast<std::size_t>(len));

        if (opcode == 0x8) {
            DisconnectAfterFlush();
            return;
        }
        if (opcode == 0x9) {
            SendWsFrame(0xA, payload);
            continue;
        }
        if (opcode == 0x1) {
            HandleWsText(payload);
        }
    }
}

void ActivitySession::HandleWsText(std::string_view text) {
    const auto type = JsonString(text, "type").value_or("");
    const auto client_id = JsonString(text, "client_id").value_or(client_id_);

    if (type == "HELLO") {
        instance_id_ = JsonString(text, "instance_id").value_or("");
        if (instance_id_.empty()) instance_id_ = "default";
        client_id_ = client_id.empty() ? RandomId("client_") : client_id;
        hub_.Register(*this,
                      std::static_pointer_cast<ActivitySession>(shared_from_this()),
                      instance_id_, client_id_);
        obs::LogInfo(obs::LogCategory::kSession,
                     "activity websocket joined client_id={} instance_id={} fd={}",
                     client_id_, instance_id_, Fd());
        SendTextAsync("{\"type\":\"STATE_UPDATE\",\"seq\":1,\"origin\":null,\"payload\":" +
                      hub_.StatePayloadJson(instance_id_) + "}");
        hub_.BroadcastPresence(instance_id_);
        return;
    }

    if (type == "CHAT_MESSAGE") {
        const auto payload_text = JsonString(text, "text").value_or("");
        const auto sender = JsonString(text, "sender").value_or(
            client_id_.empty() ? "Guest" : client_id_);
        if (!payload_text.empty()) {
            const auto now = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::ostringstream out;
            out << "{\"type\":\"CHAT_MESSAGE\",\"origin\":{\"client_id\":\""
                << JsonEscape(client_id_) << "\"},\"payload\":{\"text\":\""
                << JsonEscape(payload_text.substr(0, 500)) << "\",\"sender\":\""
                << JsonEscape(sender.substr(0, 32)) << "\",\"server_ts\":"
                << std::fixed << std::setprecision(3) << now << "}}";
            hub_.BroadcastText(instance_id_, out.str());
        }
        return;
    }

    if (type == "REQUEST_SYNC") {
        ApplySync(text, client_id);
    }
}

void ActivitySession::ApplySync(std::string_view text, std::string_view client_id) {
    const auto time = JsonNumber(text, "time").value_or(0.0);
    const auto reason = JsonString(text, "reason").value_or("");
    {
        std::lock_guard lock(sync_mu_);
        last_sync_time_ = time;
        last_sync_playing_ = reason != "pause_button";
    }
    (void)client_id;
    hub_.BroadcastState(instance_id_, std::string(client_id));
}

void ActivitySession::AdvanceQueue(const std::string& instance_id) {
    {
        std::lock_guard lock(hub_.mu_);
        auto& state = hub_.StateLocked(instance_id);
        if (!state.queue.empty()) {
            auto next = state.queue.front();
            state.queue.erase(state.queue.begin());
            state.current_video_url = next.url;
            state.metadata = std::move(next);
            state.is_playing = true;
            state.current_time = 0.0;
            state.start_at = std::chrono::steady_clock::now();
        } else {
            state.current_video_url.clear();
            state.metadata = {};
            state.is_playing = false;
            state.current_time = 0.0;
            state.start_at = {};
        }
    }
    hub_.BroadcastState(instance_id);
}

void ActivitySession::RemoveQueue(std::string_view body) {
    const auto id = JsonString(body, "id").value_or("");
    const auto instance_id = JsonString(body, "instance_id").value_or("default");
    if (id.empty()) return;
    {
        std::lock_guard lock(hub_.mu_);
        auto& queue = hub_.StateLocked(instance_id).queue;
        queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const QueueEntry& item) {
            return item.id == id;
        }), queue.end());
    }
    hub_.BroadcastState(instance_id);
}

void ActivitySession::ServeHls(const std::string& path) {
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(
        EnvString("ACTIVITY_DOWNLOAD_DIR", "/var/lib/iouring-runtime/activity-server/downloads")) / "hls");
    const auto rel = media::UrlDecode(std::string_view(path).substr(std::string_view("/hls/").size()));
    const auto target = std::filesystem::weakly_canonical(root / rel);
    if (target.string().rfind(root.string(), 0) != 0 || !std::filesystem::is_regular_file(target)) {
        SendHttp(404, "text/plain", "Not Found");
        return;
    }
    const auto body = ReadFile(target);
    std::string type = "application/octet-stream";
    if (target.extension() == ".m3u8") type = "application/vnd.apple.mpegurl";
    if (target.extension() == ".ts") type = "video/mp2t";
    SendHttp(200, type, body);
}

bool ActivitySession::ServeStaticFrontend(const HttpRequest& req) {
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(
        EnvString("ACTIVITY_STATIC_DIR", "/usr/share/activity-server/frontend")));
    if (!std::filesystem::is_directory(root)) {
        return false;
    }

    auto rel = media::UrlDecode(req.path.empty() ? "/" : req.path);
    if (!rel.empty() && rel.front() == '/') {
        rel.erase(0, 1);
    }

    auto target = rel.empty() ? root / "index.html" : root / rel;
    target = std::filesystem::weakly_canonical(target);
    if (!IsWithinRoot(root, target)) {
        SendHttp(404, "text/plain", "Not Found");
        return true;
    }

    if (!std::filesystem::is_regular_file(target)) {
        target = root / "index.html";
    }
    if (!std::filesystem::is_regular_file(target)) {
        return false;
    }

    const auto body = req.method == "HEAD" ? std::string{} : ReadFile(target);
    SendHttp(200, StaticContentType(target), body);
    return true;
}

void ActivitySession::ProxyThumbnail(const HttpRequest& req) {
    const auto target = QueryParam(req.query, "target");
    if (target.empty()) {
        SendHttp(400, "application/json", "{\"detail\":\"target is required\"}");
        return;
    }
    if (!IsHttpUrl(target)) {
        SendHttp(400, "application/json", "{\"detail\":\"Only http(s) targets supported\"}");
        return;
    }

    if (req.method == "HEAD") {
        SendHttp(200, media::ContentTypeForUrl(target), "");
        return;
    }

    const auto upstream = CurlGet(target, "--max-time 10");
    if (upstream.exit_code != 0) {
        SendHttp(502, "application/json", "{\"detail\":\"Thumbnail upstream request failed\"}");
        return;
    }
    SendHttp(200, media::ContentTypeForUrl(target), upstream.output);
}

void ActivitySession::ProxyHls(const HttpRequest& req) {
    const auto url = QueryParam(req.query, "url");
    if (url.empty()) {
        SendHttp(400, "application/json", "{\"detail\":\"url is required\"}");
        return;
    }
    if (!IsHttpUrl(url)) {
        SendHttp(400, "application/json", "{\"detail\":\"Only http/https URLs are supported\"}");
        return;
    }

    if (req.method == "HEAD") {
        SendHttp(200, media::ContentTypeForUrl(url), "");
        return;
    }

    std::string extra_args;
    if (const auto range = Header(req, "range"); !range.empty()) {
        extra_args += "-H " + ShellQuote("Range: " + range);
    }

    const auto upstream = CurlGet(url, extra_args);
    if (upstream.exit_code != 0) {
        SendHttp(502, "application/json", "{\"detail\":\"Upstream request failed\"}");
        return;
    }

    const auto content_type = media::ContentTypeForUrl(url);
    if (content_type == "application/vnd.apple.mpegurl" ||
        upstream.output.starts_with("#EXTM3U")) {
        SendHttp(200, "application/vnd.apple.mpegurl; charset=utf-8",
                 media::RewriteHlsManifest(upstream.output, url));
        return;
    }
    SendHttp(200, content_type, upstream.output);
}

void ActivitySession::ExchangeDiscordToken(std::string_view body) {
    const auto code = JsonString(body, "code").value_or("");
    if (code.empty()) {
        SendHttp(400, "application/json", "{\"detail\":\"code is required\"}");
        return;
    }

    const auto client_id = EnvString("DISCORD_CLIENT_ID", "");
    const auto client_secret = EnvString("DISCORD_CLIENT_SECRET", "");
    if (client_id.empty() || client_secret.empty()) {
        SendHttp(500, "application/json",
                 "{\"detail\":\"Server misconfigured (missing Discord credentials)\"}");
        return;
    }

    const std::string command =
        "curl -fsS --max-time 20 -X POST " +
        ShellQuote("https://discord.com/api/oauth2/token") +
        " -H " + ShellQuote("Content-Type: application/x-www-form-urlencoded") +
        " --data-urlencode " + ShellQuote("client_id=" + client_id) +
        " --data-urlencode " + ShellQuote("client_secret=" + client_secret) +
        " --data-urlencode " + ShellQuote("grant_type=authorization_code") +
        " --data-urlencode " + ShellQuote("code=" + code) +
        " 2>/dev/null";

    const auto upstream = RunCommandCapture(command);
    if (upstream.exit_code != 0) {
        SendHttp(400, "application/json", "{\"detail\":\"Failed to exchange token\"}");
        return;
    }
    SendHttp(200, "application/json", upstream.output);
}

void ActivitySession::SendHttp(int status, std::string_view content_type, std::string body) {
    std::string reason = "OK";
    if (status == 400) reason = "Bad Request";
    if (status == 404) reason = "Not Found";
    if (status == 431) reason = "Request Header Fields Too Large";
    if (status == 501) reason = "Not Implemented";
    if (status == 502) reason = "Bad Gateway";
    if (status >= 500 && reason == "OK") reason = "Internal Server Error";
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Server: iouring_activity_server\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    SendRaw(out.str());
    DisconnectAfterFlush();
}

void ActivitySession::SendRaw(const std::string& data) {
    auto result = Pool().Allocate(static_cast<std::uint32_t>(data.size()));
    if (!result) {
        Disconnect();
        return;
    }
    auto buf = std::move(*result);
    std::memcpy(buf->Writable().data(), data.data(), data.size());
    buf->Commit(static_cast<std::uint32_t>(data.size()));
    Send(std::move(buf));
}

void ActivitySession::SendWsTextOnRing(const std::string& text) {
    if (!websocket_ || Disconnecting()) {
        return;
    }
    SendWsFrame(0x1, text);
}

void ActivitySession::SendWsFrame(std::uint8_t opcode, std::string_view payload) {
    std::string frame;
    frame.reserve(payload.size() + 16);
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        frame.push_back(127);
        const auto len = static_cast<std::uint64_t>(payload.size());
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((len >> (i * 8)) & 0xff));
        }
    }
    frame.append(payload);
    SendRaw(frame);
}

} // namespace activity_server
