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
#include <initializer_list>
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
    std::string service;
    std::string event_type;
    std::string status;
    std::string source;
    std::string message;
    std::string delivery_id;
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

std::string FirstPresentJsonString(std::string_view json,
                                   std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) {
        if (auto value = ExtractJsonString(json, key)) {
            return *value;
        }
    }
    return {};
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
                      std::string_view headers_json = "[]",
                      std::optional<std::string_view> body = std::nullopt) {
    std::ostringstream out;
    out << "{\"id\":\"" << JsonEscape(event.id)
        << "\",\"inbox\":\"" << JsonEscape(event.inbox)
        << "\",\"service\":\"" << JsonEscape(event.service)
        << "\",\"event_type\":\"" << JsonEscape(event.event_type)
        << "\",\"status\":\"" << JsonEscape(event.status)
        << "\",\"source\":\"" << JsonEscape(event.source)
        << "\",\"message\":\"" << JsonEscape(event.message)
        << "\",\"delivery_id\":\"" << JsonEscape(event.delivery_id)
        << "\",\"method\":\"" << JsonEscape(event.method)
        << "\",\"path\":\"" << JsonEscape(event.path)
        << "\",\"query\":\"" << JsonEscape(event.query)
        << "\",\"content_type\":\"" << JsonEscape(event.content_type)
        << "\",\"remote_addr\":\"" << JsonEscape(event.remote_addr)
        << "\",\"size\":" << event.size
        << ",\"received_at\":" << event.received_at
        << ",\"expires_at\":" << event.expires_at
        << ",\"headers\":" << headers_json;
    if (body) {
        out << ",\"body\":\"" << JsonEscape(*body) << "\"";
    }
    out << "}";
    return out.str();
}

std::optional<WebhookEvent> ParseEvent(std::string_view json) {
    auto id = ExtractJsonString(json, "id");
    auto inbox = ExtractJsonString(json, "inbox");
    auto service = ExtractJsonString(json, "service");
    auto event_type = ExtractJsonString(json, "event_type");
    auto status = ExtractJsonString(json, "status");
    auto source = ExtractJsonString(json, "source");
    auto message = ExtractJsonString(json, "message");
    auto delivery_id = ExtractJsonString(json, "delivery_id");
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
                        .service = service.value_or(*inbox),
                        .event_type = event_type.value_or("webhook"),
                        .status = status.value_or("received"),
                        .source = source.value_or(""),
                        .message = message.value_or(""),
                        .delivery_id = delivery_id.value_or(""),
                        .method = std::move(*method),
                        .path = std::move(*path),
                        .query = std::move(*query),
                        .content_type = std::move(*content_type),
                        .remote_addr = std::move(*remote_addr),
                        .size = *size,
                        .received_at = static_cast<std::int64_t>(*received_at),
                        .expires_at = static_cast<std::int64_t>(*expires_at)};
}

std::string Truncate(std::string value, std::size_t max_size) {
    if (value.size() <= max_size) {
        return value;
    }
    value.resize(max_size);
    value += "...";
    return value;
}

std::string HeaderValue(const RequestContext& ctx, std::string_view name) {
    return std::string(ctx.request.GetHeader(name));
}

