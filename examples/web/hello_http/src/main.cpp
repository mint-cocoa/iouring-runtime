#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/web/WebServer.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <string>

using iouring_runtime::web::RequestContext;

namespace {

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

std::chrono::milliseconds ReadMillisecondsEnv(const char* name,
                                              std::chrono::milliseconds fallback) {
    if (const char* raw = std::getenv(name)) {
        return std::chrono::milliseconds(std::stoll(raw));
    }
    return fallback;
}

using WorkerAffinityMode = iouring_runtime::web::WebServerConfig::WorkerAffinityMode;

WorkerAffinityMode ReadWorkerAffinityEnv(
    const char* name,
    WorkerAffinityMode fallback) {
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
        "HELLO_HTTP_LOG_LEVEL");
}

std::string ReadEnvOrDefault(const char* name, std::string fallback) {
    if (const char* raw = std::getenv(name)) {
        return raw;
    }
    return fallback;
}

std::string JoinPath(std::string_view base, std::string_view leaf) {
    if (base.empty()) {
        return std::string(leaf);
    }
    std::string path(base);
    if (path.back() != '/') {
        path.push_back('/');
    }
    path.append(leaf);
    return path;
}

bool FileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return static_cast<bool>(file);
}

} // namespace

int main() {
    ConfigureLoggingFromEnv();

    iouring_runtime::web::WebServerConfig config;
    config.port = ReadUnsignedEnv<std::uint16_t>("HELLO_HTTP_PORT", 8080);
    config.worker_count = ReadUnsignedEnv<std::uint16_t>("HELLO_HTTP_WORKERS", 1);
    config.worker_affinity =
        ReadWorkerAffinityEnv("HELLO_HTTP_WORKER_AFFINITY", config.worker_affinity);
    config.max_sessions_per_worker =
        ReadUnsignedEnv<std::uint32_t>("HELLO_HTTP_MAX_SESSIONS_PER_WORKER", 0);
    config.ring.queue_depth =
        ReadUnsignedEnv<std::uint32_t>("HELLO_HTTP_RING_QUEUE_DEPTH",
                                       config.ring.queue_depth);
    config.ring.buf_count = ReadUnsignedEnv<std::uint32_t>("HELLO_HTTP_RING_BUF_COUNT",
                                                           config.ring.buf_count);
    config.ring.buf_size = ReadUnsignedEnv<std::uint32_t>("HELLO_HTTP_RING_BUF_SIZE",
                                                          config.ring.buf_size);
    config.ring.submit_batch_size =
        ReadUnsignedEnv<std::uint32_t>("HELLO_HTTP_RING_SUBMIT_BATCH",
                                       config.ring.submit_batch_size);
    config.ring.cqe_batch_budget =
        ReadUnsignedEnv<std::uint32_t>("HELLO_HTTP_RING_CQE_BATCH_BUDGET",
                                       config.ring.cqe_batch_budget);
    config.ring.io_timeout =
        ReadMillisecondsEnv("HELLO_HTTP_RING_IO_TIMEOUT_MS", config.ring.io_timeout);
    config.timeouts.inactivity =
        ReadMillisecondsEnv("HELLO_HTTP_INACTIVITY_TIMEOUT_MS",
                            config.timeouts.inactivity);
    config.timeouts.request =
        ReadMillisecondsEnv("HELLO_HTTP_REQUEST_TIMEOUT_MS", config.timeouts.request);
    config.backpressure.send_queue_max_pending =
        ReadUnsignedEnv<std::uint32_t>("HELLO_HTTP_SEND_QUEUE_MAX_PENDING",
                                       config.backpressure.send_queue_max_pending);
    config.backpressure.send_queue_high_watermark =
        ReadUnsignedEnv<std::uint32_t>("HELLO_HTTP_SEND_QUEUE_HIGH_WATERMARK",
                                       config.backpressure.send_queue_high_watermark);
    config.backpressure.send_queue_low_watermark =
        ReadUnsignedEnv<std::uint32_t>("HELLO_HTTP_SEND_QUEUE_LOW_WATERMARK",
                                       config.backpressure.send_queue_low_watermark);
    config.backpressure.disconnect_on_high_watermark =
        ReadUnsignedEnv<std::uint32_t>("HELLO_HTTP_DISCONNECT_ON_HIGH_WATERMARK", 0) != 0;

    const std::string static_root = ReadEnvOrDefault(
        "HELLO_HTTP_STATIC_ROOT",
        "scripts/wrk/reference_servers/www");
    const std::unordered_map<std::string, std::string> payload_files = {
        {"256b", JoinPath(static_root, "payload-256b.txt")},
        {"4k", JoinPath(static_root, "payload-4k.txt")},
        {"64k", JoinPath(static_root, "payload-64k.txt")},
    };

    iouring_runtime::web::WebServer server(config);
    iouring_runtime::web::WebServer::InstallStopSignalHandlers();

    server.Use([](RequestContext& ctx) {
        ctx.response.Header("X-Request-Id", std::string(ctx.request_id));
    });

    server.Use([](RequestContext& ctx) {
        if (ctx.request.path != "/private") {
            return;
        }
        if (ctx.request.GetHeader("X-API-Key") == "demo-key") {
            return;
        }
        ctx.response
            .Error(iouring_runtime::web::HttpStatus::kUnauthorized,
                   "missing or invalid api key")
            .Send();
    });

    server.Get("/", [](RequestContext& ctx) {
        ctx.response.Text("hello from iouring_runtime_web")
            .Send();
    });

    server.Get("/health", [](RequestContext& ctx) {
        ctx.response.Text("ok")
            .Send();
    });

    server.Get("/payload/:size", [payload_files](RequestContext& ctx) {
        const auto size = ctx.request.ParamDecoded("size");
        const auto it = payload_files.find(std::string(size));
        if (it == payload_files.end()) {
            ctx.response
                .Error(iouring_runtime::web::HttpStatus::kNotFound, "payload not found")
                .Send();
            return;
        }
        if (!FileExists(it->second)) {
            ctx.response
                .Error(iouring_runtime::web::HttpStatus::kInternalServerError, "payload file missing")
                .Send();
            return;
        }
        if (!ctx.response.SendFile(it->second, "text/plain")) {
            ctx.response
                .Error(iouring_runtime::web::HttpStatus::kInternalServerError, "failed to send payload")
                .Send();
        }
    });

    server.Get("/hello/:name", [](RequestContext& ctx) {
        auto name = ctx.request.ParamDecoded("name");
        const auto lang = ctx.request.QueryParamDecoded("lang");

        std::string body = "hello, " + name;
        if (!lang.empty()) {
            body += " (" + lang + ")";
        }

        ctx.response.Text(std::move(body)).Send();
    });

    server.Get("/cookies", [](RequestContext& ctx) {
        const auto theme = ctx.request.Cookie("theme");
        if (theme.empty()) {
            ctx.response
                .Error(iouring_runtime::web::HttpStatus::kBadRequest,
                       "missing theme cookie")
                .Send();
            return;
        }

        ctx.response.Json("{\"theme\":\"" + std::string(theme) + "\"}").Send();
    });

    server.Get("/private", [](RequestContext& ctx) {
        ctx.response.Json(R"({"ok":true})").Send();
    });

    server.Start();
    iouring_runtime::web::WebServer::WaitForStopSignal(std::chrono::seconds(1));
    server.Stop();
    return 0;
}
