#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/Listener.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/observability/Logging.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/wait.h>

namespace core = iouring_runtime::core;
namespace io = iouring_runtime::core::io;
namespace ring = iouring_runtime::core::ring;
namespace obs = iouring_runtime::observability;

namespace {

std::atomic<bool> g_stop_requested{false};

void HandleSignal(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

std::string EnvString(const char* name, std::string fallback) {
    if (const char* raw = std::getenv(name)) {
        return raw;
    }
    return fallback;
}

std::uint16_t EnvPort(const char* name, std::uint16_t fallback) {
    if (const char* raw = std::getenv(name)) {
        try {
            const auto value = std::stoul(raw);
            if (value <= 65535) {
                return static_cast<std::uint16_t>(value);
            }
        } catch (...) {
        }
    }
    return fallback;
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            out += static_cast<unsigned char>(ch) < 0x20 ? ' ' : ch;
            break;
        }
    }
    return out;
}

std::string Lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char ch : text) {
        out += static_cast<char>(std::tolower(ch));
    }
    return out;
}

std::string Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string UrlDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
            continue;
        }
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = HexValue(text[i + 1]);
            const int lo = HexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::string UrlEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const unsigned char ch : text) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out += static_cast<char>(ch);
        } else {
            out += '%';
            out += kHex[ch >> 4];
            out += kHex[ch & 0x0f];
        }
    }
    return out;
}

std::string QueryParam(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto part = query.substr(0, amp);
        const auto eq = part.find('=');
        const auto key = eq == std::string_view::npos ? part : part.substr(0, eq);
        if (key == name) {
            return UrlDecode(eq == std::string_view::npos ? std::string_view{} : part.substr(eq + 1));
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return {};
}

std::optional<std::string> JsonString(std::string_view json, std::string_view key) {
    const auto needle = "\"" + std::string(key) + "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find('"', pos + 1);
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;

    std::string out;
    while (pos < json.size()) {
        const char ch = json[pos++];
        if (ch == '"') return out;
        if (ch == '\\' && pos < json.size()) {
            const char escaped = json[pos++];
            switch (escaped) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += escaped; break;
            }
            continue;
        }
        out += ch;
    }
    return std::nullopt;
}

std::optional<double> JsonNumber(std::string_view json, std::string_view key) {
    const auto needle = "\"" + std::string(key) + "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    const auto start = pos;
    while (pos < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[pos])) ||
            json[pos] == '.' || json[pos] == '-')) {
        ++pos;
    }
    if (start == pos) return std::nullopt;
    try {
        return std::stod(std::string(json.substr(start, pos - start)));
    } catch (...) {
        return std::nullopt;
    }
}

std::string RandomId(std::string_view prefix) {
    static std::atomic<std::uint64_t> seq{1};
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream out;
    out << prefix << std::hex
        << std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count()
        << seq.fetch_add(1, std::memory_order_relaxed)
        << (rng() & 0xffff);
    return out.str();
}

