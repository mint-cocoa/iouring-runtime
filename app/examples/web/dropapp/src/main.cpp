#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/RingEvent.h>
#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/web/WebServer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using iouring_runtime::web::DeferredResponse;
using iouring_runtime::web::HttpMethod;
using iouring_runtime::web::HttpStatus;
using iouring_runtime::web::HttpStreamHandler;
using iouring_runtime::web::RequestContext;

namespace {

constexpr std::uint64_t kDefaultMaxUploadBytes = 100ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDefaultTtlSeconds = 3600;
constexpr std::uint32_t kDefaultSendChunkBytes = 256U * 1024U;

struct DropFile {
    std::string id;
    std::string filename;
    std::uint64_t size = 0;
    std::int64_t created_at = 0;
    std::int64_t expires_at = 0;
};

struct UploadState {
    struct PendingWrite : iouring_runtime::core::ring::WriteEvent {
        std::vector<std::byte> buffer;
        std::size_t written = 0;
        std::uint64_t offset = 0;
    };

    int fd = -1;
    std::filesystem::path temp_path;
    std::filesystem::path final_path;
    DropFile meta;
    std::uint64_t expected_bytes = 0;
    std::uint64_t submitted_bytes = 0;
    std::uint64_t written_bytes = 0;
    bool failed = false;
    bool aborted = false;
    bool request_complete = false;
    bool finished = false;
    std::string error;
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

std::string UrlEncode(std::string_view text) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char ch : text) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
            ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::optional<std::string> UrlDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
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

std::string ReadFile(std::string_view path) {
    std::ifstream file{std::string(path), std::ios::binary};
    if (!file) {
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::optional<std::string> ExtractJsonString(std::string_view json,
                                             std::string_view key) {
    const auto needle = "\"" + std::string(key) + "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;

    std::string out;
    while (pos < json.size()) {
        const char ch = json[pos++];
        if (ch == '"') {
            return out;
        }
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
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() &&
           std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    std::uint64_t value = 0;
    bool any = false;
    while (pos < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + static_cast<unsigned>(json[pos] - '0');
        any = true;
        ++pos;
    }
    if (!any) {
        return std::nullopt;
    }
    return value;
}

std::string SerializeMeta(const DropFile& file) {
    std::ostringstream out;
    out << "{\"id\":\"" << JsonEscape(file.id)
        << "\",\"filename\":\"" << JsonEscape(file.filename)
        << "\",\"size\":" << file.size
        << ",\"created_at\":" << file.created_at
        << ",\"expires_at\":" << file.expires_at << "}\n";
    return out.str();
}

std::optional<DropFile> ParseMeta(std::string_view json) {
    auto id = ExtractJsonString(json, "id");
    auto filename = ExtractJsonString(json, "filename");
    auto size = ExtractJsonUint(json, "size");
    auto created_at = ExtractJsonUint(json, "created_at");
    auto expires_at = ExtractJsonUint(json, "expires_at");
    if (!id || !filename || !size || !created_at || !expires_at) {
        return std::nullopt;
    }
    return DropFile{
        .id = std::move(*id),
        .filename = std::move(*filename),
        .size = *size,
        .created_at = static_cast<std::int64_t>(*created_at),
        .expires_at = static_cast<std::int64_t>(*expires_at),
    };
}

std::string SanitizeFilename(std::string_view raw) {
    std::string value(raw);
    std::replace(value.begin(), value.end(), '\\', '/');
    const auto slash = value.find_last_of('/');
    if (slash != std::string::npos) {
        value = value.substr(slash + 1);
    }

    std::string out;
    out.reserve(std::min<std::size_t>(value.size(), 160));
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == ' ' ||
            ch >= 0x80) {
            out += static_cast<char>(ch);
        } else {
            out += '_';
        }
        if (out.size() >= 160) {
            break;
        }
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.empty() || out == "." || out == "..") {
        return "file";
    }
    return out;
}

std::string AsciiFilenameFallback(std::string_view raw) {
    const auto sanitized = SanitizeFilename(raw);
    std::string out;
    out.reserve(sanitized.size());
    for (const unsigned char ch : sanitized) {
        if (std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == ' ') {
            out += static_cast<char>(ch);
        } else {
            out += '_';
        }
    }
    if (out.empty() || out == "." || out == "..") {
        return "file";
    }
    return out;
}

std::string FilenameFromHeaders(const RequestContext& ctx) {
    const auto encoded = ctx.request.GetHeader("X-Dropapp-Filename-Encoded");
    if (!encoded.empty()) {
        if (auto decoded = UrlDecode(encoded)) {
            return SanitizeFilename(*decoded);
        }
        return "file";
    }
    return SanitizeFilename(ctx.request.GetHeader("X-Dropapp-Filename"));
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

std::string FileUrl(const DropFile& file) {
    return "/d/" + UrlEncode(file.id) + "/" + UrlEncode(file.filename);
}

std::string MetaJsonForApi(const DropFile& file) {
    return "{\"id\":\"" + JsonEscape(file.id) + "\",\"filename\":\"" +
           JsonEscape(file.filename) + "\",\"size\":" + std::to_string(file.size) +
           ",\"created_at\":" + std::to_string(file.created_at) +
           ",\"expires_at\":" + std::to_string(file.expires_at) +
           ",\"url\":\"" + JsonEscape(FileUrl(file)) + "\"}";
}

std::string MimeType(const std::string& filename) {
    const auto ext = std::filesystem::path(filename).extension().string();
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".txt" || ext == ".log" || ext == ".md") return "text/plain; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".pdf") return "application/pdf";
    if (ext == ".zip") return "application/zip";
    return "application/octet-stream";
}

class DropStore {
public:
    explicit DropStore(std::filesystem::path root)
        : root_(std::move(root)),
          objects_(root_ / "objects"),
          meta_(root_ / "meta") {}

    bool Init() {
        std::error_code ec;
        std::filesystem::create_directories(objects_, ec);
        if (ec) {
            return false;
        }
        std::filesystem::create_directories(meta_, ec);
        return !ec;
    }

    std::filesystem::path TempObjectPath(std::string_view id) const {
        auto path = objects_ / std::string(id);
        path += ".part";
        return path;
    }

    std::filesystem::path ObjectPath(std::string_view id) const {
        return objects_ / std::string(id);
    }

    std::filesystem::path MetaPath(std::string_view id) const {
        auto path = meta_ / std::string(id);
        path += ".json";
        return path;
    }

    bool WriteMeta(const DropFile& file) {
        std::scoped_lock lock(mu_);
        std::ofstream out(MetaPath(file.id), std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << SerializeMeta(file);
        return static_cast<bool>(out);
    }

    std::optional<DropFile> ReadMeta(std::string_view id) const {
        if (!SafeId(id)) {
            return std::nullopt;
        }
        const auto parsed = ParseMeta(ReadFile(MetaPath(id).string()));
        if (!parsed || parsed->id != id) {
            return std::nullopt;
        }
        return parsed;
    }

    std::vector<DropFile> List() const {
        std::vector<DropFile> out;
        std::error_code ec;
        if (!std::filesystem::exists(meta_, ec)) {
            return out;
        }
        for (const auto& entry : std::filesystem::directory_iterator(meta_, ec)) {
            if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".json") {
                continue;
            }
            auto parsed = ParseMeta(ReadFile(entry.path().string()));
            if (parsed && parsed->expires_at > UnixSeconds() &&
                std::filesystem::is_regular_file(ObjectPath(parsed->id), ec) && !ec) {
                out.push_back(std::move(*parsed));
            }
        }
        std::sort(out.begin(), out.end(), [](const DropFile& a, const DropFile& b) {
            return a.created_at > b.created_at;
        });
        return out;
    }

    bool Remove(std::string_view id) {
        if (!SafeId(id)) {
            return false;
        }
        std::scoped_lock lock(mu_);
        std::error_code ec1;
        std::error_code ec2;
        const auto removed_object = std::filesystem::remove(ObjectPath(id), ec1);
        const auto removed_meta = std::filesystem::remove(MetaPath(id), ec2);
        return (removed_object || removed_meta) && !ec1 && !ec2;
    }

    void CleanupExpired() {
        std::scoped_lock lock(mu_);
        std::error_code ec;
        if (!std::filesystem::exists(meta_, ec)) {
            return;
        }
        const auto now = UnixSeconds();
        for (const auto& entry : std::filesystem::directory_iterator(meta_, ec)) {
            if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".json") {
                continue;
            }
            auto parsed = ParseMeta(ReadFile(entry.path().string()));
            if (!parsed || parsed->expires_at > now) {
                continue;
            }
            std::error_code ignored;
            std::filesystem::remove(ObjectPath(parsed->id), ignored);
            std::filesystem::remove(entry.path(), ignored);
        }
    }

private:
    std::filesystem::path root_;
    std::filesystem::path objects_;
    std::filesystem::path meta_;
    mutable std::mutex mu_;
};

class AsyncUploadState
    : public std::enable_shared_from_this<AsyncUploadState> {
public:
    using PendingWrite = UploadState::PendingWrite;

    ~AsyncUploadState() {
        Close();
    }

    void Start(UploadState upload, std::function<bool(const DropFile&)> commit,
               std::function<void()> cleanup) {
        ring_ = iouring_runtime::core::ring::IoRing::Current();
        upload_ = std::move(upload);
        commit_ = std::move(commit);
        cleanup_ = std::move(cleanup);
    }

    bool SubmitChunk(std::span<const std::byte> chunk) {
        if (upload_.failed || upload_.finished) {
            return true;
        }
        auto op = std::make_unique<PendingWrite>();
        op->buffer.assign(chunk.begin(), chunk.end());
        op->offset = upload_.submitted_bytes;
        upload_.submitted_bytes += chunk.size();
        auto* event = op.release();
        auto self = shared_from_this();
        iouring_runtime::core::ring::BindCompletion(
            *event, self, &AsyncUploadState::OnWrite);
        event->SetAutoDelete(true);
        pending_.emplace(event, event);
        if (!SubmitPendingWrite(*event)) {
            pending_.erase(event);
            delete event;
            TryFinish();
        }
        return true;
    }

    void MarkRequestComplete(DeferredResponse deferred) {
        deferred_ = std::move(deferred);
        upload_.request_complete = true;
        TryFinish();
    }

    void Abort() {
        if (upload_.finished || upload_.aborted) {
            return;
        }
        upload_.aborted = true;
        Close();
        std::filesystem::remove(upload_.temp_path);
        if (deferred_) {
            deferred_->Abort();
        }
        if (pending_.empty()) {
            upload_.finished = true;
            Cleanup();
        }
    }

protected:
    iouring_runtime::core::ring::DispatchResult OnWrite(
        iouring_runtime::core::ring::WriteEvent& ev,
        std::int32_t result) {
        auto it = pending_.find(&ev);
        if (it == pending_.end()) {
            return iouring_runtime::core::ring::DispatchResult::kComplete;
        }
        if (upload_.aborted) {
            pending_.erase(it);
            TryFinish();
            return iouring_runtime::core::ring::DispatchResult::kComplete;
        }
        auto& op = *it->second;
        if (result <= 0) {
            upload_.failed = true;
            upload_.error = "async write failed";
            pending_.erase(it);
            TryFinish();
            return iouring_runtime::core::ring::DispatchResult::kComplete;
        }
        op.written += static_cast<std::size_t>(result);
        if (op.written < op.buffer.size()) {
            if (!SubmitPendingWrite(op)) {
                pending_.erase(it);
                TryFinish();
                return iouring_runtime::core::ring::DispatchResult::kComplete;
            }
            return iouring_runtime::core::ring::DispatchResult::kPending;
        }
        upload_.written_bytes += op.buffer.size();
        pending_.erase(it);
        TryFinish();
        return iouring_runtime::core::ring::DispatchResult::kComplete;
    }

private:
    bool SubmitPendingWrite(PendingWrite& op) {
        if (!ring_ || upload_.fd < 0) {
            upload_.failed = true;
            upload_.error = "io_uring unavailable";
            return false;
        }
        const auto remaining = op.buffer.size() - op.written;
        const auto* data = op.buffer.data() + op.written;
        if (!ring_->PrepWrite(op, upload_.fd, data,
                              static_cast<unsigned>(remaining),
                              op.offset + op.written)) {
            upload_.failed = true;
            upload_.error = "submit write failed";
            return false;
        }
        ring_->Submit();
        return true;
    }

    void TryFinish() {
        if (upload_.finished) {
            return;
        }
        if (upload_.aborted) {
            if (pending_.empty()) {
                upload_.finished = true;
                Cleanup();
            }
            return;
        }
        if (!upload_.request_complete || !deferred_ || !pending_.empty()) {
            return;
        }
        upload_.finished = true;
        Close();

        if (upload_.failed ||
            (upload_.expected_bytes != 0 &&
             upload_.submitted_bytes != upload_.expected_bytes) ||
            upload_.written_bytes != upload_.submitted_bytes) {
            std::filesystem::remove(upload_.temp_path);
            SendText(HttpStatus::kInternalServerError,
                     upload_.failed ? upload_.error : "short upload");
            FinishDeferredResponse();
            return;
        }

        std::error_code ec;
        std::filesystem::rename(upload_.temp_path, upload_.final_path, ec);
        if (ec) {
            std::filesystem::remove(upload_.temp_path);
            SendText(HttpStatus::kInternalServerError, "rename failed");
            FinishDeferredResponse();
            return;
        }

        upload_.meta.size = upload_.written_bytes;
        if (!commit_ || !commit_(upload_.meta)) {
            std::filesystem::remove(upload_.final_path);
            SendText(HttpStatus::kInternalServerError, "metadata write failed");
            FinishDeferredResponse();
            return;
        }

        deferred_->Response()
            .Status(HttpStatus::kCreated)
            .KeepAlive(false)
            .Json(MetaJsonForApi(upload_.meta))
            .Send();
        FinishDeferredResponse();
    }

    void SendText(HttpStatus status, std::string body) {
        if (!deferred_) {
            return;
        }
        deferred_->Response()
            .Status(status)
            .KeepAlive(false)
            .ContentType("text/plain")
            .Body(std::move(body))
            .Send();
    }

    void FinishDeferredResponse() {
        Cleanup();
        if (deferred_) {
            deferred_->Complete();
            deferred_.reset();
        }
    }

    void Cleanup() {
        if (cleanup_) {
            auto cleanup = std::move(cleanup_);
            cleanup();
        }
    }

    void Close() {
        if (upload_.fd >= 0) {
            close(upload_.fd);
            upload_.fd = -1;
        }
    }

    UploadState upload_;
    iouring_runtime::core::ring::IoRing* ring_ = nullptr;
    std::optional<DeferredResponse> deferred_;
    std::unordered_map<iouring_runtime::core::ring::WriteEvent*,
                       PendingWrite*> pending_;
    std::function<bool(const DropFile&)> commit_;
    std::function<void()> cleanup_;
};

class UploadRegistry {
public:
    void Put(std::string request_id, std::shared_ptr<AsyncUploadState> state) {
        std::scoped_lock lock(mu_);
        uploads_[std::move(request_id)] = std::move(state);
    }

