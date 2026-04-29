#include "TcpReverseProxyConfig.h"

#include <iouring_runtime/observability/Logging.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace iouring_runtime::examples::tcp_reverse_proxy {

namespace {

namespace obs = iouring_runtime::observability;
constexpr auto kLogCategory = obs::LogCategory::kProxy;

using WorkerAffinityMode = proxy::TcpProxyConfig::WorkerAffinityMode;
using UpstreamRoute = proxy::TcpProxyConfig::UpstreamRoute;

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

bool HasEnv(const char* name) {
    return std::getenv(name) != nullptr;
}

template <typename T>
std::optional<T> ParseUnsigned(std::string_view raw) {
    const auto text = Trim(raw);
    if (text.empty()) {
        return std::nullopt;
    }

    unsigned long long value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last ||
        value > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
        return std::nullopt;
    }

    return static_cast<T>(value);
}

std::optional<long long> ParseSigned(std::string_view raw) {
    const auto text = Trim(raw);
    if (text.empty()) {
        return std::nullopt;
    }

    long long value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }

    return value;
}

template <typename T>
T ReadUnsignedEnv(const char* name, T fallback) {
    if (const char* raw = std::getenv(name)) {
        if (auto value = ParseUnsigned<T>(raw)) {
            return *value;
        }
        obs::LogWarn(kLogCategory,
                     "tcp_reverse_proxy: ignoring invalid unsigned env {}='{}'",
                     name, raw);
    }
    return fallback;
}

std::chrono::milliseconds ReadMillisecondsEnv(
    const char* name, std::chrono::milliseconds fallback) {
    if (const char* raw = std::getenv(name)) {
        if (auto value = ParseSigned(raw)) {
            return std::chrono::milliseconds(*value);
        }
        obs::LogWarn(kLogCategory,
                     "tcp_reverse_proxy: ignoring invalid millisecond env {}='{}'",
                     name, raw);
    }
    return fallback;
}

std::string ReadStringEnv(const char* name, std::string fallback) {
    if (const char* raw = std::getenv(name)) {
        return raw;
    }
    return fallback;
}

WorkerAffinityMode ReadWorkerAffinityEnv(
    const char* name, WorkerAffinityMode fallback) {
    if (const char* raw = std::getenv(name)) {
        const std::string_view value = raw;
        if (value == "off") {
            return WorkerAffinityMode::kOff;
        }
        if (value == "physical") {
            return WorkerAffinityMode::kPhysicalCores;
        }
        if (value == "logical") {
            return WorkerAffinityMode::kLogicalCpus;
        }
        obs::LogWarn(kLogCategory,
                     "tcp_reverse_proxy: ignoring invalid worker affinity {}='{}'",
                     name, raw);
    }
    return fallback;
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
    if (host.empty()) {
        return std::nullopt;
    }

    auto port_value = ParseUnsigned<std::uint16_t>(port_text);
    if (!port_value || *port_value == 0) {
        return std::nullopt;
    }

    return std::pair<std::string, std::uint16_t>{host, *port_value};
}

std::optional<UpstreamRoute> ParseRouteAssignment(std::string_view token) {
    const auto eq = token.find('=');
    if (eq == std::string::npos) {
        return std::nullopt;
    }

    auto hostname = Trim(token.substr(0, eq));
    auto upstream = ParseUpstreamTarget(token.substr(eq + 1));
    if (hostname.empty() || !upstream) {
        return std::nullopt;
    }

    return UpstreamRoute{
        .hostname = std::move(hostname),
        .upstream_host = std::move(upstream->first),
        .upstream_port = upstream->second,
    };
}

void AppendRouteToken(std::vector<UpstreamRoute>& routes,
                      std::string_view token,
                      std::string_view source) {
    const auto trimmed = Trim(token);
    if (trimmed.empty()) {
        return;
    }

    auto route = ParseRouteAssignment(trimmed);
    if (!route) {
        obs::LogWarn(kLogCategory,
                     "tcp_reverse_proxy: ignoring invalid route '{}' from {}",
                     trimmed, source);
        return;
    }
    routes.push_back(std::move(*route));
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
        AppendRouteToken(routes, token, name);
    }

    return routes;
}

