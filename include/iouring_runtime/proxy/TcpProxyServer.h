#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace iouring_runtime::core::buffer {
class BufferPool;
} // namespace iouring_runtime::core::buffer

namespace iouring_runtime::core::ring {
class IoRing;
} // namespace iouring_runtime::core::ring

namespace iouring_runtime::proxy {

namespace detail {
struct TcpProxyResolvedEndpoint;
struct TcpProxyResolvedRoute;
struct TcpProxyWorker;
struct DownstreamTlsContext;
class ProxyBridge;
} // namespace detail

struct TcpProxyConfig {
    enum class WorkerAffinityMode {
        kOff,
        kPhysicalCores,
        kLogicalCpus,
    };

    struct RingOptions {
        std::uint32_t queue_depth = 2048;
        std::uint32_t buf_count = 4096;
        std::uint32_t buf_size = 4096;
        std::uint32_t submit_batch_size = 1;
        std::uint32_t cqe_batch_budget = 0;
        std::chrono::milliseconds io_timeout{1};
    };

    struct TimeoutOptions {
        std::chrono::milliseconds inactivity{30000};
        std::chrono::milliseconds connect{3000};
    };

    struct ShutdownOptions {
        std::chrono::milliseconds drain_timeout{1000};
        std::chrono::milliseconds force_close_timeout{200};
    };

    struct BackpressureOptions {
        std::uint32_t send_queue_max_pending = 16384;
        std::uint32_t send_queue_high_watermark = 8192;
        std::uint32_t send_queue_low_watermark = 2048;
        bool disconnect_on_high_watermark = false;
    };

    struct DownstreamTlsOptions {
        std::string certificate_chain_file;
        std::string private_key_file;

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
    BackpressureOptions backpressure;
    DownstreamTlsOptions downstream_tls;
    CertbotOptions certbot;
};

class TcpProxyServer {
public:
    explicit TcpProxyServer(const TcpProxyConfig& config);
    ~TcpProxyServer();

    void Start();
    void Stop();
    [[nodiscard]] bool ReloadDownstreamTlsContext();

    static void InstallStopSignalHandlers();
    static void RequestStop() noexcept;
    static bool StopRequested() noexcept;
    static void RequestReload() noexcept;
    static bool ReloadRequested() noexcept;
    static bool ConsumeReloadRequest() noexcept;
    static void WaitForStopSignal(
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds{100});
    static void ResetStopRequestedForTests() noexcept;
    static void ResetReloadRequestedForTests() noexcept;

private:
    std::unique_ptr<detail::TcpProxyResolvedEndpoint> ResolveUpstreamEndpoint() const;
    std::vector<detail::TcpProxyResolvedRoute> ResolveUpstreamRoutes() const;
    const detail::TcpProxyResolvedEndpoint& SelectUpstreamEndpoint(
        std::string_view hostname) const;
    bool StartConnector(core::ring::IoRing& ring,
                        core::buffer::BufferPool& pool,
                        const detail::TcpProxyResolvedEndpoint& endpoint,
                        detail::TcpProxyWorker& worker,
                        const std::shared_ptr<detail::ProxyBridge>& bridge);
    std::shared_ptr<detail::DownstreamTlsContext> BuildDownstreamTlsContext() const;
    std::shared_ptr<detail::DownstreamTlsContext> CurrentDownstreamTlsContext() const;
    void StopAccepting();
    void CancelConnectors();
    void DrainSessions(bool force_close);
    bool WaitForZeroConnections(std::chrono::milliseconds timeout);
    void ConfigureWorkerAffinity(detail::TcpProxyWorker& worker);
    void WorkerLoop(detail::TcpProxyWorker& worker);

    TcpProxyConfig config_;
    std::vector<std::unique_ptr<detail::TcpProxyWorker>> workers_;
    std::unique_ptr<detail::TcpProxyResolvedEndpoint> upstream_endpoint_;
    std::vector<detail::TcpProxyResolvedRoute> upstream_routes_;
    mutable std::mutex downstream_tls_context_mu_;
    std::shared_ptr<detail::DownstreamTlsContext> downstream_tls_context_;
    std::atomic<bool> running_{false};
};

} // namespace iouring_runtime::proxy