    std::shared_ptr<AsyncUploadState> Get(std::string_view request_id) {
        std::scoped_lock lock(mu_);
        auto it = uploads_.find(std::string(request_id));
        return it == uploads_.end() ? nullptr : it->second;
    }

    std::shared_ptr<AsyncUploadState> Take(std::string_view request_id) {
        std::scoped_lock lock(mu_);
        auto it = uploads_.find(std::string(request_id));
        if (it == uploads_.end()) {
            return nullptr;
        }
        auto state = std::move(it->second);
        uploads_.erase(it);
        return state;
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<AsyncUploadState>> uploads_;
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
    iouring_runtime::observability::ConfigureLoggingFromEnv("DROPAPP_LOG_LEVEL");
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
    const std::filesystem::path packaged = "/usr/share/dropapp/static";
    std::error_code ec;
    if (std::filesystem::is_regular_file(packaged / "index.html", ec)) {
        return packaged;
    }
    return std::filesystem::path("app/examples/web/dropapp/static");
}

std::optional<std::filesystem::path> SafeStaticPath(std::string_view raw_path) {
    auto decoded = UrlDecode(raw_path);
    if (!decoded) {
        return std::nullopt;
    }
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
        if (value.empty() || value == ".") {
            continue;
        }
        if (value == "..") {
            return std::nullopt;
        }
        relative /= part;
    }
    if (relative.empty()) {
        relative = "index.html";
    }
    return relative;
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

void ListFiles(RequestContext& ctx, DropStore& store) {
    store.CleanupExpired();
    const auto files = store.List();
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const auto& file : files) {
        json << (first ? "" : ",") << MetaJsonForApi(file);
        first = false;
    }
    json << "]";
    ctx.response.Json(json.str()).Send();
}

void SendFile(RequestContext& ctx, DropStore& store, std::uint32_t chunk_bytes) {
    store.CleanupExpired();
    const auto id = std::string(ctx.request.Param("id"));
    auto meta = store.ReadMeta(id);
    if (!meta || meta->expires_at <= UnixSeconds()) {
        SendText(ctx, HttpStatus::kNotFound, "Not Found");
        return;
    }

    const auto path = store.ObjectPath(id);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        SendText(ctx, HttpStatus::kNotFound, "Not Found");
        return;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        SendText(ctx, HttpStatus::kInternalServerError, "Internal Server Error");
        return;
    }