std::vector<UpstreamRoute> ReadUpstreamRoutesFileEnv(const char* name) {
    std::vector<UpstreamRoute> routes;
    const char* raw_path = std::getenv(name);
    if (!raw_path || std::string_view(raw_path).empty()) {
        return routes;
    }

    const std::string path = raw_path;
    std::ifstream file(path);
    if (!file) {
        obs::LogWarn(kLogCategory,
                     "tcp_reverse_proxy: failed to open route config file '{}'",
                     path);
        return routes;
    }

    std::string line;
    std::size_t line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;

        if (const auto comment = line.find('#');
            comment != std::string::npos) {
            line.erase(comment);
        }

        std::stringstream ss(line);
        std::string token;
        while (std::getline(ss, token, ',')) {
            AppendRouteToken(routes, token,
                             path + ":" + std::to_string(line_no));
        }
    }

    return routes;
}

void AppendFileRoutes(proxy::TcpProxyConfig& config) {
    auto file_routes =
        ReadUpstreamRoutesFileEnv("TCP_PROXY_UPSTREAM_ROUTES_FILE");
    config.upstream_routes.insert(config.upstream_routes.end(),
                                  file_routes.begin(), file_routes.end());
}

void ApplyBackpressureWatermarkDefaults(proxy::TcpProxyConfig& config,
                                        bool high_watermark_set,
                                        bool low_watermark_set) {
    if (config.backpressure.send_queue_max_pending == 0) {
        return;
    }

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

} // namespace

void ConfigureLoggingFromEnv() {
    obs::ConfigureLoggingFromEnv("TCP_PROXY_LOG_LEVEL");
}

AppConfig LoadConfigFromEnv() {
    AppConfig app_config;
    auto& config = app_config.stream;
    auto& acme_http = app_config.acme_http;

    config.listen_host =
        ReadStringEnv("TCP_PROXY_LISTEN_HOST", config.listen_host);
    config.listen_port =
        ReadUnsignedEnv<std::uint16_t>("TCP_PROXY_LISTEN_PORT", 18080);
    config.upstream_host =
        ReadStringEnv("TCP_PROXY_UPSTREAM_HOST", config.upstream_host);
    config.upstream_port =
        ReadUnsignedEnv<std::uint16_t>("TCP_PROXY_UPSTREAM_PORT", 8080);
    config.upstream_routes =
        ReadUpstreamRoutesEnv("TCP_PROXY_UPSTREAM_ROUTES");
    AppendFileRoutes(config);

    config.downstream_tls.certificate_chain_file =
        ReadStringEnv("TCP_PROXY_TLS_CERT_FILE",
                      config.downstream_tls.certificate_chain_file);
    config.downstream_tls.private_key_file =
        ReadStringEnv("TCP_PROXY_TLS_KEY_FILE",
                      config.downstream_tls.private_key_file);
    config.downstream_tls.pending_plaintext_limit =
        ReadUnsignedEnv<std::uint32_t>(
            "TCP_PROXY_TLS_PENDING_PLAINTEXT_LIMIT",
            config.downstream_tls.pending_plaintext_limit);

    acme_http.listen_host =
        ReadStringEnv("TCP_PROXY_CERTBOT_CHALLENGE_HOST",
                      acme_http.listen_host);
    acme_http.listen_port =
        ReadUnsignedEnv<std::uint16_t>("TCP_PROXY_CERTBOT_CHALLENGE_PORT",
                                       acme_http.listen_port);
    acme_http.webroot =
        ReadStringEnv("TCP_PROXY_CERTBOT_CHALLENGE_WEBROOT",
                      acme_http.webroot);

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
                                       config.backpressure
                                           .send_queue_max_pending);
    config.backpressure.send_queue_high_watermark =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_SEND_QUEUE_HIGH_WATERMARK",
                                       config.backpressure
                                           .send_queue_high_watermark);
    config.backpressure.send_queue_low_watermark =
        ReadUnsignedEnv<std::uint32_t>("TCP_PROXY_SEND_QUEUE_LOW_WATERMARK",
                                       config.backpressure
                                           .send_queue_low_watermark);
    config.backpressure.disconnect_on_high_watermark =
        ReadUnsignedEnv<std::uint32_t>(
            "TCP_PROXY_DISCONNECT_ON_HIGH_WATERMARK", 0) != 0;
    ApplyBackpressureWatermarkDefaults(config, high_watermark_set,
                                       low_watermark_set);

    acme_http.max_sessions_per_worker = config.max_sessions_per_worker;
    acme_http.send_queue_max_pending =
        config.backpressure.send_queue_max_pending;
    acme_http.inactivity_timeout = config.timeouts.inactivity;
    acme_http.ring.io_timeout = config.ring.io_timeout;
    acme_http.shutdown.drain_timeout = config.shutdown.drain_timeout;
    acme_http.shutdown.force_close_timeout =
        config.shutdown.force_close_timeout;

    return app_config;
}

} // namespace iouring_runtime::examples::tcp_reverse_proxy
