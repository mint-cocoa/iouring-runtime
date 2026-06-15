#pragma once

#include <iouring/stream/TcpProxyConfig.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace iouring::core::buffer {
class BufferPool;
} // namespace iouring::core::buffer

namespace iouring::event {
class IoRing;
} // namespace iouring::event

namespace iouring::stream {

namespace detail {
struct TcpProxyResolvedEndpoint;
struct TcpProxyResolvedRoute;
struct TcpProxyWorker;
struct DownstreamTlsContext;
class ProxyBridge;
} // namespace detail

class TcpProxyServer {
public:
    explicit TcpProxyServer(const TcpProxyConfig& config);
    ~TcpProxyServer();

    void Start();
    void Stop();
    [[nodiscard]] bool ReloadDownstreamTlsContext();
    [[nodiscard]] std::string SnapshotMetricsJson() const;
    [[nodiscard]] bool WriteMetricsSnapshot() const;

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
    bool StartConnector(event::IoRing& ring,
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
    void ConfigureWorkerThread(detail::TcpProxyWorker& worker);
    void StartMetricsWriter();
    void StopMetricsWriter();

    TcpProxyConfig config_;
    std::vector<std::unique_ptr<detail::TcpProxyWorker>> workers_;
    std::unique_ptr<detail::TcpProxyResolvedEndpoint> upstream_endpoint_;
    std::vector<detail::TcpProxyResolvedRoute> upstream_routes_;
    mutable std::mutex downstream_tls_context_mu_;
    std::shared_ptr<detail::DownstreamTlsContext> downstream_tls_context_;
    std::atomic<bool> running_{false};
    std::atomic<bool> metrics_writer_running_{false};
    std::thread metrics_thread_;
    std::chrono::steady_clock::time_point started_at_{};
    std::atomic<std::int64_t> last_tls_reload_success_unix_{0};
    std::atomic<std::int64_t> last_tls_reload_failure_unix_{0};
};

} // namespace iouring::stream
