#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/web/WebServer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using iouring_runtime::web::HttpMethod;
using iouring_runtime::web::HttpMethodToString;
using iouring_runtime::web::HttpStatus;
using iouring_runtime::web::RequestContext;

namespace {

constexpr std::uint64_t kDefaultMaxBodyBytes = 10ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDefaultTtlSeconds = 86400;

struct WebhookEvent {
    std::string id;
    std::string inbox;
    std::string method;
    std::string path;
    std::string query;
    std::string content_type;
    std::string remote_addr;
    std::uint64_t size = 0;
    std::int64_t received_at = 0;
    std::int64_t expires_at = 0;
};

std::int64_t UnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t UnixMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
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

std::string ReadFile(std::string_view path) {
    std::ifstream file{std::string(path), std::ios::binary};
    if (!file) {
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

bool WriteFile(const std::filesystem::path& path, std::string_view body) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(out);
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::optional<std::string> UrlDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
            continue;
        }
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        if (i + 2 >= text.size()) {
            return std::nullopt;
        }
        const int hi = HexValue(text[i + 1]);
        const int lo = HexValue(text[i + 2]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
    }
    return out;
}

std::optional<std::string> ExtractJsonString(std::string_view json,
                                             std::string_view key) {
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

std::optional<std::uint64_t> ExtractJsonUint(std::string_view json,
                                             std::string_view key) {
    const auto needle = "\"" + std::string(key) + "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }

    std::uint64_t value = 0;
    bool any = false;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + static_cast<unsigned>(json[pos] - '0');
        any = true;
        ++pos;
    }
    return any ? std::optional<std::uint64_t>(value) : std::nullopt;
}

std::string SafeName(std::string_view raw, std::string_view fallback) {
    std::string out;
    out.reserve(std::min<std::size_t>(raw.size(), 80));
    for (const unsigned char ch : raw) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            out += static_cast<char>(std::tolower(ch));
        } else if (ch == '.' || ch == ' ') {
            out += '-';
        }
        if (out.size() >= 80) {
            break;
        }
    }
    while (!out.empty() && out.front() == '-') out.erase(out.begin());
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string(fallback) : out;
}

bool SafeId(std::string_view id) {
    if (id.size() < 8 || id.size() > 64) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_';
    });
}

std::string GenerateId() {
    static std::atomic<std::uint64_t> counter{0};
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const auto now = static_cast<std::uint64_t>(UnixMilliseconds());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    const auto random = rng();
    std::ostringstream out;
    out << std::hex << now << seq << random;
    return out.str().substr(0, 32);
}

std::string HeadersJson(const RequestContext& ctx) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& header : ctx.request.headers()) {
        out << (first ? "" : ",") << "{\"name\":\"" << JsonEscape(header.name)
            << "\",\"value\":\"" << JsonEscape(header.value) << "\"}";
        first = false;
    }
    out << "]";
    return out.str();
}

std::string EventJson(const WebhookEvent& event,
                      std::string_view headers_json = "[]") {
    std::ostringstream out;
    out << "{\"id\":\"" << JsonEscape(event.id)
        << "\",\"inbox\":\"" << JsonEscape(event.inbox)
        << "\",\"method\":\"" << JsonEscape(event.method)
        << "\",\"path\":\"" << JsonEscape(event.path)
        << "\",\"query\":\"" << JsonEscape(event.query)
        << "\",\"content_type\":\"" << JsonEscape(event.content_type)
        << "\",\"remote_addr\":\"" << JsonEscape(event.remote_addr)
        << "\",\"size\":" << event.size
        << ",\"received_at\":" << event.received_at
        << ",\"expires_at\":" << event.expires_at
        << ",\"headers\":" << headers_json << "}";
    return out.str();
}

