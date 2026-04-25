#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/web/WebServer.h>

#include <chrono>
#include <cstdlib>
#include <limits>
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

    iouring_runtime::web::WebServer server(config);
    iouring_runtime::web::WebServer::InstallStopSignalHandlers();

    server.Get("/", [](RequestContext& ctx) {
        ctx.response.ContentType("text/plain")
            .Body("hello from iouring_runtime_web")
            .Send();
    });

    server.Get("/health", [](RequestContext& ctx) {
        ctx.response.ContentType("text/plain")
            .Body("ok")
            .Send();
    });

    server.Start();
    iouring_runtime::web::WebServer::WaitForStopSignal(std::chrono::seconds(1));
    server.Stop();
    return 0;
}
