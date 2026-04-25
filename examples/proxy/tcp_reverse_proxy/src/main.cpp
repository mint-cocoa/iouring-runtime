#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/proxy/TcpProxyServer.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace obs = iouring_runtime::observability;
constexpr auto kLogCategory = obs::LogCategory::kProxy;

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

bool HasEnv(const char* name) {
    return std::getenv(name) != nullptr;
}

std::chrono::milliseconds ReadMillisecondsEnv(
    const char* name, std::chrono::milliseconds fallback) {
    if (const char* raw = std::getenv(name)) {
        return std::chrono::milliseconds(std::stoll(raw));
    }
    return fallback;
}

std::string ReadStringEnv(const char* name, std::string fallback) {
    if (const char* raw = std::getenv(name)) {
        return raw;
    }
    return fallback;
}

using WorkerAffinityMode = iouring_runtime::proxy::TcpProxyConfig::WorkerAffinityMode;
using UpstreamRoute = iouring_runtime::proxy::TcpProxyConfig::UpstreamRoute;

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

std::string Trim(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() &&
           std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

std::optional<std::pair<std::string, std::uint16_t>> ParseUpstreamTarget(
    std::string_view target) {
    const auto text = Trim(target);
    if (text.empty()) {
        return std::nullopt;
    }

    std::string host;
    std::string port_text;
    if (text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string::npos || close + 1 >= text.size() ||
            text[close + 1] != ':') {
            return std::nullopt;
        }
        host = text.substr(1, close - 1);
        port_text = text.substr(close + 2);
    } else {
        const auto colon = text.rfind(':');
        if (colon == std::string::npos) {
            return std::nullopt;
        }
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
    }

    host = Trim(host);
    port_text = Trim(port_text);
    if (host.empty() || port_text.empty()) {
        return std::nullopt;
    }

    unsigned long long port_value = 0;
    try {
        port_value = std::stoull(port_text);
    } catch (...) {
        return std::nullopt;
    }
    if (port_value == 0 ||
        port_value > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }

    return std::pair<std::string, std::uint16_t>{
        host, static_cast<std::uint16_t>(port_value)};
}

std::vector<UpstreamRoute> ReadUpstreamRoutesEnv(const char* name) {
    std::vector<UpstreamRoute> routes;
    const char* raw = std::getenv(name);
    if (!raw || std::string_view(raw).empty()) {
        return routes;
    }

    std::stringstream ss(raw);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = Trim(token);
        if (token.empty()) {
            continue;
        }

        const auto eq = token.find('=');
        if (eq == std::string::npos) {
            obs::LogWarn(kLogCategory,
                         "tcp_reverse_proxy: ignoring invalid route '{}'",
                         token);
            continue;
        }

        auto hostname = Trim(std::string_view(token).substr(0, eq));
        auto upstream =
            ParseUpstreamTarget(std::string_view(token).substr(eq + 1));
        if (hostname.empty() || !upstream) {
            obs::LogWarn(kLogCategory,
                         "tcp_reverse_proxy: ignoring invalid route '{}'",
                         token);
            continue;
        }

        routes.push_back(UpstreamRoute{
            .hostname = std::move(hostname),
            .upstream_host = std::move(upstream->first),
            .upstream_port = upstream->second,
        });
    }

    return routes;
}

void ConfigureLoggingFromEnv() {
    obs::ConfigureLoggingFromEnv("TCP_PROXY_LOG_LEVEL");
}

} // namespace