std::optional<WebhookEvent> ParseEvent(std::string_view json) {
    auto id = ExtractJsonString(json, "id");
    auto inbox = ExtractJsonString(json, "inbox");
    auto method = ExtractJsonString(json, "method");
    auto path = ExtractJsonString(json, "path");
    auto query = ExtractJsonString(json, "query");
    auto content_type = ExtractJsonString(json, "content_type");
    auto remote_addr = ExtractJsonString(json, "remote_addr");
    auto size = ExtractJsonUint(json, "size");
    auto received_at = ExtractJsonUint(json, "received_at");
    auto expires_at = ExtractJsonUint(json, "expires_at");
    if (!id || !inbox || !method || !path || !query || !content_type ||
        !remote_addr || !size || !received_at || !expires_at) {
        return std::nullopt;
    }
    return WebhookEvent{.id = std::move(*id),
                        .inbox = std::move(*inbox),
                        .method = std::move(*method),
                        .path = std::move(*path),
                        .query = std::move(*query),
                        .content_type = std::move(*content_type),
                        .remote_addr = std::move(*remote_addr),
                        .size = *size,
                        .received_at = static_cast<std::int64_t>(*received_at),
                        .expires_at = static_cast<std::int64_t>(*expires_at)};
}

std::string MimeType(const std::string& filename) {
    const auto ext = std::filesystem::path(filename).extension().string();
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".txt" || ext == ".log" || ext == ".md") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

class InboxStore {
public:
    explicit InboxStore(std::filesystem::path root)
        : root_(std::move(root)),
          events_(root_ / "events"),
          bodies_(root_ / "bodies") {}

    bool Init() {
        std::error_code ec;
        std::filesystem::create_directories(events_, ec);
        if (ec) return false;
        std::filesystem::create_directories(bodies_, ec);
        return !ec;
    }

    bool Put(const WebhookEvent& event, std::string_view headers_json,
             std::string_view body) {
        std::scoped_lock lock(mu_);
        if (!WriteFile(BodyPath(event.id), body)) {
            return false;
        }
        if (!WriteFile(EventPath(event.id), EventJson(event, headers_json) + "\n")) {
            std::error_code ignored;
            std::filesystem::remove(BodyPath(event.id), ignored);
            return false;
        }
        return true;
    }

    std::optional<WebhookEvent> Read(std::string_view id) const {
        if (!SafeId(id)) return std::nullopt;
        auto parsed = ParseEvent(ReadFile(EventPath(id).string()));
        if (!parsed || parsed->id != id) return std::nullopt;
        return parsed;
    }

    std::string ReadEventJson(std::string_view id) const {
        if (!SafeId(id)) return {};
        return ReadFile(EventPath(id).string());
    }

    std::string ReadBody(std::string_view id) const {
        if (!SafeId(id)) return {};
        return ReadFile(BodyPath(id).string());
    }

    std::vector<WebhookEvent> List(std::string_view inbox) const {
        std::vector<WebhookEvent> out;
        const auto safe_inbox = SafeName(inbox, "");
        std::error_code ec;
        if (!std::filesystem::exists(events_, ec)) {
            return out;
        }
        for (const auto& entry : std::filesystem::directory_iterator(events_, ec)) {
            if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".json") {
                continue;
            }
            auto parsed = ParseEvent(ReadFile(entry.path().string()));
            if (!parsed || parsed->expires_at <= UnixSeconds()) {
                continue;
            }
            if (!safe_inbox.empty() && parsed->inbox != safe_inbox) {
                continue;
            }
            out.push_back(std::move(*parsed));
        }
        std::sort(out.begin(), out.end(), [](const WebhookEvent& a, const WebhookEvent& b) {
            return a.received_at > b.received_at;
        });
        return out;
    }

    bool Remove(std::string_view id) {
        if (!SafeId(id)) return false;
        std::scoped_lock lock(mu_);
        std::error_code ec1;
        std::error_code ec2;
        const auto removed_event = std::filesystem::remove(EventPath(id), ec1);
        const auto removed_body = std::filesystem::remove(BodyPath(id), ec2);
        return (removed_event || removed_body) && !ec1 && !ec2;
    }

    void CleanupExpired() {
        std::scoped_lock lock(mu_);
        std::error_code ec;
        if (!std::filesystem::exists(events_, ec)) {
            return;
        }
        const auto now = UnixSeconds();
        for (const auto& entry : std::filesystem::directory_iterator(events_, ec)) {
            if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".json") {
                continue;
            }
            auto parsed = ParseEvent(ReadFile(entry.path().string()));
            if (!parsed || parsed->expires_at > now) {
                continue;
            }
            std::error_code ignored;
            std::filesystem::remove(BodyPath(parsed->id), ignored);
            std::filesystem::remove(entry.path(), ignored);
        }
    }