WebhookEvent ClassifyEvent(const RequestContext& ctx,
                           std::string service,
                           std::string inbox,
                           std::uint64_t ttl_seconds) {
    const auto now = UnixSeconds();
    const auto body = std::string_view(ctx.request.body);
    const auto id = GenerateId();
    const auto path_tail = std::string(ctx.request.Param("path"));

    WebhookEvent event{
        .id = id,
        .inbox = std::move(inbox),
        .service = SafeName(service, "custom"),
        .event_type = "webhook",
        .status = "received",
        .source = "",
        .message = "",
        .delivery_id = "",
        .method = std::string(HttpMethodToString(ctx.request.method)),
        .path = path_tail.empty() ? "/" : "/" + path_tail,
        .query = ctx.request.query,
        .content_type = std::string(ctx.request.ContentType()),
        .remote_addr = std::string(ctx.remote_addr),
        .size = ctx.request.body.size(),
        .received_at = now,
        .expires_at = now + static_cast<std::int64_t>(ttl_seconds),
    };

    if (event.service == "github") {
        event.event_type = HeaderValue(ctx, "X-GitHub-Event");
        if (event.event_type.empty()) event.event_type = "github";
        event.delivery_id = HeaderValue(ctx, "X-GitHub-Delivery");
        event.source = FirstPresentJsonString(body, {"full_name", "name", "repository"});
        const auto action = FirstPresentJsonString(body, {"action"});
        const auto ref = FirstPresentJsonString(body, {"ref", "head_branch"});
        event.status = FirstPresentJsonString(body, {"conclusion", "status", "state"});
        if (event.status.empty()) event.status = "received";
        if (!action.empty() && !ref.empty()) {
            event.message = action + " on " + ref;
        } else if (!action.empty()) {
            event.message = action;
        } else if (!ref.empty()) {
            event.message = ref;
        }
    } else if (event.service == "argocd") {
        event.event_type = FirstPresentJsonString(body, {"eventType", "type", "reason"});
        if (event.event_type.empty()) event.event_type = "argocd";
        event.source = FirstPresentJsonString(body, {"appName", "application", "name"});
        event.status = FirstPresentJsonString(body, {"phase", "sync_status", "health_status", "status"});
        if (event.status.empty()) event.status = "received";
        event.message = FirstPresentJsonString(body, {"message", "summary", "description"});
    } else {
        event.event_type = FirstPresentJsonString(body, {"event", "type", "action"});
        if (event.event_type.empty()) event.event_type = event.service;
        event.source = FirstPresentJsonString(body, {"source", "app", "name"});
        event.status = FirstPresentJsonString(body, {"status", "state", "phase"});
        if (event.status.empty()) event.status = "received";
        event.message = FirstPresentJsonString(body, {"message", "summary", "description"});
    }

    event.source = Truncate(event.source, 120);
    event.message = Truncate(event.message, 240);
    return event;
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
private:
    struct EventRecord {
        WebhookEvent event;
        std::string json;
    };

public:
    explicit InboxStore(std::filesystem::path root)
        : root_(std::move(root)),
          log_path_(root_ / "events.jsonl") {}

    bool Init() {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        if (ec) return false;
        std::ifstream in(log_path_, std::ios::binary);
        if (!in) {
            std::ofstream create(log_path_, std::ios::binary | std::ios::app);
            return static_cast<bool>(create);
        }

        std::string line;
        std::scoped_lock lock(mu_);
        while (std::getline(in, line)) {
            if (auto deleted_id = ExtractJsonString(line, "deleted_id")) {
                by_id_.erase(std::remove_if(by_id_.begin(), by_id_.end(),
                                            [&](const EventRecord& record) {
                                                return record.event.id == *deleted_id;
                                            }),
                             by_id_.end());
                continue;
            }
            auto parsed = ParseEvent(line);
            if (!parsed) {
                continue;
            }
            by_id_.push_back(EventRecord{.event = std::move(*parsed),
                                         .json = std::move(line)});
        }
        return true;
    }

    bool Put(const WebhookEvent& event, std::string_view headers_json,
             std::string_view body) {
        auto line = EventJson(event, headers_json, body);
        std::scoped_lock lock(mu_);
        std::ofstream out(log_path_, std::ios::binary | std::ios::app);
        if (!out) {
            return false;
        }
        out << line << "\n";
        if (!out) {
            return false;
        }
        by_id_.push_back(EventRecord{.event = event, .json = std::move(line)});
        return true;
    }

    std::optional<WebhookEvent> Read(std::string_view id) const {
        if (!SafeId(id)) return std::nullopt;
        std::scoped_lock lock(mu_);
        for (auto it = by_id_.rbegin(); it != by_id_.rend(); ++it) {
            if (it->event.id == id && it->event.expires_at > UnixSeconds()) {
                return it->event;
            }
        }
        return std::nullopt;
    }

    std::string ReadEventJson(std::string_view id) const {
        if (!SafeId(id)) return {};
        std::scoped_lock lock(mu_);
        for (auto it = by_id_.rbegin(); it != by_id_.rend(); ++it) {
            if (it->event.id == id && it->event.expires_at > UnixSeconds()) {
                return it->json;
            }
        }
        return {};
    }

    std::vector<WebhookEvent> List(std::string_view service,
                                   std::string_view query,
                                   std::size_t limit) const {
        std::vector<WebhookEvent> out;
        const auto safe_service = SafeName(service, "");
        const auto safe_query = SafeName(query, "");
        const auto now = UnixSeconds();
        std::scoped_lock lock(mu_);
        for (auto it = by_id_.rbegin(); it != by_id_.rend(); ++it) {
            const auto& event = it->event;
            if (event.expires_at <= now) {
                continue;
            }
            if (!safe_service.empty() && event.service != safe_service) {
                continue;
            }
            if (!safe_query.empty()) {
                const auto haystack = SafeName(event.service + " " + event.event_type + " " +
                                                   event.status + " " + event.source + " " +
                                                   event.message,
                                               "");
                if (haystack.find(safe_query) == std::string::npos) {
                    continue;
                }
            }
            out.push_back(event);
            if (out.size() >= limit) {
                break;
            }
        }
        return out;
    }

    bool Remove(std::string_view id) {
        if (!SafeId(id)) return false;
        std::scoped_lock lock(mu_);
        const auto before = by_id_.size();
        by_id_.erase(std::remove_if(by_id_.begin(), by_id_.end(), [&](const EventRecord& record) {
                         return record.event.id == id;
                     }),
                     by_id_.end());
        if (by_id_.size() == before) {
            return false;
        }
        std::ofstream out(log_path_, std::ios::binary | std::ios::app);
        if (out) {
            out << "{\"deleted_id\":\"" << JsonEscape(id)
                << "\",\"deleted_at\":" << UnixSeconds() << "}\n";
        }
        return true;
    }

    void CleanupExpired() {
        std::scoped_lock lock(mu_);
        const auto now = UnixSeconds();
        by_id_.erase(std::remove_if(by_id_.begin(), by_id_.end(), [&](const EventRecord& record) {
                         return record.event.expires_at <= now;
                     }),
                     by_id_.end());
    }

private:
    std::filesystem::path root_;
    std::filesystem::path log_path_;
    std::vector<EventRecord> by_id_;
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
    return std::filesystem::path("app/examples/web/webhook_inbox/static");
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
                    std::uint64_t ttl_seconds,
                    std::string_view service,
                    std::string_view inbox) {
    store.CleanupExpired();
    const auto safe_service = SafeName(service, "custom");
    const auto safe_inbox = SafeName(inbox, safe_service);
    auto event = ClassifyEvent(ctx, safe_service, safe_inbox, ttl_seconds);

    if (!store.Put(event, HeadersJson(ctx), ctx.request.body)) {
        SendText(ctx, HttpStatus::kInternalServerError, "store failed");
        return;
    }

    ctx.response.Status(HttpStatus::kCreated)
        .Json("{\"id\":\"" + JsonEscape(event.id) +
              "\",\"service\":\"" + JsonEscape(event.service) +
              "\",\"event_type\":\"" + JsonEscape(event.event_type) +
              "\",\"status\":\"" + JsonEscape(event.status) +
              "\",\"source\":\"" + JsonEscape(event.source) +
              "\",\"size\":" + std::to_string(event.size) + "}")
        .Send();
}