std::string Base64(const unsigned char* data, std::size_t size) {
    std::string out;
    out.resize(4 * ((size + 2) / 3));
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
    if (written < 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

std::string WebSocketAccept(std::string_view key) {
    static constexpr std::string_view kMagic =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string material = std::string(key) + std::string(kMagic);
    unsigned char digest[SHA_DIGEST_LENGTH] = {};
    SHA1(reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest);
    return Base64(digest, sizeof(digest));
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::string ShellQuote(std::string_view value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

int RunCommand(const std::string& command) {
    return std::system(command.c_str());
}

struct CommandResult {
    int exit_code = -1;
    std::string output;
};

CommandResult RunCommandCapture(const std::string& command) {
    CommandResult result;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return result;
    }

    char buffer[8192];
    while (true) {
        const auto n = std::fread(buffer, 1, sizeof(buffer), pipe);
        if (n > 0) {
            result.output.append(buffer, n);
        }
        if (n < sizeof(buffer)) {
            if (std::feof(pipe)) break;
            if (std::ferror(pipe)) break;
        }
    }

    const int status = ::pclose(pipe);
    if (status >= 0 && WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }
    return result;
}

bool IsHttpUrl(std::string_view url) {
    return url.starts_with("http://") || url.starts_with("https://");
}

std::string OriginOf(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) return {};
    const auto host_start = scheme + 3;
    const auto path_start = url.find('/', host_start);
    return std::string(url.substr(0, path_start == std::string_view::npos
        ? std::string_view::npos
        : path_start));
}

std::string ResolveUrl(std::string_view base, std::string_view value) {
    if (IsHttpUrl(value)) {
        return std::string(value);
    }
    const auto origin = OriginOf(base);
    if (value.starts_with("//")) {
        const auto scheme = base.substr(0, base.find(':'));
        return std::string(scheme) + ":" + std::string(value);
    }
    if (value.starts_with('/')) {
        return origin + std::string(value);
    }

    const auto slash = base.rfind('/');
    if (slash == std::string_view::npos || base.substr(0, slash).find("://") == std::string_view::npos) {
        return origin + "/" + std::string(value);
    }
    return std::string(base.substr(0, slash + 1)) + std::string(value);
}

std::string ProxyHlsUrl(std::string_view remote_url) {
    return "/proxy/hls?url=" + UrlEncode(remote_url);
}

bool ShouldProxyManifestUri(std::string_view uri) {
    return !uri.empty() && !uri.starts_with("data:");
}

std::string RewriteHlsManifest(std::string_view text, std::string_view base_url) {
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
                if (ShouldProxyManifestUri(uri)) {
                    const auto resolved = ResolveUrl(base_url, uri);
                    const auto proxied = ProxyHlsUrl(resolved);
                    rewritten.replace(start, end - start, proxied);
                    pos = start + proxied.size();
                } else {
                    pos = end + 1;
                }
            }
            out << rewritten << "\n";
            continue;
        }

        out << ProxyHlsUrl(ResolveUrl(base_url, line)) << "\n";
    }
    return out.str();
}

CommandResult CurlGet(std::string_view url, std::string_view extra_args = {}) {
    return RunCommandCapture("curl -LfsS --max-time 30 -A " +
                             ShellQuote("Mozilla/5.0 Cocoatube/1.0") + " " +
                             std::string(extra_args) + " " + ShellQuote(url) +
                             " 2>/dev/null");
}

std::string ContentTypeForUrl(std::string_view url) {
    const auto lower = Lower(url);
    if (lower.ends_with(".m3u8")) return "application/vnd.apple.mpegurl";
    if (lower.ends_with(".mpd")) return "application/dash+xml";
    if (lower.ends_with(".ts")) return "video/mp2t";
    if (lower.ends_with(".m4s")) return "video/iso.segment";
    if (lower.ends_with(".mp4")) return "video/mp4";
    if (lower.ends_with(".jpg") || lower.ends_with(".jpeg")) return "image/jpeg";
    if (lower.ends_with(".png")) return "image/png";
    if (lower.ends_with(".webp")) return "image/webp";
    if (lower.ends_with(".gif")) return "image/gif";
    return "application/octet-stream";
}

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

std::string Header(const HttpRequest& req, std::string_view name) {
    const auto it = req.headers.find(std::string(name));
    return it == req.headers.end() ? std::string{} : it->second;
}

struct QueueEntry {
    std::string id;
    std::string url;
    std::string path;
    std::string source = "youtube";
    std::string title;
    std::string thumbnail;
    std::string ext = "m3u8";
    double duration = 0.0;
};

struct InstanceState {
    std::string current_video_url;
    QueueEntry metadata;
    std::vector<QueueEntry> queue;
    bool is_playing = false;
    double current_time = 0.0;
    std::chrono::steady_clock::time_point start_at{};
};

struct DownloadTask {
    std::string id;
    std::string url;
    std::string status = "pending";
    std::string error;
    QueueEntry entry;
    int progress = 0;
    bool force_play = false;
};

class ActivitySession;

class ActivityHub {
public:
    void Register(ActivitySession& session, std::shared_ptr<ActivitySession> self,
                  std::string instance_id, std::string client_id);
    void Remove(ActivitySession& session);
    void BroadcastText(std::string_view instance_id, const std::string& text);
    void BroadcastState(std::string_view instance_id,
                        std::optional<std::string> origin_client_id = std::nullopt);
    void BroadcastPresence(std::string_view instance_id);
    std::string QueueJson(std::string_view instance_id);
    std::string StatePayloadJson(std::string_view instance_id);
    std::string CreateDownload(std::string url, std::string instance_id,
                               bool force_play = false);
    std::string TaskJson(std::string_view task_id);

private:
    friend class ActivitySession;