private:
    std::filesystem::path EventPath(std::string_view id) const {
        auto path = events_ / std::string(id);
        path += ".json";
        return path;
    }

    std::filesystem::path BodyPath(std::string_view id) const {
        auto path = bodies_ / std::string(id);
        path += ".body";
        return path;
    }

    std::filesystem::path root_;
    std::filesystem::path events_;
    std::filesystem::path bodies_;
    mutable std::mutex mu_;
};

template <typename T>
T ReadUnsignedEnv(const char* name, T fallback) {
    if (const char* raw = std::getenv(name)) {
        const auto value = std::stoull(raw);
        if (value > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
            return fallback;
        }
        return static_cast<T>(value);
    }
    return fallback;
}

std::string ReadStringEnv(const char* name, std::string fallback) {
    if (const char* raw = std::getenv(name)) {
        return raw;
    }
    return fallback;
}

std::chrono::milliseconds ReadMillisecondsEnv(
    const char* name, std::chrono::milliseconds fallback) {
    if (const char* raw = std::getenv(name)) {
        return std::chrono::milliseconds(std::stoll(raw));
    }
    return fallback;
}

using WorkerAffinityMode = iouring_runtime::web::WebServerConfig::WorkerAffinityMode;

WorkerAffinityMode ReadWorkerAffinityEnv(const char* name,
                                         WorkerAffinityMode fallback) {
    if (const char* raw = std::getenv(name)) {
        const std::string value = raw;
        if (value == "off") return WorkerAffinityMode::kOff;
        if (value == "physical") return WorkerAffinityMode::kPhysicalCores;
        if (value == "logical") return WorkerAffinityMode::kLogicalCpus;
    }
    return fallback;
}

void ConfigureLoggingFromEnv() {
    iouring_runtime::observability::ConfigureLoggingFromEnv("WEBHOOK_INBOX_LOG_LEVEL");
}

bool Authorized(const RequestContext& ctx, std::string_view token) {
    if (token.empty()) {
        return true;
    }
    const auto header = ctx.request.GetHeader("Authorization");
    const std::string expected = "Bearer " + std::string(token);
    return header == expected;
}

void SendText(RequestContext& ctx, HttpStatus status, std::string body) {
    ctx.response.Status(status)
        .ContentType("text/plain; charset=utf-8")
        .Body(std::move(body))
        .Send();
}

std::filesystem::path DefaultStaticRoot() {
    const std::filesystem::path packaged = "/usr/share/webhook-inbox/static";
    std::error_code ec;
    if (std::filesystem::is_regular_file(packaged / "index.html", ec)) {
        return packaged;
    }
    return std::filesystem::path("examples/web/webhook_inbox/static");
}

std::optional<std::filesystem::path> SafeStaticPath(std::string_view raw_path) {
    auto decoded = UrlDecode(raw_path);
    if (!decoded) return std::nullopt;
    std::replace(decoded->begin(), decoded->end(), '\\', '/');
    while (!decoded->empty() && decoded->front() == '/') {
        decoded->erase(decoded->begin());
    }
    if (decoded->empty()) {
        *decoded = "index.html";
    }

    std::filesystem::path relative;
    for (const auto& part : std::filesystem::path(*decoded)) {
        const auto value = part.string();
        if (value.empty() || value == ".") continue;
        if (value == "..") return std::nullopt;
        relative /= part;
    }
    return relative.empty() ? std::filesystem::path("index.html") : relative;
}

