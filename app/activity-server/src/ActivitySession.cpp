#include "ActivitySession.h"

#include "ActivityHub.h"
#include "ActivityUtils.h"

#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/media/Hls.h>
#include <iouring_runtime/observability/Logging.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

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

std::string ReplaceAll(std::string text, std::string_view from, std::string_view to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

std::string CleanCookieText(std::string_view raw_cookie) {
    std::istringstream in{std::string(raw_cookie)};
    std::string line;
    std::vector<std::string> parts;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        if (line.starts_with("#") && !line.starts_with("#HttpOnly_")) continue;
        if (Lower(line).starts_with("cookie:")) {
            line = Trim(std::string_view(line).substr(line.find(':') + 1));
        }
        if (line.find('\t') != std::string::npos) {
            std::vector<std::string> columns;
            std::istringstream tab_in(line);
            std::string column;
            while (std::getline(tab_in, column, '\t')) {
                columns.push_back(column);
            }
            if (columns.size() >= 7) {
                parts.push_back(columns[columns.size() - 2] + "=" + columns.back());
            }
            continue;
        }
        parts.push_back(line);
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out << "; ";
        out << parts[i];
    }
    return out.str();
}

std::string CookieValue(std::string_view cookie, std::string_view key) {
    while (!cookie.empty()) {
        const auto semi = cookie.find(';');
        auto part = Trim(cookie.substr(0, semi));
        const auto eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == key) {
            return iouring_runtime::media::UrlDecode(part.substr(eq + 1));
        }
        if (semi == std::string_view::npos) break;
        cookie.remove_prefix(semi + 1);
    }
    return {};
}

std::vector<std::string> ExtractM3u8Urls(std::string text) {
    text = ReplaceAll(std::move(text), "\\/", "/");
    text = ReplaceAll(std::move(text), "\\u0026", "&");
    text = ReplaceAll(std::move(text), "\\u003d", "=");
    text = ReplaceAll(std::move(text), "\\u003D", "=");

    std::vector<std::string> urls;
    std::unordered_set<std::string> seen;
    std::size_t pos = 0;
    while (true) {
        const auto http = text.find("http", pos);
        if (http == std::string::npos) break;
        if (text.compare(http, 8, "https://") != 0 &&
            text.compare(http, 7, "http://") != 0) {
            pos = http + 4;
            continue;
        }

        auto end = http;
        while (end < text.size()) {
            const char ch = text[end];
            if (ch == '"' || ch == '\'' || ch == '<' || ch == '>' ||
                ch == '\\' || ch == '\r' || ch == '\n' || ch == ' ') {
                break;
            }
            ++end;
        }
        auto url = text.substr(http, end - http);
        while (!url.empty() && (url.back() == ',' || url.back() == ']' || url.back() == '}')) {
            url.pop_back();
        }
        if (Lower(url).find(".m3u8") != std::string::npos && seen.insert(url).second) {
            urls.push_back(std::move(url));
        }
        pos = end;
    }
    return urls;
}

std::string JsonArray(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ",";
        out << "\"" << JsonEscape(values[i]) << "\"";
    }
    out << "]";
    return out.str();
}