int main() {
    ConfigureLoggingFromEnv();

    iouring_runtime::proxy::TcpProxyConfig config;
    config.listen_host = ReadStringEnv("TCP_PROXY_LISTEN_HOST", config.listen_host);
    config.listen_port =
        ReadUnsignedEnv<std::uint16_t>("TCP_PROXY_LISTEN_PORT", 18080);
    config.upstream_host =
        ReadStringEnv("TCP_PROXY_UPSTREAM_HOST", config.upstream_host);
    config.upstream_port =
        ReadUnsignedEnv<std::uint16_t>("TCP_PROXY_UPSTREAM_PORT", 8080);
    config.upstream_routes =
        ReadUpstreamRoutesEnv("TCP_PROXY_UPSTREAM_ROUTES");
    config.downstream_tls.certificate_chain_file =
        ReadStringEnv("TCP_PROXY_TLS_CERT_FILE",
                      config.downstream_tls.certificate_chain_file);
    config.downstream_tls.private_key_file =
        ReadStringEnv("TCP_PROXY_TLS_KEY_FILE",
                      config.downstream_tls.private_key_file);
    config.certbot.challenge_host =
        ReadStringEnv("TCP_PROXY_CERTBOT_CHALLENGE_HOST",
                      config.certbot.challenge_host);
    config.certbot.challenge_port =
        ReadUnsignedEnv<std::uint16_t>("TCP_PROXY_CERTBOT_CHALLENGE_PORT",
                                       config.certbot.challenge_port);
    config.certbot.challenge_webroot =
        ReadStringEnv("TCP_PROXY_CERTBOT_CHALLENGE_WEBROOT",
                      config.certbot.challenge_webroot);
    config.metrics.file_path =
        ReadStringEnv("TCP_PROXY_METRICS_FILE",
                      "/run/iouring-runtime/tcp_reverse_proxy.metrics.json");
    config.metrics.interval =
        ReadMillisecondsEnv("TCP_PROXY_METRICS_INTERVAL_MS",
                            config.metrics.interval);
    config.worker_count =
        ReadUnsignedEnv<std::uint16_t>("TCP_PROXY_WORKERS", 1);
    config.worker_affinity =
        ReadWorkerAffinityEnv("TCP_PROXY_WORKER_AFFINITY",
                              config.worker_affinity);
    config.max_sessions_per_worker =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_MAX_SESSIONS_PER_WORKER",
                                       config.max_sessions_per_worker);
    config.pending_connect_buffer_limit =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_PENDING_CONNECT_BUFFER_LIMIT",
                                       config.pending_connect_buffer_limit);
    config.ring.queue_depth =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_RING_QUEUE_DEPTH",
                                       config.ring.queue_depth);
    config.ring.buf_count =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_RING_BUF_COUNT",
                                       config.ring.buf_count);
    config.ring.buf_size =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_RING_BUF_SIZE",
                                       config.ring.buf_size);
    config.ring.submit_batch_size =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_RING_SUBMIT_BATCH",
                                       config.ring.submit_batch_size);
    config.ring.cqe_batch_budget =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_RING_CQE_BATCH_BUDGET",
                                       config.ring.cqe_batch_budget);
    config.ring.io_timeout =
        ReadMillisecondsEnv("TCP_PROXY_RING_IO_TIMEOUT_MS",
                            config.ring.io_timeout);
    config.timeouts.inactivity =
        ReadMillisecondsEnv("TCP_PROXY_INACTIVITY_TIMEOUT_MS",
                            config.timeouts.inactivity);
    config.timeouts.connect =
        ReadMillisecondsEnv("TCP_PROXY_CONNECT_TIMEOUT_MS",
                            config.timeouts.connect);
    const bool high_watermark_set = HasEnv("TCP_PROXY_SEND_QUEUE_HIGH_WATERMARK");
    const bool low_watermark_set = HasEnv("TCP_PROXY_SEND_QUEUE_LOW_WATERMARK");
    config.backpressure.send_queue_max_pending =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_SEND_QUEUE_MAX_PENDING",
                                       config.backpressure.send_queue_max_pending);
    config.backpressure.send_queue_high_watermark =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_SEND_QUEUE_HIGH_WATERMARK",
                                       config.backpressure.send_queue_high_watermark);
    config.backpressure.send_queue_low_watermark =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_SEND_QUEUE_LOW_WATERMARK",
                                       config.backpressure.send_queue_low_watermark);
    config.backpressure.disconnect_on_high_watermark =
        ReadUnsignedEnv<std::uint32_t>(
            "TCP_PROXY_DISCONNECT_ON_HIGH_WATERMARK", 0) != 0;
    if (config.backpressure.send_queue_max_pending > 0) {
        if (!high_watermark_set ||
            config.backpressure.send_queue_high_watermark >=
                config.backpressure.send_queue_max_pending) {
            config.backpressure.send_queue_high_watermark =
                std::max<std::uint32_t>(
                    1, config.backpressure.send_queue_max_pending / 2);
        }
        if (!low_watermark_set ||
            config.backpressure.send_queue_low_watermark >
                config.backpressure.send_queue_high_watermark) {
            config.backpressure.send_queue_low_watermark =
                config.backpressure.send_queue_high_watermark / 4;
        }
    }

    iouring_runtime::proxy::TcpProxyServer server(config);
    iouring_runtime::proxy::TcpProxyServer::InstallStopSignalHandlers();
    server.Start();

    while (!iouring_runtime::proxy::TcpProxyServer::StopRequested()) {
        if (iouring_runtime::proxy::TcpProxyServer::ConsumeReloadRequest()) {
            if (!server.ReloadDownstreamTlsContext()) {
                obs::LogError(
                    kLogCategory,
                    "tcp_reverse_proxy: failed to reload downstream TLS context");
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    server.Stop();
    return 0;
}