    struct Client {
        std::weak_ptr<ActivitySession> session;
        std::string instance_id;
        std::string client_id;
    };

    void DownloadWorker(std::string task_id, std::string instance_id);
    InstanceState& StateLocked(std::string_view instance_id);
    std::vector<std::shared_ptr<ActivitySession>> SessionsLocked(std::string_view instance_id);
    std::string ClientsJsonLocked(std::string_view instance_id);
    std::string EntryJson(const QueueEntry& entry);

    std::mutex mu_;
    std::unordered_map<ActivitySession*, Client> clients_;
    std::unordered_map<std::string, InstanceState> states_;
    std::unordered_map<std::string, DownloadTask> tasks_;
};

ActivityHub g_hub;

class ActivitySession : public io::Session {
public:
    ActivitySession(int fd, ring::IoRing& io_ring, core::buffer::BufferPool& pool)
        : Session(fd, io_ring, pool, 4096) {
        SetInactivityTimeout(std::chrono::minutes{10});
    }

    void SendTextAsync(std::string text) {
        auto self = std::static_pointer_cast<ActivitySession>(shared_from_this());
        Ring().RunOnRing([self, text = std::move(text)] {
            self->SendWsTextOnRing(text);
        });
    }

protected:
    void OnRecv(std::span<const std::byte> data) override {
        const auto* ptr = reinterpret_cast<const char*>(data.data());
        if (websocket_) {
            ws_buffer_.append(ptr, data.size());
            ProcessWsBuffer();
            return;
        }
        http_buffer_.append(ptr, data.size());
        ProcessHttpBuffer();
    }