void ListEvents(RequestContext& ctx, InboxStore& store, std::string_view token) {
    if (!Authorized(ctx, token)) {
        SendText(ctx, HttpStatus::kUnauthorized, "Unauthorized");
        return;
    }
    store.CleanupExpired();
    const auto service = ctx.request.QueryParam("service");
    const auto query = ctx.request.QueryParam("q");
    const auto limit_value = ctx.request.QueryParam("limit");
    std::size_t limit = 100;
    if (!limit_value.empty()) {
        try {
            limit = std::clamp<std::size_t>(std::stoull(std::string(limit_value)), 1, 500);
        } catch (...) {
            limit = 100;
        }
    }
    const auto events = store.List(service, query, limit);
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
    const auto body = ExtractJsonString(event_json, "body").value_or("");
    ctx.response.Json("{\"event\":" + event_json + ",\"body\":\"" +
                      JsonEscape(body) + "\"}")
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
    auto github = [&store, ttl_seconds](RequestContext& ctx) {
        CaptureWebhook(ctx, store, ttl_seconds, "github", "github");
    };
    auto argocd = [&store, ttl_seconds](RequestContext& ctx) {
        CaptureWebhook(ctx, store, ttl_seconds, "argocd", "argocd");
    };
    auto generic = [&store, ttl_seconds](RequestContext& ctx) {
        CaptureWebhook(ctx, store, ttl_seconds, ctx.request.Param("inbox"),
                       ctx.request.Param("inbox"));
    };
    server.Post("/hooks/github", github);
    server.Post("/hooks/github/*path", github);
    server.Post("/hooks/argocd", argocd);
    server.Post("/hooks/argocd/*path", argocd);
    server.Post("/hook/:inbox", generic);
    server.Post("/hook/:inbox/*path", generic);
    server.Put("/hook/:inbox", generic);
    server.Put("/hook/:inbox/*path", generic);
    server.Route(HttpMethod::kPatch, "/hook/:inbox", generic);
    server.Route(HttpMethod::kPatch, "/hook/:inbox/*path", generic);
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