void ServeStaticFile(RequestContext& ctx,
                     const std::filesystem::path& root,
                     std::string_view relative_path) {
    const auto safe_path = SafeStaticPath(relative_path);
    if (!safe_path) {
        SendText(ctx, HttpStatus::kNotFound, "Not Found");
        return;
    }
    const auto path = root / *safe_path;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        SendText(ctx, HttpStatus::kNotFound, "Not Found");
        return;
    }
    auto body = ReadFile(path.string());
    ctx.response.ContentType(MimeType(path.filename().string()))
        .Header("Cache-Control", "no-cache")
        .Body(std::move(body))
        .Send();
}

void CaptureWebhook(RequestContext& ctx, InboxStore& store,
                    std::uint64_t ttl_seconds) {
    store.CleanupExpired();
    const auto inbox = SafeName(ctx.request.Param("inbox"), "default");
    const auto id = GenerateId();
    const auto now = UnixSeconds();
    const auto path_tail = std::string(ctx.request.Param("path"));

    WebhookEvent event{
        .id = id,
        .inbox = inbox,
        .method = std::string(HttpMethodToString(ctx.request.method)),
        .path = path_tail.empty() ? "/" : "/" + path_tail,
        .query = ctx.request.query,
        .content_type = std::string(ctx.request.ContentType()),
        .remote_addr = std::string(ctx.remote_addr),
        .size = ctx.request.body.size(),
        .received_at = now,
        .expires_at = now + static_cast<std::int64_t>(ttl_seconds),
    };

    if (!store.Put(event, HeadersJson(ctx), ctx.request.body)) {
        SendText(ctx, HttpStatus::kInternalServerError, "store failed");
        return;
    }

    ctx.response.Status(HttpStatus::kCreated)
        .Json("{\"id\":\"" + JsonEscape(id) + "\",\"inbox\":\"" + JsonEscape(inbox) +
              "\",\"size\":" + std::to_string(event.size) + "}")
        .Send();
}

void ListEvents(RequestContext& ctx, InboxStore& store, std::string_view token) {
    if (!Authorized(ctx, token)) {
        SendText(ctx, HttpStatus::kUnauthorized, "Unauthorized");
        return;
    }
    store.CleanupExpired();
    const auto inbox = ctx.request.QueryParam("inbox");
    const auto events = store.List(inbox);
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const auto& event : events) {
        json << (first ? "" : ",") << EventJson(event);
        first = false;
    }
    json << "]";
    ctx.response.Json(json.str()).Send();
}

void GetEvent(RequestContext& ctx, InboxStore& store, std::string_view token) {
    if (!Authorized(ctx, token)) {
        SendText(ctx, HttpStatus::kUnauthorized, "Unauthorized");
        return;
    }
    store.CleanupExpired();
    const auto id = std::string(ctx.request.Param("id"));
    auto event_json = store.ReadEventJson(id);
    auto event = ParseEvent(event_json);
    if (!event || event->expires_at <= UnixSeconds()) {
        SendText(ctx, HttpStatus::kNotFound, "Not Found");
        return;
    }
    while (!event_json.empty() &&
           (event_json.back() == '\n' || event_json.back() == '\r')) {
        event_json.pop_back();
    }
    ctx.response.Json("{\"event\":" + event_json + ",\"body\":\"" +
                      JsonEscape(store.ReadBody(id)) + "\"}")
        .Send();
}

void DeleteEvent(RequestContext& ctx, InboxStore& store, std::string_view token) {
    if (!Authorized(ctx, token)) {
        SendText(ctx, HttpStatus::kUnauthorized, "Unauthorized");
        return;
    }
    store.CleanupExpired();
    if (!store.Remove(ctx.request.Param("id"))) {
        SendText(ctx, HttpStatus::kNotFound, "Not Found");
        return;
    }
    ctx.response.Status(HttpStatus::kNoContent).Send();
}