    void OnDisconnected() override {
        g_hub.Remove(*this);
    }

private:
    void ProcessHttpBuffer() {
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

    std::optional<HttpRequest> TryParseHttp() {
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
            req.headers[Lower(line.substr(0, colon))] = Trim(std::string_view(line).substr(colon + 1));
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

    void HandleHttp(const HttpRequest& req) {
        if (req.method == "GET" && req.path == "/ws") {
            UpgradeWebSocket(req);
            return;
        }
        if (req.method == "GET" && req.path == "/api/queue") {
            const auto instance_id = QueryParam(req.query, "instance_id").empty()
                ? "default"
                : QueryParam(req.query, "instance_id");
            SendHttp(200, "application/json", g_hub.QueueJson(instance_id));
            return;
        }
        if (req.method == "POST" && req.path == "/api/download") {
            const auto url = JsonString(req.body, "url").value_or("");
            const auto instance_id = JsonString(req.body, "instance_id").value_or("default");
            if (url.empty()) {
                SendHttp(400, "application/json", "{\"detail\":\"url is required\"}");
                return;
            }
            const auto task_id = g_hub.CreateDownload(url, instance_id.empty() ? "default" : instance_id);
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
            const auto task_id = g_hub.CreateDownload(
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
            const auto body = g_hub.TaskJson(task_id);
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

        SendHttp(404, "application/json", "{\"detail\":\"Not found\"}");
    }

    void UpgradeWebSocket(const HttpRequest& req) {
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

    void ProcessWsBuffer() {
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

    void HandleWsText(std::string_view text) {
        const auto type = JsonString(text, "type").value_or("");
        const auto client_id = JsonString(text, "client_id").value_or(client_id_);

        if (type == "HELLO") {
            instance_id_ = JsonString(text, "instance_id").value_or("");
            if (instance_id_.empty()) instance_id_ = "default";
            client_id_ = client_id.empty() ? RandomId("client_") : client_id;
            g_hub.Register(*this,
                           std::static_pointer_cast<ActivitySession>(shared_from_this()),
                           instance_id_, client_id_);
            SendTextAsync("{\"type\":\"STATE_UPDATE\",\"seq\":1,\"origin\":null,\"payload\":" +
                          g_hub.StatePayloadJson(instance_id_) + "}");
            g_hub.BroadcastPresence(instance_id_);
            return;
        }

        if (type == "CHAT_MESSAGE") {
            const auto payload_text = JsonString(text, "text").value_or("");
            const auto sender = JsonString(text, "sender").value_or(client_id_.empty() ? "Guest" : client_id_);
            if (!payload_text.empty()) {
                const auto now = std::chrono::duration<double>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                std::ostringstream out;
                out << "{\"type\":\"CHAT_MESSAGE\",\"origin\":{\"client_id\":\""
                    << JsonEscape(client_id_) << "\"},\"payload\":{\"text\":\""
                    << JsonEscape(payload_text.substr(0, 500)) << "\",\"sender\":\""
                    << JsonEscape(sender.substr(0, 32)) << "\",\"server_ts\":"
                    << std::fixed << std::setprecision(3) << now << "}}";
                g_hub.BroadcastText(instance_id_, out.str());
            }
            return;
        }

        if (type == "REQUEST_SYNC") {
            ApplySync(text, client_id);
        }
    }

    void ApplySync(std::string_view text, std::string_view client_id) {
        const auto time = JsonNumber(text, "time").value_or(0.0);
        const auto reason = JsonString(text, "reason").value_or("");
        {
            std::lock_guard lock(sync_mu_);
            last_sync_time_ = time;
            last_sync_playing_ = reason != "pause_button";
        }
        (void)client_id;
        g_hub.BroadcastState(instance_id_, std::string(client_id));
    }

    void AdvanceQueue(const std::string& instance_id);
    void RemoveQueue(std::string_view body);
    void ServeHls(const std::string& path);
    void ProxyThumbnail(const HttpRequest& req);
    void ProxyHls(const HttpRequest& req);
    void ExchangeDiscordToken(std::string_view body);

    void SendHttp(int status, std::string_view content_type, std::string body) {
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

    void SendRaw(const std::string& data) {
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

    void SendWsTextOnRing(const std::string& text) {
        if (!websocket_ || Disconnecting()) {
            return;
        }
        SendWsFrame(0x1, text);
    }

    void SendWsFrame(std::uint8_t opcode, std::string_view payload) {
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

    std::string http_buffer_;
    std::string ws_buffer_;
    std::string instance_id_ = "default";
    std::string client_id_;
    bool websocket_ = false;

    static inline std::mutex sync_mu_;
    static inline double last_sync_time_ = 0.0;
    static inline bool last_sync_playing_ = true;
};

InstanceState& ActivityHub::StateLocked(std::string_view instance_id) {
    return states_[std::string(instance_id.empty() ? "default" : instance_id)];
}

std::vector<std::shared_ptr<ActivitySession>>
ActivityHub::SessionsLocked(std::string_view instance_id) {
    std::vector<std::shared_ptr<ActivitySession>> sessions;
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (auto session = it->second.session.lock()) {
            if (it->second.instance_id == instance_id) {
                sessions.push_back(std::move(session));
            }
            ++it;
        } else {
            it = clients_.erase(it);
        }
    }
    return sessions;
}

std::string ActivityHub::ClientsJsonLocked(std::string_view instance_id) {
    std::string out = "[";
    bool first = true;
    for (const auto& [_, client] : clients_) {
        if (client.instance_id != instance_id) continue;
        if (!first) out += ',';
        first = false;
        out += "{\"client_id\":\"" + JsonEscape(client.client_id) + "\"}";
    }
    out += "]";
    return out;
}

std::string ActivityHub::EntryJson(const QueueEntry& entry) {
    std::ostringstream out;
    out << "{\"id\":\"" << JsonEscape(entry.id)
        << "\",\"url\":\"" << JsonEscape(entry.url)
        << "\",\"path\":\"" << JsonEscape(entry.path)
        << "\",\"source\":\"" << JsonEscape(entry.source)
        << "\",\"title\":\"" << JsonEscape(entry.title)
        << "\",\"duration\":" << entry.duration
        << ",\"thumbnail\":\"" << JsonEscape(entry.thumbnail)
        << "\",\"ext\":\"" << JsonEscape(entry.ext) << "\"}";
    return out.str();
}

void ActivityHub::Register(ActivitySession& session, std::shared_ptr<ActivitySession> self,
                           std::string instance_id, std::string client_id) {
    std::lock_guard lock(mu_);
    clients_[&session] = Client{std::move(self), std::move(instance_id), std::move(client_id)};
}

void ActivityHub::Remove(ActivitySession& session) {
    std::string instance_id;
    {
        std::lock_guard lock(mu_);
        if (auto it = clients_.find(&session); it != clients_.end()) {
            instance_id = it->second.instance_id;
            clients_.erase(it);
        }
    }
    if (!instance_id.empty()) {
        BroadcastPresence(instance_id);
    }
}

void ActivityHub::BroadcastText(std::string_view instance_id, const std::string& text) {
    std::vector<std::shared_ptr<ActivitySession>> sessions;
    {
        std::lock_guard lock(mu_);
        sessions = SessionsLocked(instance_id);
    }
    for (auto& session : sessions) {
        session->SendTextAsync(text);
    }
}

std::string ActivityHub::StatePayloadJson(std::string_view instance_id) {
    std::lock_guard lock(mu_);
    auto& state = StateLocked(instance_id);
    const auto clients = ClientsJsonLocked(instance_id);
    double media_time = state.current_time;
    if (state.is_playing && state.start_at.time_since_epoch().count() != 0) {
        media_time += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - state.start_at).count();
    }
    std::string queue = "[";
    for (std::size_t i = 0; i < state.queue.size(); ++i) {
        if (i) queue += ',';
        queue += EntryJson(state.queue[i]);
    }
    queue += "]";

    std::ostringstream out;
    out << "{\"playback\":{\"is_playing\":" << (state.is_playing ? "true" : "false")
        << ",\"time\":" << std::fixed << std::setprecision(3) << media_time
        << ",\"paused_time\":" << state.current_time
        << ",\"server_now\":" << std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count()
        << "},\"media\":";
    if (state.current_video_url.empty()) {
        out << "null";
    } else {
        out << EntryJson(state.metadata);
    }
    out << ",\"queue\":" << queue
        << ",\"clients\":" << clients
        << ",\"client_count\":" << std::count_if(
               clients_.begin(), clients_.end(), [&](const auto& item) {
                   return item.second.instance_id == instance_id;
               })
        << "}";
    return out.str();
}

void ActivityHub::BroadcastState(std::string_view instance_id,
                                 std::optional<std::string> origin_client_id) {
    std::string text = "{\"type\":\"STATE_UPDATE\",\"seq\":1,\"origin\":";
    if (origin_client_id) {
        text += "{\"client_id\":\"" + JsonEscape(*origin_client_id) + "\"}";
    } else {
        text += "null";
    }
    text += ",\"payload\":" + StatePayloadJson(instance_id) + "}";
    BroadcastText(instance_id, text);
}

void ActivityHub::BroadcastPresence(std::string_view instance_id) {
    std::string payload;
    {
        std::lock_guard lock(mu_);
        const auto clients = ClientsJsonLocked(instance_id);
        const auto count = std::count_if(clients_.begin(), clients_.end(), [&](const auto& item) {
            return item.second.instance_id == instance_id;
        });
        payload = "{\"type\":\"PRESENCE_UPDATE\",\"payload\":{\"clients\":" +
                  clients + ",\"client_count\":" + std::to_string(count) + "}}";
    }
    BroadcastText(instance_id, payload);
}

std::string ActivityHub::QueueJson(std::string_view instance_id) {
    std::lock_guard lock(mu_);
    auto& state = StateLocked(instance_id);
    std::string out = "{\"queue\":[";
    for (std::size_t i = 0; i < state.queue.size(); ++i) {
        if (i) out += ',';
        out += EntryJson(state.queue[i]);
    }
    out += "]}";
    return out;
}

std::string ActivityHub::CreateDownload(std::string url, std::string instance_id,
                                        bool force_play) {
    const auto task_id = RandomId("dl_");
    {
        std::lock_guard lock(mu_);
        DownloadTask task;
        task.id = task_id;
        task.url = std::move(url);
        task.force_play = force_play;
        tasks_[task_id] = std::move(task);
    }
    std::thread([this, task_id, instance_id = std::move(instance_id)] {
        DownloadWorker(task_id, instance_id);
    }).detach();
    return task_id;
}

std::string ActivityHub::TaskJson(std::string_view task_id) {
    std::lock_guard lock(mu_);
    auto it = tasks_.find(std::string(task_id));
    if (it == tasks_.end()) return {};
    const auto& task = it->second;
    std::string out = "{\"task_id\":\"" + JsonEscape(task.id) +
        "\",\"status\":\"" + JsonEscape(task.status) +
        "\",\"url\":\"" + JsonEscape(task.url) +
        "\",\"progress\":" + std::to_string(task.progress);
    if (!task.error.empty()) {
        out += ",\"error\":\"" + JsonEscape(task.error) + "\"";
    }
    if (!task.entry.id.empty()) {
        out += ",\"entry\":" + EntryJson(task.entry);
    }
    out += "}";
    return out;
}

void ActivityHub::DownloadWorker(std::string task_id, std::string instance_id) {
    const auto downloads = std::filesystem::path(
        EnvString("ACTIVITY_DOWNLOAD_DIR", "/var/lib/iouring-runtime/activity-server/downloads"));
    const auto local_dir = downloads / "local" / "youtube";
    const auto hls_dir = downloads / "hls" / task_id;
    try {
        std::filesystem::create_directories(local_dir);
        std::filesystem::create_directories(hls_dir);
    } catch (const std::exception& ex) {
        {
            std::lock_guard lock(mu_);
            auto& task = tasks_[task_id];
            task.status = "failed";
            task.error = std::string("download directory error: ") + ex.what();
        }
        BroadcastText(instance_id, "{\"type\":\"DOWNLOAD_FAILED\",\"task_id\":\"" +
            JsonEscape(task_id) + "\",\"error\":\"download directory error\"}");
        return;
    }

    std::string url;
    {
        std::lock_guard lock(mu_);
        auto& task = tasks_[task_id];
        task.status = "downloading";
        task.progress = 5;
        url = task.url;
    }
    BroadcastText(instance_id, "{\"type\":\"DOWNLOAD_PROGRESS\",\"task_id\":\"" +
        JsonEscape(task_id) + "\",\"status\":\"downloading\",\"url\":\"" + JsonEscape(url) + "\"}");

    const auto output_template = (local_dir / (task_id + ".%(ext)s")).string();
    const auto media_file = (local_dir / (task_id + ".mp4")).string();
    const std::string ytdlp =
        "yt-dlp -f " +
        ShellQuote("bestvideo[vcodec^=avc1][ext=mp4][height<=1080]+bestaudio[ext=m4a]/best[ext=mp4][height<=1080]/best[height<=1080]") +
        " --merge-output-format mp4 --no-playlist -o " + ShellQuote(output_template) +
        " " + ShellQuote(url);

    if (RunCommand(ytdlp) != 0 || !std::filesystem::exists(media_file)) {
        std::lock_guard lock(mu_);
        auto& task = tasks_[task_id];
        task.status = "failed";
        task.error = "yt-dlp failed";
        BroadcastText(instance_id, "{\"type\":\"DOWNLOAD_FAILED\",\"task_id\":\"" +
            JsonEscape(task_id) + "\",\"error\":\"yt-dlp failed\"}");
        return;
    }

    const auto playlist = hls_dir / "index.m3u8";
    const auto segment_pattern = hls_dir / "seg_%05d.ts";
    const auto ffmpeg_copy =
        "ffmpeg -y -i " + ShellQuote(media_file) +
        " -c copy -f hls -hls_time 6 -hls_playlist_type vod -hls_segment_filename " +
        ShellQuote(segment_pattern.string()) + " " + ShellQuote(playlist.string());
    const auto ffmpeg_transcode =
        "ffmpeg -y -i " + ShellQuote(media_file) +
        " -c:v libx264 -preset veryfast -c:a aac -f hls -hls_time 6 -hls_playlist_type vod -hls_segment_filename " +
        ShellQuote(segment_pattern.string()) + " " + ShellQuote(playlist.string());
    if ((RunCommand(ffmpeg_copy) != 0 || !std::filesystem::exists(playlist)) &&
        (RunCommand(ffmpeg_transcode) != 0 || !std::filesystem::exists(playlist))) {
        std::lock_guard lock(mu_);
        auto& task = tasks_[task_id];
        task.status = "failed";
        task.error = "ffmpeg hls failed";
        BroadcastText(instance_id, "{\"type\":\"DOWNLOAD_FAILED\",\"task_id\":\"" +
            JsonEscape(task_id) + "\",\"error\":\"ffmpeg hls failed\"}");
        return;
    }

    QueueEntry entry;
    entry.id = task_id;
    entry.url = "/hls/" + task_id + "/index.m3u8";
    entry.path = playlist.string();
    entry.title = url;

    bool autostart = false;
    bool force_play = false;
    {
        std::lock_guard lock(mu_);
        auto& task = tasks_[task_id];
        task.status = "completed";
        task.progress = 100;
        task.entry = entry;
        force_play = task.force_play;

        auto& state = StateLocked(instance_id);
        if (force_play || (state.current_video_url.empty() && !state.is_playing)) {
            state.current_video_url = entry.url;
            state.metadata = entry;
            state.is_playing = true;
            state.current_time = 0.0;
            state.start_at = std::chrono::steady_clock::now();
            autostart = true;
        } else {
            state.queue.push_back(entry);
        }
    }

    BroadcastState(instance_id);
    BroadcastText(instance_id, "{\"type\":\"DOWNLOAD_COMPLETE\",\"task_id\":\"" +
        JsonEscape(task_id) + "\",\"entry\":" + EntryJson(entry) +
        ",\"autostart\":" + (autostart ? "true" : "false") + "}");
}

void ActivitySession::AdvanceQueue(const std::string& instance_id) {
    {
        std::lock_guard lock(g_hub.mu_);
        auto& state = g_hub.StateLocked(instance_id);
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
    g_hub.BroadcastState(instance_id);
}

void ActivitySession::RemoveQueue(std::string_view body) {
    const auto id = JsonString(body, "id").value_or("");
    const auto instance_id = JsonString(body, "instance_id").value_or("default");
    if (id.empty()) return;
    {
        std::lock_guard lock(g_hub.mu_);
        auto& queue = g_hub.StateLocked(instance_id).queue;
        queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const QueueEntry& item) {
            return item.id == id;
        }), queue.end());
    }
    g_hub.BroadcastState(instance_id);
}

void ActivitySession::ServeHls(const std::string& path) {
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(
        EnvString("ACTIVITY_DOWNLOAD_DIR", "/var/lib/iouring-runtime/activity-server/downloads")) / "hls");
    const auto rel = UrlDecode(std::string_view(path).substr(std::string_view("/hls/").size()));
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
        SendHttp(200, ContentTypeForUrl(target), "");
        return;
    }

    const auto upstream = CurlGet(target, "--max-time 10");
    if (upstream.exit_code != 0) {
        SendHttp(502, "application/json", "{\"detail\":\"Thumbnail upstream request failed\"}");
        return;
    }
    SendHttp(200, ContentTypeForUrl(target), upstream.output);
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
        SendHttp(200, ContentTypeForUrl(url), "");
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

    const auto content_type = ContentTypeForUrl(url);
    if (content_type == "application/vnd.apple.mpegurl" ||
        upstream.output.starts_with("#EXTM3U")) {
        SendHttp(200, "application/vnd.apple.mpegurl; charset=utf-8",
                 RewriteHlsManifest(upstream.output, url));
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

} // namespace

int main() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    const auto host = EnvString("ACTIVITY_HOST", "0.0.0.0");
    const auto port = EnvPort("ACTIVITY_PORT", 8000);

    ring::IoRingConfig config;
    config.queue_depth = 2048;
    config.buf_ring.buf_count = 4096;
    config.buf_ring.buf_size = 8192;
    config.submit_batch_size = 1;

    auto ring_result = ring::IoRing::Create(config);
    if (!ring_result) {
        std::fprintf(stderr, "failed to create io_uring\n");
        return 1;
    }
    auto io_ring = std::move(*ring_result);
    ring::IoRing::SetCurrent(io_ring.get());
    core::buffer::BufferPool pool;

    io::SessionFactory factory =
        [](int fd, ring::IoRing& loop, core::buffer::BufferPool& buffer_pool,
           core::ContextId) -> io::SessionRef {
        return std::make_shared<ActivitySession>(fd, loop, buffer_pool);
    };

    auto listener = std::make_shared<io::Listener>(
        *io_ring, pool, core::Address{host, port}, std::move(factory), 0, 0);
    auto listen_result = listener->Start();
    if (!listen_result) {
        std::fprintf(stderr, "failed to listen on %s:%u\n", host.c_str(), port);
        return 1;
    }

    std::printf("activity_server listening on %s:%u\n", host.c_str(), port);
    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        io_ring->Dispatch(std::chrono::milliseconds{10});
        io_ring->ProcessPostedTasks();
    }
    listener->Stop();
    for (int i = 0; i < 8; ++i) {
        io_ring->Dispatch(std::chrono::milliseconds{0});
        io_ring->ProcessPostedTasks();
    }
    return 0;
}
