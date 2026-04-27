#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/web/WebServer.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

using iouring_runtime::web::HttpMethod;
using iouring_runtime::web::HttpStatus;
using iouring_runtime::web::RequestContext;

namespace {

constexpr std::uint64_t kDefaultDownloadBytes = 30ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kDefaultDownloadChunkBytes = 1024U * 1024U;
constexpr std::uint64_t kDefaultMaxUploadBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDefaultMaxStaticFileBytes = 8ULL * 1024ULL * 1024ULL;

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

WorkerAffinityMode ReadWorkerAffinityEnv(
    const char* name, WorkerAffinityMode fallback) {
    if (const char* raw = std::getenv(name)) {
        const std::string value = raw;
        if (value == "off") {
            return WorkerAffinityMode::kOff;
        }
        if (value == "physical") {
            return WorkerAffinityMode::kPhysicalCores;
        }
        if (value == "logical") {
            return WorkerAffinityMode::kLogicalCpus;
        }
    }
    return fallback;
}

void ConfigureLoggingFromEnv() {
    iouring_runtime::observability::ConfigureLoggingFromEnv(
        "SPEEDTEST_LOG_LEVEL");
}

std::string_view ExtensionOf(std::string_view path) {
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    return path.substr(dot);
}

std::string_view MimeType(std::string_view path) {
    const auto ext = ExtensionOf(path);
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".xml") return "application/xml; charset=utf-8";
    if (ext == ".json" || ext == ".webmanifest") return "application/json";
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf") return "font/ttf";
    if (ext == ".eot") return "application/vnd.ms-fontobject";
    return "application/octet-stream";
}

bool IsSafeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path,
                                    std::uint64_t max_bytes) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > max_bytes) {
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string body;
    body.resize(static_cast<std::size_t>(size));
    file.read(body.data(), static_cast<std::streamsize>(body.size()));
    if (!file && static_cast<std::uint64_t>(file.gcount()) != size) {
        return std::nullopt;
    }
    return body;
}

void AddCorsHeaders(iouring_runtime::web::HttpResponse& response) {
    response.Header("Access-Control-Allow-Origin", "*")
        .Header("Access-Control-Allow-Methods", "GET,HEAD,POST,OPTIONS")
        .Header("Access-Control-Allow-Headers", "Content-Type,Content-Length")
        .Header("Access-Control-Max-Age", "86400");
}

void SendOptions(RequestContext& ctx) {
    AddCorsHeaders(ctx.response);
    ctx.response.Status(HttpStatus::kNoContent).Send();
}

bool QueueFilledBuffer(RequestContext& ctx, std::uint32_t size, std::byte value,
                       iouring_runtime::core::buffer::SendBufferRef& out) {
    auto result = ctx.pool.Allocate(size);
    if (!result) {
        return false;
    }
    out = std::move(*result);
    std::memset(out->Writable().data(), static_cast<int>(value), size);
    out->Commit(size);
    return true;
}

void SendDownload(RequestContext& ctx, std::uint64_t bytes,
                  std::uint32_t chunk_bytes) {
    chunk_bytes = std::clamp<std::uint32_t>(chunk_bytes, 16 * 1024, 4 * 1024 * 1024);
    if (bytes > 0 && chunk_bytes > bytes) {
        chunk_bytes = static_cast<std::uint32_t>(bytes);
    }

    iouring_runtime::core::buffer::SendBufferRef chunk;
    const bool body_suppressed = ctx.request.method == HttpMethod::kHead;
    if (!body_suppressed && bytes > 0 &&
        !QueueFilledBuffer(ctx, chunk_bytes, std::byte{'0'}, chunk)) {
        ctx.response.Status(HttpStatus::kServiceUnavailable)
            .ContentType("text/plain")
            .Body("send buffer unavailable")
            .Send();
        return;
    }

    AddCorsHeaders(ctx.response);
    ctx.response.ContentType("application/octet-stream")
        .Header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        .Header("Pragma", "no-cache")
        .Header("Content-Length", std::to_string(bytes))
        .Send();

    if (body_suppressed) {
        return;
    }

    auto remaining = bytes;
    while (remaining >= chunk_bytes && chunk) {
        ctx.session.SendResponse(chunk);
        remaining -= chunk_bytes;
    }

    if (remaining > 0) {
        iouring_runtime::core::buffer::SendBufferRef tail;
        if (QueueFilledBuffer(ctx, static_cast<std::uint32_t>(remaining),
                              std::byte{'0'}, tail)) {
            ctx.session.SendResponse(std::move(tail));
        }
    }
}

void SendUploadOk(RequestContext& ctx) {
    AddCorsHeaders(ctx.response);
    ctx.response.ContentType("text/plain")
        .Header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        .Header("Content-Length", "0")
        .Send();
}

void SendStaticFile(RequestContext& ctx, const std::filesystem::path& static_root,
                    std::uint64_t max_static_file_bytes) {
    std::filesystem::path relative =
        ctx.request.path == "/" ? std::filesystem::path{"index.html"}
                                : std::filesystem::path{ctx.request.path.substr(1)};
    if (!IsSafeRelativePath(relative)) {
        ctx.response.Status(HttpStatus::kBadRequest)
            .ContentType("text/plain")
            .Body("Bad Request")
            .Send();
        return;
    }

    auto path = static_root / relative;
    auto body = ReadFile(path, max_static_file_bytes);
    if (!body && ctx.request.path != "/" && relative.extension().empty()) {
        path = static_root / relative / "index.html";
        body = ReadFile(path, max_static_file_bytes);
    }
    if (!body) {
        ctx.response.Status(HttpStatus::kNotFound)
            .ContentType("text/plain")
            .Body("Not Found")
            .Send();
        return;
    }

    ctx.response.ContentType(MimeType(path.string()))
        .Header("Cache-Control", path.filename() == "index.html" ? "no-cache"
                                                                  : "public, max-age=3600")
        .Body(std::move(*body))
        .Send();
}

} // namespace