    ctx.response.ContentType(MimeType(meta->filename))
        .Header("Content-Length", std::to_string(size))
        .Header("Content-Disposition",
                "attachment; filename=\"" + JsonEscape(AsciiFilenameFallback(meta->filename)) +
                    "\"; filename*=UTF-8''" + UrlEncode(meta->filename))
        .Send();
    if (ctx.request.method == HttpMethod::kHead) {
        return;
    }

    std::ifstream file(path, std::ios::binary);
    std::string chunk;
    chunk.resize(std::max<std::uint32_t>(4096, chunk_bytes));
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto got = file.gcount();
        if (got <= 0) {
            break;
        }
        auto allocated = ctx.pool.Allocate(static_cast<std::uint32_t>(got));
        if (!allocated) {
            break;
        }
        auto buffer = std::move(*allocated);
        std::memcpy(buffer->Writable().data(), chunk.data(), static_cast<std::size_t>(got));
        buffer->Commit(static_cast<std::uint32_t>(got));
        ctx.session.SendResponse(std::move(buffer));
    }
}

void DeleteFile(RequestContext& ctx, DropStore& store, std::string_view token) {
    store.CleanupExpired();
    if (!Authorized(ctx, token)) {
        SendText(ctx, HttpStatus::kUnauthorized, "Unauthorized");
        return;
    }
    const auto id = std::string(ctx.request.Param("id"));
    if (!store.Remove(id)) {
        SendText(ctx, HttpStatus::kNotFound, "Not Found");
        return;
    }
    ctx.response.Status(HttpStatus::kNoContent).Send();
}

} // namespace

