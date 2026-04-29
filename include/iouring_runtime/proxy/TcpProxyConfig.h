#pragma once

#include <iouring_runtime/core/ServerOptions.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace iouring_runtime::proxy {

struct TcpProxyConfig {
    enum class WorkerAffinityMode {
        kOff,
        kPhysicalCores,
        kLogicalCpus,
    };

    using RingOptions = core::ServerRingOptions;

    struct TimeoutOptions : core::InactivityTimeoutOptions {
        std::chrono::milliseconds connect{3000};
    };

    using ShutdownOptions = core::ServerShutdownOptions;
    using BackpressureOptions = core::SendQueueBackpressureOptions;

    struct DownstreamTlsOptions {
        std::string certificate_chain_file;
        std::string private_key_file;
        std::uint32_t pending_plaintext_limit = 16 * 1024 * 1024;

        bool Enabled() const noexcept {
            return !certificate_chain_file.empty() || !private_key_file.empty();
        }
    };

    struct CertbotOptions {
        std::string challenge_host = "0.0.0.0";
        std::uint16_t challenge_port = 0;
        std::string challenge_webroot;

        bool Enabled() const noexcept {
            return challenge_port != 0 && !challenge_webroot.empty();
        }
    };

    struct MetricsOptions {
        std::string file_path;
        std::chrono::milliseconds interval{1000};
    };

    struct UpstreamRoute {
        std::string hostname;
        std::string upstream_host;
        std::uint16_t upstream_port = 0;
    };

    std::string listen_host = "0.0.0.0";
    std::uint16_t listen_port = 8080;
    std::string upstream_host = "127.0.0.1";
    std::uint16_t upstream_port = 9000;
    std::vector<UpstreamRoute> upstream_routes;
    std::uint16_t worker_count = 4;
    WorkerAffinityMode worker_affinity = WorkerAffinityMode::kOff;
    std::uint32_t max_sessions_per_worker = 0;
    std::uint32_t pending_connect_buffer_limit = 256 * 1024;
    RingOptions ring;
    TimeoutOptions timeouts;
    ShutdownOptions shutdown;
    BackpressureOptions backpressure{
        .send_queue_max_pending = 16384,
        .send_queue_high_watermark = 8192,
        .send_queue_low_watermark = 2048,
        .disconnect_on_high_watermark = false,
    };
    DownstreamTlsOptions downstream_tls;
    CertbotOptions certbot;
    MetricsOptions metrics;
};

} // namespace iouring_runtime::proxy