void RegisterCaptureRoutes(iouring_runtime::web::WebServer& server,
                           InboxStore& store,
                           std::uint64_t ttl_seconds) {
    auto capture = [&store, ttl_seconds](RequestContext& ctx) {
        CaptureWebhook(ctx, store, ttl_seconds);
    };
    server.Post("/hook/:inbox", capture);
    server.Post("/hook/:inbox/*path", capture);
    server.Put("/hook/:inbox", capture);
    server.Put("/hook/:inbox/*path", capture);
    server.Route(HttpMethod::kPatch, "/hook/:inbox", capture);
    server.Route(HttpMethod::kPatch, "/hook/:inbox/*path", capture);
}

} // namespace

int main() {
    ConfigureLoggingFromEnv();

    const auto auth_token = ReadStringEnv("WEBHOOK_INBOX_AUTH_TOKEN", "");
    const auto ttl_seconds =
        ReadUnsignedEnv<std::uint64_t>("WEBHOOK_INBOX_DEFAULT_TTL_SECONDS",
                                       kDefaultTtlSeconds);
    const auto max_body_bytes =
        ReadUnsignedEnv<std::uint64_t>("WEBHOOK_INBOX_MAX_BODY_BYTES",
                                       kDefaultMaxBodyBytes);
    const auto static_root =
        std::filesystem::path(ReadStringEnv("WEBHOOK_INBOX_STATIC_ROOT",
                                            DefaultStaticRoot().string()));

    InboxStore store(std::filesystem::path(ReadStringEnv("WEBHOOK_INBOX_ROOT", "/data")));
    if (!store.Init()) {
        return 1;
    }
    store.CleanupExpired();

    iouring_runtime::web::WebServerConfig config;
    config.host = ReadStringEnv("WEBHOOK_INBOX_HOST", "0.0.0.0");
    config.port = ReadUnsignedEnv<std::uint16_t>("WEBHOOK_INBOX_PORT", 3000);
    config.worker_count = ReadUnsignedEnv<std::uint16_t>("WEBHOOK_INBOX_WORKERS", 1);
    config.worker_affinity =
        ReadWorkerAffinityEnv("WEBHOOK_INBOX_WORKER_AFFINITY",
                              config.worker_affinity);
    config.parser.max_body_bytes = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(max_body_bytes,
                                std::numeric_limits<std::uint32_t>::max()));
    config.timeouts.inactivity =
        ReadMillisecondsEnv("WEBHOOK_INBOX_INACTIVITY_TIMEOUT_MS",
                            std::chrono::milliseconds{60000});
    config.timeouts.request =
        ReadMillisecondsEnv("WEBHOOK_INBOX_REQUEST_TIMEOUT_MS",
                            std::chrono::milliseconds{120000});

    std::atomic<bool> cleanup_running{true};
    std::thread cleanup_thread([&] {
        while (cleanup_running.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 60 && cleanup_running.load(std::memory_order_relaxed); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (cleanup_running.load(std::memory_order_relaxed)) {
                store.CleanupExpired();
            }
        }
    });

    iouring_runtime::web::WebServer server(config);
    iouring_runtime::web::WebServer::InstallStopSignalHandlers();

    server.Get("/", [&](RequestContext& ctx) {
        ServeStaticFile(ctx, static_root, "index.html");
    });
    server.Get("/static/*path", [&](RequestContext& ctx) {
        ServeStaticFile(ctx, static_root, ctx.request.Param("path"));
    });
    server.Get("/healthz", [](RequestContext& ctx) {
        ctx.response.ContentType("text/plain; charset=utf-8").Body("ok").Send();
    });
    server.Get("/api/events", [&](RequestContext& ctx) {
        ListEvents(ctx, store, auth_token);
    });
    server.Get("/api/events/:id", [&](RequestContext& ctx) {
        GetEvent(ctx, store, auth_token);
    });
    server.Delete("/api/events/:id", [&](RequestContext& ctx) {
        DeleteEvent(ctx, store, auth_token);
    });
    RegisterCaptureRoutes(server, store, ttl_seconds);

    server.Start();
    iouring_runtime::web::WebServer::WaitForStopSignal(std::chrono::seconds(1));
    server.Stop();
    cleanup_running.store(false, std::memory_order_relaxed);
    if (cleanup_thread.joinable()) {
        cleanup_thread.join();
    }
    return 0;
}