int main() {
    ConfigureLoggingFromEnv();

    const auto static_root = std::filesystem::path(ReadStringEnv(
        "SPEEDTEST_STATIC_ROOT", SPEEDTEST_DEFAULT_STATIC_ROOT));
    const auto download_bytes =
        ReadUnsignedEnv<std::uint64_t>("SPEEDTEST_DOWNLOAD_BYTES",
                                       kDefaultDownloadBytes);
    const auto download_chunk_bytes =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_DOWNLOAD_CHUNK_BYTES",
                                       kDefaultDownloadChunkBytes);
    const auto max_upload_bytes =
        ReadUnsignedEnv<std::uint64_t>("SPEEDTEST_MAX_UPLOAD_BYTES",
                                       kDefaultMaxUploadBytes);
    const auto max_static_file_bytes =
        ReadUnsignedEnv<std::uint64_t>("SPEEDTEST_MAX_STATIC_FILE_BYTES",
                                       kDefaultMaxStaticFileBytes);

    iouring_runtime::web::WebServerConfig config;
    config.host = ReadStringEnv("SPEEDTEST_HOST", "127.0.0.1");
    config.port = ReadUnsignedEnv<std::uint16_t>("SPEEDTEST_PORT", 3011);
    config.worker_count = ReadUnsignedEnv<std::uint16_t>("SPEEDTEST_WORKERS", 1);
    config.worker_affinity =
        ReadWorkerAffinityEnv("SPEEDTEST_WORKER_AFFINITY", config.worker_affinity);
    config.max_sessions_per_worker =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_MAX_SESSIONS_PER_WORKER", 0);
    config.ring.queue_depth =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_RING_QUEUE_DEPTH",
                                       config.ring.queue_depth);
    config.ring.buf_count =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_RING_BUF_COUNT",
                                       config.ring.buf_count);
    config.ring.buf_size =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_RING_BUF_SIZE",
                                       config.ring.buf_size);
    config.ring.submit_batch_size =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_RING_SUBMIT_BATCH",
                                       config.ring.submit_batch_size);
    config.ring.cqe_batch_budget =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_RING_CQE_BATCH_BUDGET",
                                       config.ring.cqe_batch_budget);
    config.ring.io_timeout =
        ReadMillisecondsEnv("SPEEDTEST_RING_IO_TIMEOUT_MS", config.ring.io_timeout);
    config.parser.max_body_bytes = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(max_upload_bytes,
                                std::numeric_limits<std::uint32_t>::max()));
    config.timeouts.inactivity =
        ReadMillisecondsEnv("SPEEDTEST_INACTIVITY_TIMEOUT_MS",
                            std::chrono::milliseconds{60000});
    config.timeouts.request =
        ReadMillisecondsEnv("SPEEDTEST_REQUEST_TIMEOUT_MS",
                            std::chrono::milliseconds{60000});
    config.backpressure.send_queue_max_pending =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_SEND_QUEUE_MAX_PENDING",
                                       config.backpressure.send_queue_max_pending);
    config.backpressure.send_queue_high_watermark =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_SEND_QUEUE_HIGH_WATERMARK",
                                       config.backpressure.send_queue_high_watermark);
    config.backpressure.send_queue_low_watermark =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_SEND_QUEUE_LOW_WATERMARK",
                                       config.backpressure.send_queue_low_watermark);
    config.backpressure.disconnect_on_high_watermark =
        ReadUnsignedEnv<std::uint32_t>("SPEEDTEST_DISCONNECT_ON_HIGH_WATERMARK", 0) != 0;

    iouring_runtime::web::WebServer server(config);
    iouring_runtime::web::WebServer::InstallStopSignalHandlers();

    server.Get("/", [&](RequestContext& ctx) {
        SendStaticFile(ctx, static_root, max_static_file_bytes);
    });

    server.Get("/downloading", [&](RequestContext& ctx) {
        SendDownload(ctx, download_bytes, download_chunk_bytes);
    });
    server.Head("/downloading", [&](RequestContext& ctx) {
        SendDownload(ctx, download_bytes, download_chunk_bytes);
    });
    server.Route(HttpMethod::kOptions, "/downloading", SendOptions);

    server.Get("/upload", SendUploadOk);
    server.Head("/upload", SendUploadOk);
    server.PostStream("/upload", iouring_runtime::web::HttpStreamHandler{
        .on_headers = [](RequestContext& ctx) {
            SendUploadOk(ctx);
            return false;
        },
    });
    server.Route(HttpMethod::kOptions, "/upload", SendOptions);

    server.Get("/*path", [&](RequestContext& ctx) {
        SendStaticFile(ctx, static_root, max_static_file_bytes);
    });
    server.Route(HttpMethod::kOptions, "/*path", SendOptions);

    server.Start();
    iouring_runtime::web::WebServer::WaitForStopSignal(std::chrono::seconds(1));
    server.Stop();
    return 0;
}