CommandResult CurlGetWithConfig(std::string_view url,
                                const std::vector<std::string>& headers) {
    const auto config_path = std::filesystem::temp_directory_path() /
        (RandomId("tving-curl-") + ".conf");
    std::ostringstream config;
    config << "silent\nshow-error\nlocation\nfail-with-body\nmax-time = 20\n";
    config << "url = \"" << ReplaceAll(std::string(url), "\"", "\\\"") << "\"\n";
    for (const auto& header : headers) {
        config << "header = \"" << ReplaceAll(header, "\"", "\\\"") << "\"\n";
    }

    const auto config_text = config.str();
    const int fd = ::open(config_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return {};
    }
    const auto* data = config_text.data();
    std::size_t remaining = config_text.size();
    while (remaining > 0) {
        const auto written = ::write(fd, data, remaining);
        if (written <= 0) {
            ::close(fd);
            std::error_code ec;
            std::filesystem::remove(config_path, ec);
            return {};
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    ::close(fd);

    const auto result = RunCommandCapture("curl --config " + ShellQuote(config_path.string()) +
                                          " 2>/dev/null");
    std::error_code ec;
    std::filesystem::remove(config_path, ec);
    return result;
}

struct TvingPlaylist {
    std::string id;
    std::string media_code;
    std::string manifest;
    std::string video_url;
    std::string audio_url;
    std::vector<std::string> headers;
    std::chrono::steady_clock::time_point created_at;
};

std::mutex g_tving_mu;
std::unordered_map<std::string, TvingPlaylist> g_tving_playlists;

std::string BuildTvingStreamInfoUrl(std::string_view media_code) {
    return "https://api.tving.com/v2/media/stream/info"
        "?screenCode=CSSD0100"
        "&networkCode=CSND0900"
        "&osCode=CSOD0900"
        "&teleCode=CSCD0900"
        "&apiKey=1e7952d0917d6aab1f0293a063697610"
        "&mediaCode=" + media::UrlEncode(media_code) +
        "&info=Y"
        "&callingFrom=HTML5"
        "&adReq=adproxy"
        "&uuid=2410204104-300a362f"
        "&deviceInfo=PC"
        "&noCache=" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
}

std::vector<std::string> BuildTvingHeaders(const std::string& cookie,
                                           const std::string& token,
                                           const std::string& auth_token,
                                           const std::string& access_token) {
    std::vector<std::string> headers = {
        "Accept: application/json, text/plain, */*",
        "Accept-Language: ko,en;q=0.9",
        "Origin: https://www.tving.com",
        "Referer: https://www.tving.com/",
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/147.0.0.0 Safari/537.36",
        "Authorization: Bearer " + token,
        "Cookie: " + cookie,
    };
    if (!auth_token.empty()) {
        headers.push_back("Auth-Token: " + auth_token);
    }
    if (!access_token.empty()) {
        headers.push_back("Access-Token: " + access_token);
    }
    return headers;
}

std::string ChooseTvingVideoUrl(const std::vector<std::string>& urls) {
    for (const auto& url : urls) {
        const auto lower = Lower(url);
        if (lower.find("audio") == std::string::npos &&
            lower.find("kbo2026audio") == std::string::npos) {
            return url;
        }
    }
    return urls.empty() ? std::string{} : urls.front();
}

std::string ChooseTvingAudioUrl(const std::vector<std::string>& urls) {
    for (const auto& url : urls) {
        const auto lower = Lower(url);
        if (lower.find("audio") != std::string::npos ||
            lower.find("kbo2026audio") != std::string::npos) {
            return url;
        }
    }
    return {};
}

std::string TvingProxyUrl(std::string_view remote_url, std::string_view tving_id) {
    return "/proxy/hls?url=" + media::UrlEncode(remote_url) +
           "&tving_id=" + media::UrlEncode(tving_id);
}

std::string BuildTvingMasterPlaylist(std::string_view video_url,
                                     std::string_view audio_url,
                                     std::string_view tving_id) {
    std::ostringstream out;
    out << "#EXTM3U\n"
        << "#EXT-X-VERSION:7\n"
        << "#EXT-X-INDEPENDENT-SEGMENTS\n";
    std::string codecs = "avc1.640028";
    if (!audio_url.empty()) {
        codecs = "avc1.640028,mp4a.40.2";
        out << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"tving-audio\",NAME=\"TVING\","
            << "DEFAULT=YES,AUTOSELECT=YES,URI=\""
            << TvingProxyUrl(audio_url, tving_id) << "\"\n";
    }
    out << "#EXT-X-STREAM-INF:BANDWIDTH=6000000,AVERAGE-BANDWIDTH=4500000,"
        << "CODECS=\"" << codecs << "\"";
    if (!audio_url.empty()) {
        out << ",AUDIO=\"tving-audio\"";
    }
    out << "\n" << TvingProxyUrl(video_url, tving_id) << "\n";
    return out.str();
}

std::string RewriteHlsManifestWithTving(std::string_view text,
                                        std::string_view base_url,
                                        std::string_view tving_id) {
    std::istringstream in{std::string(text)};
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            out << "\n";
            continue;
        }
        if (line.starts_with("#")) {
            std::string rewritten = line;
            std::size_t pos = 0;
            while ((pos = rewritten.find("URI=\"", pos)) != std::string::npos) {
                const auto start = pos + 5;
                const auto end = rewritten.find('"', start);
                if (end == std::string::npos) break;
                const auto uri = rewritten.substr(start, end - start);
                if (!uri.empty() && !uri.starts_with("data:")) {
                    const auto resolved = media::ResolveUrl(base_url, uri);
                    const auto proxied = TvingProxyUrl(resolved, tving_id);
                    rewritten.replace(start, end - start, proxied);
                    pos = start + proxied.size();
                } else {
                    pos = end + 1;
                }
            }
            out << rewritten << "\n";
            continue;
        }
        out << TvingProxyUrl(media::ResolveUrl(base_url, line), tving_id) << "\n";
    }
    return out.str();
}