int main() {
    ConfigureLoggingFromEnv();

    const auto auth_token = ReadStringEnv("DROPAPP_AUTH_TOKEN", "");
    const auto ttl_seconds =
        ReadUnsignedEnv<std::uint64_t>("DROPAPP_DEFAULT_TTL_SECONDS",
                                       kDefaultTtlSeconds);
    const auto max_upload_bytes =
        ReadUnsignedEnv<std::uint64_t>("DROPAPP_MAX_UPLOAD_BYTES",
                                       kDefaultMaxUploadBytes);
    const auto send_chunk_bytes =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_SEND_CHUNK_BYTES",
                                       kDefaultSendChunkBytes);
    const auto static_root =
        std::filesystem::path(ReadStringEnv("DROPAPP_STATIC_ROOT",
                                            DefaultStaticRoot().string()));

    DropStore store(std::filesystem::path(ReadStringEnv("DROPAPP_ROOT", "/data")));
    if (!store.Init()) {
        return 1;
    }
    store.CleanupExpired();

    iouring_runtime::web::WebServerConfig config;
    config.host = ReadStringEnv("DROPAPP_HOST", "0.0.0.0");
    config.port = ReadUnsignedEnv<std::uint16_t>("DROPAPP_PORT", 3000);
    config.worker_count = ReadUnsignedEnv<std::uint16_t>("DROPAPP_WORKERS", 1);
    config.worker_affinity =
        ReadWorkerAffinityEnv("DROPAPP_WORKER_AFFINITY", config.worker_affinity);
    config.max_sessions_per_worker =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_MAX_SESSIONS_PER_WORKER", 0);
    config.ring.queue_depth =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_RING_QUEUE_DEPTH",
                                       config.ring.queue_depth);
    config.ring.buf_count =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_RING_BUF_COUNT",
                                       config.ring.buf_count);
    config.ring.buf_size =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_RING_BUF_SIZE",
                                       config.ring.buf_size);
    config.ring.submit_batch_size =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_RING_SUBMIT_BATCH",
                                       config.ring.submit_batch_size);
    config.ring.cqe_batch_budget =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_RING_CQE_BATCH_BUDGET",
                                       config.ring.cqe_batch_budget);
    config.ring.io_timeout =
        ReadMillisecondsEnv("DROPAPP_RING_IO_TIMEOUT_MS", config.ring.io_timeout);
    config.parser.max_body_bytes = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(max_upload_bytes,
                                std::numeric_limits<std::uint32_t>::max()));
    config.timeouts.inactivity =
        ReadMillisecondsEnv("DROPAPP_INACTIVITY_TIMEOUT_MS",
                            std::chrono::milliseconds{60000});
    config.timeouts.request =
        ReadMillisecondsEnv("DROPAPP_REQUEST_TIMEOUT_MS",
                            std::chrono::milliseconds{120000});
    config.backpressure.send_queue_max_pending =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_SEND_QUEUE_MAX_PENDING",
                                       config.backpressure.send_queue_max_pending);
    config.backpressure.send_queue_high_watermark =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_SEND_QUEUE_HIGH_WATERMARK",
                                       config.backpressure.send_queue_high_watermark);
    config.backpressure.send_queue_low_watermark =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_SEND_QUEUE_LOW_WATERMARK",
                                       config.backpressure.send_queue_low_watermark);
    config.backpressure.disconnect_on_high_watermark =
        ReadUnsignedEnv<std::uint32_t>("DROPAPP_DISCONNECT_ON_HIGH_WATERMARK", 0) != 0;

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

    UploadRegistry uploads;
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
    server.Get("/api/files", [&](RequestContext& ctx) {
        ListFiles(ctx, store);
    });
    server.Get("/d/:id/*filename", [&](RequestContext& ctx) {
        SendFile(ctx, store, send_chunk_bytes);
    });
    server.Head("/d/:id/*filename", [&](RequestContext& ctx) {
        SendFile(ctx, store, send_chunk_bytes);
    });
    server.Delete("/api/files/:id", [&](RequestContext& ctx) {
        DeleteFile(ctx, store, auth_token);
    });

    server.PostStream("/api/files", HttpStreamHandler{
        .on_headers = [&](RequestContext& ctx) {
            store.CleanupExpired();
            if (!Authorized(ctx, auth_token)) {
                SendText(ctx, HttpStatus::kUnauthorized, "Unauthorized");
                return false;
            }
            const auto content_length = ctx.request.ContentLength();
            if (content_length > max_upload_bytes) {
                SendText(ctx, HttpStatus::kPayloadTooLarge, "Payload Too Large");
                return false;
            }
            const auto filename = FilenameFromHeaders(ctx);
            const auto id = GenerateId();
            const auto now = UnixSeconds();

            UploadState upload;
            upload.expected_bytes = content_length;
            upload.temp_path = store.TempObjectPath(id);
            upload.final_path = store.ObjectPath(id);
            upload.meta = DropFile{
                .id = id,
                .filename = filename,
                .size = content_length,
                .created_at = now,
                .expires_at = now + static_cast<std::int64_t>(ttl_seconds),
            };
            upload.fd = open(upload.temp_path.c_str(),
                             O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
            if (upload.fd < 0) {
                SendText(ctx, HttpStatus::kInternalServerError, "open failed");
                return false;
            }

            const std::string request_id(ctx.request_id);
            auto state = std::make_shared<AsyncUploadState>();
            state->Start(std::move(upload),
                         [&store](const DropFile& file) {
                             return store.WriteMeta(file);
                         },
                         [&uploads, request_id] {
                             uploads.Take(request_id);
                         });
            uploads.Put(request_id, std::move(state));
            return true;
        },
        .on_body = [&](RequestContext& ctx, std::span<const std::byte> chunk) {
            auto state = uploads.Get(ctx.request_id);
            return state ? state->SubmitChunk(chunk) : true;
        },
        .on_complete = [&](RequestContext& ctx) {
            auto deferred = ctx.DeferResponse();
            auto state = uploads.Get(ctx.request_id);
            if (state) {
                state->MarkRequestComplete(std::move(deferred));
            }
        },
        .on_abort = [&](RequestContext& ctx) {
            auto state = uploads.Get(ctx.request_id);
            if (state) {
                state->Abort();
            }
        },
    });

    server.Start();
    iouring_runtime::web::WebServer::WaitForStopSignal(std::chrono::seconds(1));
    server.Stop();
    cleanup_running.store(false, std::memory_order_relaxed);
    if (cleanup_thread.joinable()) {
        cleanup_thread.join();
    }
    return 0;
}