std::optional<TvingPlaylist> GetTvingPlaylist(std::string_view tving_id) {
    std::lock_guard lock(g_tving_mu);
    auto it = g_tving_playlists.find(std::string(tving_id));
    if (it == g_tving_playlists.end()) {
        return std::nullopt;
    }
    if (std::chrono::steady_clock::now() - it->second.created_at > std::chrono::hours{6}) {
        g_tving_playlists.erase(it);
        return std::nullopt;
    }
    return it->second;
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
    ResetFileStream();
    if (websocket_ && !client_id_.empty()) {
        obs::LogInfo(obs::LogCategory::kSession,
                     "activity websocket disconnected client_id={} instance_id={} fd={}",
                     client_id_, instance_id_, Fd());
    }
    hub_.Remove(*this);
}

bool ActivitySession::HasPendingAppWork() const {
    return file_stream_.active;
}

void ActivitySession::OnSocketDrained() {
    if (file_stream_.active) {
        PumpFileStream();
    }
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
    if (req.method == "POST" && req.path == "/api/tving/probe") {
        ProbeTving(req.body);
        return;
    }
    if (req.method == "POST" && req.path == "/api/tving/cookie-play") {
        CookiePlayTving(req.body);
        return;
    }
    if ((req.method == "GET" || req.method == "HEAD") &&
        req.path.rfind("/api/tving/playlist/", 0) == 0) {
        ServeTvingPlaylist(req);
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
    if ((req.method == "GET" || req.method == "HEAD") && req.path.rfind("/hls/", 0) == 0) {
        ServeHls(req);
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

    if (type == "PING") {
        SendTextAsync("{\"type\":\"PONG\"}");
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

void ActivitySession::ServeHls(const HttpRequest& req) {
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(
        EnvString("ACTIVITY_DOWNLOAD_DIR", "/var/lib/iouring-runtime/activity-server/downloads")) / "hls");
    const auto rel = media::UrlDecode(std::string_view(req.path).substr(std::string_view("/hls/").size()));
    const auto target = std::filesystem::weakly_canonical(root / rel);
    if (target.string().rfind(root.string(), 0) != 0 || !std::filesystem::is_regular_file(target)) {
        SendHttp(404, "text/plain", "Not Found");
        return;
    }
    std::string type = "application/octet-stream";
    if (target.extension() == ".m3u8") type = "application/vnd.apple.mpegurl";
    if (target.extension() == ".ts") type = "video/mp2t";
    SendFileResponse(req, target, type);
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
    const auto tving_id = QueryParam(req.query, "tving_id");
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

    std::vector<std::string> tving_headers;
    if (!tving_id.empty()) {
        auto playlist = GetTvingPlaylist(tving_id);
        if (!playlist) {
            SendHttp(404, "application/json",
                     "{\"detail\":\"TVING playlist not found or expired\"}");
            return;
        }
        tving_headers = playlist->headers;
    }
    if (const auto range = Header(req, "range"); !range.empty()) {
        tving_headers.push_back("Range: " + range);
    }

    const auto upstream = tving_headers.empty()
        ? CurlGet(url)
        : CurlGetWithConfig(url, tving_headers);
    if (upstream.exit_code != 0) {
        SendHttp(502, "application/json", "{\"detail\":\"Upstream request failed\"}");
        return;
    }

    const auto content_type = media::ContentTypeForUrl(url);
    if (content_type == "application/vnd.apple.mpegurl" ||
        upstream.output.starts_with("#EXTM3U")) {
        SendHttp(200, "application/vnd.apple.mpegurl; charset=utf-8",
                 tving_id.empty()
                     ? media::RewriteHlsManifest(upstream.output, url)
                     : RewriteHlsManifestWithTving(upstream.output, url, tving_id));
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

void ActivitySession::ProbeTving(std::string_view body) {
    auto cookie = JsonString(body, "cookie_text").value_or("");
    if (cookie.empty()) {
        cookie = JsonString(body, "cookie").value_or("");
    }
    const auto media_code = JsonString(body, "media_code").value_or("C51850");
    if (cookie.empty()) {
        SendHttp(400, "application/json",
                 "{\"detail\":\"cookie_text is required\"}");
        return;
    }

    cookie = CleanCookieText(cookie);
    if (cookie.size() > 1024 * 1024) {
        SendHttp(400, "application/json",
                 "{\"detail\":\"cookie_text is too large\"}");
        return;
    }

    const auto token = CookieValue(cookie, "_tving_token");
    const auto auth_token = CookieValue(cookie, "authToken");
    const auto access_token = CookieValue(cookie, "accessToken");
    if (token.empty()) {
        SendHttp(400, "application/json",
                 "{\"detail\":\"cookie_text must contain _tving_token\"}");
        return;
    }

    const std::string stream_info_url =
        "https://api.tving.com/v2/media/stream/info"
        "?screenCode=CSSD0100"
        "&networkCode=CSND0900"
        "&osCode=CSOD0900"
        "&teleCode=CSCD0900"
        "&apiKey=1e7952d0917d6aab1f0293a063697610"
        "&mediaCode=" + media::UrlEncode(media_code) +
        "&info=Y"
        "&callingFrom=HTML5"
        "&adReq=adproxy"
        "&uuid=2410204104-300a362f"
        "&deviceInfo=PC"
        "&noCache=" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

    std::vector<std::string> headers = {
        "Accept: application/json, text/plain, */*",
        "Accept-Language: ko,en;q=0.9",
        "Origin: https://www.tving.com",
        "Referer: https://www.tving.com/",
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/147.0.0.0 Safari/537.36",
        "Authorization: Bearer " + token,
        "Cookie: " + cookie,
    };
    if (!auth_token.empty()) {
        headers.push_back("Auth-Token: " + auth_token);
    }
    if (!access_token.empty()) {
        headers.push_back("Access-Token: " + access_token);
    }

    const auto upstream = CurlGetWithConfig(stream_info_url, headers);
    if (upstream.exit_code != 0) {
        SendHttp(502, "application/json",
                 "{\"detail\":\"TVING stream info request failed\","
                 "\"cookie_keys\":{\"_tving_token\":true,\"authToken\":" +
                 std::string(auth_token.empty() ? "false" : "true") +
                 ",\"accessToken\":" +
                 std::string(access_token.empty() ? "false" : "true") + "}}");
        return;
    }

    const auto urls = ExtractM3u8Urls(upstream.output);
    SendHttp(200, "application/json",
             "{\"status\":\"ok\","
             "\"media_code\":\"" + JsonEscape(media_code) + "\","
             "\"response_bytes\":" + std::to_string(upstream.output.size()) + ","
             "\"m3u8_count\":" + std::to_string(urls.size()) + ","
             "\"m3u8_urls\":" + JsonArray(urls) + ","
             "\"cookie_keys\":{\"_tving_token\":true,\"authToken\":" +
             std::string(auth_token.empty() ? "false" : "true") +
             ",\"accessToken\":" +
             std::string(access_token.empty() ? "false" : "true") + "}}");
}

void ActivitySession::CookiePlayTving(std::string_view body) {
    auto cookie = JsonString(body, "cookie_text").value_or("");
    if (cookie.empty()) {
        cookie = JsonString(body, "cookie").value_or("");
    }
    const auto media_code = JsonString(body, "media_code").value_or("C51850");
    const auto instance_id = JsonString(body, "instance_id").value_or("default");
    if (cookie.empty()) {
        SendHttp(400, "application/json",
                 "{\"detail\":\"cookie_text is required\"}");
        return;
    }

    cookie = CleanCookieText(cookie);
    if (cookie.size() > 1024 * 1024) {
        SendHttp(400, "application/json",
                 "{\"detail\":\"cookie_text is too large\"}");
        return;
    }

    const auto token = CookieValue(cookie, "_tving_token");
    const auto auth_token = CookieValue(cookie, "authToken");
    const auto access_token = CookieValue(cookie, "accessToken");
    if (token.empty()) {
        SendHttp(400, "application/json",
                 "{\"detail\":\"cookie_text must contain _tving_token\"}");
        return;
    }

    auto headers = BuildTvingHeaders(cookie, token, auth_token, access_token);
    const auto upstream = CurlGetWithConfig(BuildTvingStreamInfoUrl(media_code), headers);
    if (upstream.exit_code != 0) {
        SendHttp(502, "application/json",
                 "{\"detail\":\"TVING stream info request failed\"}");
        return;
    }

    const auto urls = ExtractM3u8Urls(upstream.output);
    const auto video_url = ChooseTvingVideoUrl(urls);
    const auto audio_url = ChooseTvingAudioUrl(urls);
    if (video_url.empty()) {
        SendHttp(502, "application/json",
                 "{\"detail\":\"TVING response did not contain a usable video m3u8\","
                 "\"m3u8_count\":" + std::to_string(urls.size()) + ","
                 "\"m3u8_urls\":" + JsonArray(urls) + "}");
        return;
    }

    TvingPlaylist playlist;
    playlist.id = RandomId("tving_");
    playlist.media_code = media_code;
    playlist.video_url = video_url;
    playlist.audio_url = audio_url;
    playlist.headers = std::move(headers);
    playlist.created_at = std::chrono::steady_clock::now();
    playlist.manifest = BuildTvingMasterPlaylist(
        playlist.video_url, playlist.audio_url, playlist.id);

    QueueEntry entry;
    entry.id = "tving_" + playlist.id;
    entry.url = "/api/tving/playlist/" + playlist.id + ".m3u8";
    entry.source = "tving";
    entry.title = "TVING " + media_code;
    entry.ext = "m3u8";

    {
        std::lock_guard lock(g_tving_mu);
        g_tving_playlists[playlist.id] = playlist;
    }

    {
        std::lock_guard lock(hub_.mu_);
        auto& state = hub_.StateLocked(instance_id.empty() ? "default" : instance_id);
        state.current_video_url = entry.url;
        state.metadata = entry;
        state.is_playing = true;
        state.current_time = 0.0;
        state.start_at = std::chrono::steady_clock::now();
    }

    hub_.BroadcastState(instance_id.empty() ? "default" : instance_id);
    SendHttp(200, "application/json",
             "{\"status\":\"success\","
             "\"entry\":" + hub_.EntryJson(entry) + ","
             "\"autostart\":true,"
             "\"playlist\":{\"id\":\"" + JsonEscape(playlist.id) + "\","
             "\"url\":\"" + JsonEscape(entry.url) + "\","
             "\"has_audio\":" + std::string(audio_url.empty() ? "false" : "true") + ","
             "\"video_url\":\"" + JsonEscape(video_url) + "\","
             "\"audio_url\":\"" + JsonEscape(audio_url) + "\","
             "\"m3u8_count\":" + std::to_string(urls.size()) + ","
             "\"m3u8_urls\":" + JsonArray(urls) + "}}");
}

void ActivitySession::ServeTvingPlaylist(const HttpRequest& req) {
    const std::string prefix = "/api/tving/playlist/";
    auto id = req.path.substr(prefix.size());
    if (id.ends_with(".m3u8")) {
        id.resize(id.size() - std::string(".m3u8").size());
    }
    const auto playlist = GetTvingPlaylist(id);
    if (!playlist) {
        SendHttp(404, "application/json",
                 "{\"detail\":\"TVING playlist not found or expired\"}");
        return;
    }
    SendHttp(200, "application/vnd.apple.mpegurl; charset=utf-8",
             req.method == "HEAD" ? std::string{} : playlist->manifest);
}

void ActivitySession::SendFileResponse(const HttpRequest& req, const std::filesystem::path& path,
                                       std::string_view content_type) {
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        SendHttp(404, "text/plain", "Not Found");
        return;
    }

    std::uint64_t start = 0;
    std::uint64_t end = file_size == 0 ? 0 : file_size - 1;
    bool partial = false;
    const auto range = Header(req, "range");
    if (file_size > 0 && range.rfind("bytes=", 0) == 0) {
        const auto spec = range.substr(6);
        const auto dash = spec.find('-');
        try {
            if (dash != std::string::npos && dash > 0) {
                start = std::stoull(spec.substr(0, dash));
                if (dash + 1 < spec.size()) {
                    end = std::min<std::uint64_t>(std::stoull(spec.substr(dash + 1)), file_size - 1);
                }
                partial = start <= end && start < file_size;
            } else if (dash == 0 && spec.size() > 1) {
                const auto suffix = std::min<std::uint64_t>(std::stoull(spec.substr(1)), file_size);
                start = file_size - suffix;
                end = file_size - 1;
                partial = suffix > 0;
            }
        } catch (...) {
            partial = false;
            start = 0;
            end = file_size - 1;
        }
        if (!partial) {
            std::ostringstream out;
            out << "HTTP/1.1 416 Range Not Satisfiable\r\n"
                << "Server: iouring_activity_server\r\n"
                << "Content-Range: bytes */" << file_size << "\r\n"
                << "Content-Length: 0\r\n"
                << "Access-Control-Allow-Origin: *\r\n"
                << "Accept-Ranges: bytes\r\n"
                << "Connection: close\r\n\r\n";
            SendRaw(out.str());
            DisconnectAfterFlush();
            return;
        }
    }

    const auto content_length = file_size == 0 ? 0 : (end - start + 1);
    const int status = partial ? 206 : 200;
    std::ostringstream headers;
    headers << "HTTP/1.1 " << status << (partial ? " Partial Content" : " OK") << "\r\n"
            << "Server: iouring_activity_server\r\n"
            << "Content-Type: " << content_type << "\r\n"
            << "Content-Length: " << content_length << "\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Accept-Ranges: bytes\r\n";
    if (partial) {
        headers << "Content-Range: bytes " << start << '-' << end << '/' << file_size << "\r\n";
    }
    headers << "Connection: close\r\n\r\n";

    if (req.method == "HEAD" || content_length == 0) {
        SendRaw(headers.str());
        DisconnectAfterFlush();
        return;
    }

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        SendHttp(404, "text/plain", "Not Found");
        return;
    }
    if (::lseek(fd, static_cast<off_t>(start), SEEK_SET) < 0) {
        ::close(fd);
        SendHttp(404, "text/plain", "Not Found");
        return;
    }

    file_stream_.fd.Reset(fd);
    file_stream_.remaining_bytes = content_length;
    file_stream_.active = true;

    SendRaw(headers.str());
    PumpFileStream();
    DisconnectAfterFlush();
}

bool ActivitySession::PumpFileStream() {
    if (!file_stream_.active) {
        return true;
    }
    if (file_stream_.remaining_bytes == 0) {
        ResetFileStream();
        MaybeDisconnectAfterFlush();
        return true;
    }

    for (std::uint32_t i = 0;
         i < file_stream_.max_chunks_per_write && file_stream_.remaining_bytes > 0;
         ++i) {
        const auto next_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            file_stream_.remaining_bytes, file_stream_.chunk_size));
        auto buffer_result = Pool().Allocate(next_size);
        if (!buffer_result) {
            ResetFileStream();
            Disconnect();
            return false;
        }

        auto buffer = std::move(*buffer_result);
        auto writable = buffer->Writable();
        std::size_t total_read = 0;
        while (total_read < next_size) {
            const auto n = ::read(file_stream_.fd.Get(),
                                  writable.data() + total_read,
                                  next_size - total_read);
            if (n > 0) {
                total_read += static_cast<std::size_t>(n);
                continue;
            }
            if (n == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            ResetFileStream();
            Disconnect();
            return false;
        }

        if (total_read == 0) {
            ResetFileStream();
            MaybeDisconnectAfterFlush();
            return true;
        }

        buffer->Commit(static_cast<std::uint32_t>(total_read));
        file_stream_.remaining_bytes -= total_read;
        if (!Send(std::move(buffer)).has_value()) {
            ResetFileStream();
            return false;
        }
    }

    if (file_stream_.remaining_bytes == 0) {
        ResetFileStream();
        MaybeDisconnectAfterFlush();
    }
    return true;
}

void ActivitySession::ResetFileStream() {
    file_stream_.fd.Reset();
    file_stream_.remaining_bytes = 0;
    file_stream_.active = false;
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
