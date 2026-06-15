#include <iouring/stream/TcpProxyServer.h>

#include "DownstreamTlsContext.h"
#include "ProxyCommon.h"
#include "ProxyConnector.h"
#include "ProxySessions.h"

#include <iouring/observability/Logging.h>

#include <atomic>
#include <cctype>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <netdb.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace obs = iouring::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kProxy;
}

namespace iouring::stream {

namespace {

	std::atomic<bool> g_stop_requested{false};
	std::atomic<bool> g_reload_requested{false};

	std::int64_t UnixSecondsNow() {
	    return std::chrono::duration_cast<std::chrono::seconds>(
	               std::chrono::system_clock::now().time_since_epoch())
	        .count();
	}

	std::string JsonEscape(std::string_view text) {
	    std::string out;
	    out.reserve(text.size() + 8);
	    for (const char ch : text) {
	        switch (ch) {
	        case '"':
	            out += "\\\"";
	            break;
	        case '\\':
	            out += "\\\\";
	            break;
	        case '\n':
	            out += "\\n";
	            break;
	        case '\r':
	            out += "\\r";
	            break;
	        case '\t':
	            out += "\\t";
	            break;
	        default:
	            if (static_cast<unsigned char>(ch) < 0x20) {
	                out += ' ';
	            } else {
	                out += ch;
	            }
	            break;
	        }
	    }
	    return out;
	}

	bool WriteFileAtomically(const std::string& path, std::string_view content) {
	    if (path.empty()) {
	        return false;
	    }

	    std::error_code ec;
	    const std::filesystem::path final_path(path);
	    const auto parent = final_path.parent_path();
	    if (!parent.empty()) {
	        std::filesystem::create_directories(parent, ec);
	        if (ec) {
	            obs::LogWarn(kLogCategory, "TcpProxyServer: failed to create metrics directory {}: {}",
	                         parent.string(), ec.message());
	            return false;
	        }
	    }

	    const std::string tmp_path =
	        path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
	    {
	        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
	        if (!out) {
	            obs::LogWarn(kLogCategory, "TcpProxyServer: failed to open metrics temp file {}",
	                         tmp_path);
	            return false;
	        }
	        out << content;
	        if (!out) {
	            std::filesystem::remove(tmp_path, ec);
	            obs::LogWarn(kLogCategory, "TcpProxyServer: failed to write metrics temp file {}",
	                         tmp_path);
	            return false;
	        }
	    }

	    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
	        std::filesystem::remove(tmp_path, ec);
	        obs::LogWarn(kLogCategory, "TcpProxyServer: failed to publish metrics file {}",
	                     path);
	        return false;
	    }
	    return true;
	}

void ProxyStopSignalHandler(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

void ProxyReloadSignalHandler(int) {
    g_reload_requested.store(true, std::memory_order_relaxed);
}

std::optional<detail::TcpProxyResolvedEndpoint> ResolveEndpoint(
    std::string_view host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%u",
                  static_cast<unsigned>(port));

    addrinfo* results = nullptr;
    const int rc = ::getaddrinfo(std::string(host).c_str(), port_text,
                                 &hints, &results);
    if (rc != 0) {
        obs::LogError(kLogCategory, "TcpProxyServer: getaddrinfo({}:{}) failed: {}",
                      host, port, ::gai_strerror(rc));
        return std::nullopt;
    }

    std::optional<detail::TcpProxyResolvedEndpoint> endpoint;
    for (auto* it = results; it != nullptr; it = it->ai_next) {
        if (it->ai_family != AF_INET && it->ai_family != AF_INET6) {
            continue;
        }
        if (it->ai_addrlen > static_cast<socklen_t>(sizeof(sockaddr_storage))) {
            continue;
        }

        detail::TcpProxyResolvedEndpoint resolved;
        std::memcpy(&resolved.storage, it->ai_addr,
                    static_cast<std::size_t>(it->ai_addrlen));
        resolved.len = static_cast<socklen_t>(it->ai_addrlen);
        resolved.family = it->ai_family;
        resolved.display =
            std::string(host) + ":" + std::to_string(static_cast<unsigned>(port));
        endpoint = std::move(resolved);
        break;
    }

    ::freeaddrinfo(results);
    if (!endpoint) {
        obs::LogError(kLogCategory, "TcpProxyServer: no usable upstream address for {}:{}",
                      host, port);
    }
    return endpoint;
}

std::string NormalizeHostname(std::string_view hostname) {
    std::string normalized;
    normalized.reserve(hostname.size());
    for (unsigned char ch : hostname) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    while (!normalized.empty() && normalized.back() == '.') {
        normalized.pop_back();
    }
    return normalized;
}

bool HostnameMatches(std::string_view pattern, std::string_view hostname) {
    const auto normalized_pattern = NormalizeHostname(pattern);
    const auto normalized_hostname = NormalizeHostname(hostname);
    if (normalized_pattern.empty() || normalized_hostname.empty()) {
        return false;
    }

    if (normalized_pattern.starts_with("*.")) {
        const std::string_view suffix(normalized_pattern.data() + 1,
                                      normalized_pattern.size() - 1);
        return normalized_hostname.size() > suffix.size() &&
               normalized_hostname.ends_with(suffix);
    }

    return normalized_pattern == normalized_hostname;
}

} // namespace

TcpProxyServer::TcpProxyServer(const TcpProxyConfig& config)
    : config_(config) {}

TcpProxyServer::~TcpProxyServer() {
    Stop();
}

void TcpProxyServer::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    started_at_ = std::chrono::steady_clock::now();

    upstream_endpoint_ = ResolveUpstreamEndpoint();
    if (!upstream_endpoint_) {
        running_.store(false, std::memory_order_release);
        return;
    }
    upstream_routes_ = ResolveUpstreamRoutes();
    if (upstream_routes_.size() != config_.upstream_routes.size()) {
        upstream_endpoint_.reset();
        running_.store(false, std::memory_order_release);
        return;
    }

    {
        auto tls_context = BuildDownstreamTlsContext();
        if (config_.downstream_tls.Enabled() && !tls_context) {
            upstream_endpoint_.reset();
            running_.store(false, std::memory_order_release);
            return;
        }
        std::lock_guard lock(downstream_tls_context_mu_);
        downstream_tls_context_ = std::move(tls_context);
    }

    bool any_main_listener = false;

    for (std::uint16_t i = 0; i < config_.worker_count; ++i) {
        auto worker = std::make_unique<detail::TcpProxyWorker>();
        auto* raw_worker = worker.get();
        worker->index = i;

        net::SessionFactory proxy_factory =
            [this, raw_worker](int fd, event::IoRing& ring,
                               core::buffer::BufferPool& pool, core::ContextId)
                -> net::SessionRef {
            auto bridge = std::make_shared<detail::ProxyBridge>(
                config_.pending_connect_buffer_limit);

            auto tls_context = CurrentDownstreamTlsContext();
            net::SessionRef session;
            if (tls_context) {
                auto* ring_ptr = &ring;
                auto* pool_ptr = &pool;
                session = detail::CreateTlsProxySession(
                    fd, ring, pool, bridge, std::move(tls_context),
                    [this, raw_worker, bridge, ring_ptr, pool_ptr](
                        std::string_view server_name) {
                        const auto& endpoint = SelectUpstreamEndpoint(server_name);
                        return StartConnector(*ring_ptr, *pool_ptr, endpoint,
                                              *raw_worker, bridge);
                    },
                    detail::PeerRole::kDownstream,
                    config_.backpressure.send_queue_max_pending,
                    config_.downstream_tls.pending_plaintext_limit);
            } else {
                if (!StartConnector(ring, pool, *upstream_endpoint_, *raw_worker,
                                    bridge)) {
                    return {};
                }
                session = detail::CreatePlainProxySession(
                    fd, ring, pool, bridge, detail::PeerRole::kDownstream,
                    config_.backpressure.send_queue_max_pending);
            }

            detail::ConfigureProxySession(session, *raw_worker, config_);
            bridge->AttachDownstream(detail::ToProxyPeer(session));
            return session;
        };

        event::WorkerConfig worker_config;
        worker_config.id = i;
        worker_config.address = core::Address{
            config_.listen_host, config_.listen_port};
        worker_config.ring.queue_depth = config_.ring.queue_depth;
        worker_config.ring.buf_ring.buf_count = config_.ring.buf_count;
        worker_config.ring.buf_ring.buf_size = config_.ring.buf_size;
        worker_config.ring.buf_ring.group_id = static_cast<std::uint16_t>(i + 1);
        worker_config.ring.submit_batch_size = config_.ring.submit_batch_size;
        worker_config.ring.cqe_batch_budget = config_.ring.cqe_batch_budget;
        worker_config.buffer_chunk_size = 256 * 1024;
        worker_config.buffer_max_chunks = 1024;
        worker_config.max_sessions = config_.max_sessions_per_worker;
        worker_config.io_timeout = config_.ring.io_timeout;
        worker_config.drain_timeout = config_.shutdown.drain_timeout;
        worker_config.force_close_timeout = config_.shutdown.force_close_timeout;
        worker_config.extra_session_count = [raw_worker] {
            return raw_worker->live_connectors.load(std::memory_order_relaxed);
        };

        event::WorkerHooks hooks;
        hooks.on_start = [this, raw_worker](event::Worker&) {
            ConfigureWorkerThread(*raw_worker);
        };

        worker->io_worker = std::make_unique<event::Worker>(
            std::move(worker_config), std::move(proxy_factory), std::move(hooks));

        if (!worker->io_worker->Start()) {
            obs::LogError(kLogCategory, "TcpProxyServer: worker {} failed to listen",
                          i);
            continue;
        }

        any_main_listener = true;
        workers_.push_back(std::move(worker));
    }

    if (!any_main_listener || workers_.empty()) {
        running_.store(false, std::memory_order_release);
        Stop();
        obs::LogError(kLogCategory, "TcpProxyServer: failed to start required listeners");
        return;
    }

    const bool tls_enabled = CurrentDownstreamTlsContext() != nullptr;
    if (tls_enabled) {
        obs::LogInfo(kLogCategory,
            "TcpProxyServer: listening on {}:{} -> {} ({} workers, downstream TLS enabled)",
            config_.listen_host, config_.listen_port, upstream_endpoint_->display,
            workers_.size());
        StartMetricsWriter();
        return;
    }

    obs::LogInfo(kLogCategory, "TcpProxyServer: listening on {}:{} -> {} ({} workers)",
                 config_.listen_host, config_.listen_port,
                 upstream_endpoint_->display, workers_.size());
    StartMetricsWriter();
}

void TcpProxyServer::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel) && workers_.empty()) {
        return;
    }
    StopMetricsWriter();
    if (workers_.empty()) {
        std::lock_guard lock(downstream_tls_context_mu_);
        downstream_tls_context_.reset();
        upstream_endpoint_.reset();
        upstream_routes_.clear();
        return;
    }

    StopAccepting();
    CancelConnectors();
    DrainSessions(false);
    if (!WaitForZeroConnections(config_.shutdown.drain_timeout)) {
        obs::LogWarn(kLogCategory,
            "TcpProxyServer: graceful drain timed out after {}ms, forcing close",
            config_.shutdown.drain_timeout.count());
        CancelConnectors();
        DrainSessions(true);
        WaitForZeroConnections(config_.shutdown.force_close_timeout);
    }

    for (auto& worker : workers_) {
        worker->io_worker->Stop();
    }
    workers_.clear();
    {
        std::lock_guard lock(downstream_tls_context_mu_);
        downstream_tls_context_.reset();
    }
    upstream_endpoint_.reset();
    upstream_routes_.clear();
    obs::LogInfo(kLogCategory, "TcpProxyServer: stopped");
}

bool TcpProxyServer::ReloadDownstreamTlsContext() {
    if (!config_.downstream_tls.Enabled()) {
        obs::LogWarn(kLogCategory, "TcpProxyServer: downstream TLS is not configured; reload skipped");
        return false;
    }

    auto tls_context = BuildDownstreamTlsContext();
    if (!tls_context) {
        last_tls_reload_failure_unix_.store(UnixSecondsNow(), std::memory_order_relaxed);
        return false;
    }

    {
        std::lock_guard lock(downstream_tls_context_mu_);
        downstream_tls_context_ = std::move(tls_context);
    }
    last_tls_reload_success_unix_.store(UnixSecondsNow(), std::memory_order_relaxed);
    obs::LogInfo(kLogCategory, "TcpProxyServer: reloaded downstream TLS certificate context");
    return true;
}

void TcpProxyServer::InstallStopSignalHandlers() {
    std::signal(SIGINT, ProxyStopSignalHandler);
    std::signal(SIGTERM, ProxyStopSignalHandler);
    std::signal(SIGHUP, ProxyReloadSignalHandler);
}

void TcpProxyServer::RequestStop() noexcept {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

bool TcpProxyServer::StopRequested() noexcept {
    return g_stop_requested.load(std::memory_order_relaxed);
}

void TcpProxyServer::RequestReload() noexcept {
    g_reload_requested.store(true, std::memory_order_relaxed);
}

bool TcpProxyServer::ReloadRequested() noexcept {
    return g_reload_requested.load(std::memory_order_relaxed);
}

bool TcpProxyServer::ConsumeReloadRequest() noexcept {
    return g_reload_requested.exchange(false, std::memory_order_acq_rel);
}

void TcpProxyServer::WaitForStopSignal(std::chrono::milliseconds poll_interval) {
    if (poll_interval <= std::chrono::milliseconds{0}) {
        poll_interval = std::chrono::milliseconds{100};
    }
    while (!StopRequested()) {
        std::this_thread::sleep_for(poll_interval);
    }
}

void TcpProxyServer::ResetStopRequestedForTests() noexcept {
    g_stop_requested.store(false, std::memory_order_relaxed);
}

void TcpProxyServer::ResetReloadRequestedForTests() noexcept {
    g_reload_requested.store(false, std::memory_order_relaxed);
}

std::unique_ptr<detail::TcpProxyResolvedEndpoint>
TcpProxyServer::ResolveUpstreamEndpoint() const {
    auto endpoint = ResolveEndpoint(config_.upstream_host, config_.upstream_port);
    if (!endpoint) {
        return {};
    }
    return std::make_unique<detail::TcpProxyResolvedEndpoint>(
        std::move(*endpoint));
}

std::vector<detail::TcpProxyResolvedRoute>
TcpProxyServer::ResolveUpstreamRoutes() const {
    std::vector<detail::TcpProxyResolvedRoute> routes;
    routes.reserve(config_.upstream_routes.size());

    for (const auto& route : config_.upstream_routes) {
        const auto normalized_hostname = NormalizeHostname(route.hostname);
        if (normalized_hostname.empty()) {
            obs::LogError(kLogCategory, "TcpProxyServer: upstream route has an empty hostname");
            return {};
        }
        if (route.upstream_port == 0) {
            obs::LogError(kLogCategory, "TcpProxyServer: upstream route {} has port 0",
                          normalized_hostname);
            return {};
        }

        auto endpoint =
            ResolveEndpoint(route.upstream_host, route.upstream_port);
        if (!endpoint) {
            return {};
        }

        routes.push_back(detail::TcpProxyResolvedRoute{
            .hostname = normalized_hostname,
            .endpoint = std::move(*endpoint),
        });
    }

    return routes;
}

const detail::TcpProxyResolvedEndpoint& TcpProxyServer::SelectUpstreamEndpoint(
    std::string_view hostname) const {
    for (const auto& route : upstream_routes_) {
        if (HostnameMatches(route.hostname, hostname)) {
            obs::LogDebug(kLogCategory, "TcpProxyServer: selected route {} -> {}",
                          route.hostname, route.endpoint.display);
            return route.endpoint;
        }
    }
    return *upstream_endpoint_;
}

bool TcpProxyServer::StartConnector(
    event::IoRing& ring,
    core::buffer::BufferPool& pool,
    const detail::TcpProxyResolvedEndpoint& endpoint,
    detail::TcpProxyWorker& worker,
    const std::shared_ptr<detail::ProxyBridge>& bridge) {
    auto connector = std::make_shared<detail::ProxyConnector>(
        ring, pool, endpoint, config_, worker, bridge);
    connector->SetFinishedCallback(
        [&worker](detail::ProxyConnector* connector_ptr) {
            worker.live_connectors.fetch_sub(1, std::memory_order_relaxed);
            std::lock_guard lock(worker.connectors_mu);
            worker.connectors.erase(connector_ptr);
        });
    {
        std::lock_guard lock(worker.connectors_mu);
        worker.connectors.emplace(connector.get(), connector);
    }
    worker.live_connectors.fetch_add(1, std::memory_order_relaxed);

    auto connect_result = connector->Start();
    if (!connect_result) {
        {
            std::lock_guard lock(worker.connectors_mu);
            worker.connectors.erase(connector.get());
        }
        worker.live_connectors.fetch_sub(1, std::memory_order_relaxed);
        bridge->ClosePair();
        return false;
    }

    return true;
}

std::string TcpProxyServer::SnapshotMetricsJson() const {
    std::size_t total_live_sessions = 0;
    std::size_t total_live_connectors = 0;
    const auto now = std::chrono::steady_clock::now();
    const auto uptime_seconds =
        started_at_ == std::chrono::steady_clock::time_point{}
            ? 0
            : std::chrono::duration_cast<std::chrono::seconds>(now - started_at_).count();
    const bool tls_enabled = CurrentDownstreamTlsContext() != nullptr;

    std::ostringstream json;
    json << "{";
    json << "\"service\":\"tcp_reverse_proxy\",";
    json << "\"pid\":" << static_cast<long long>(::getpid()) << ",";
    json << "\"uptime_seconds\":" << uptime_seconds << ",";
    json << "\"listen\":{";
    json << "\"host\":\"" << JsonEscape(config_.listen_host) << "\",";
    json << "\"port\":" << static_cast<unsigned>(config_.listen_port);
    json << "},";
    json << "\"configured_worker_count\":" << static_cast<unsigned>(config_.worker_count) << ",";
    json << "\"running_worker_count\":" << workers_.size() << ",";
    json << "\"default_upstream\":\""
         << JsonEscape(config_.upstream_host + ":" +
                       std::to_string(static_cast<unsigned>(config_.upstream_port)))
         << "\",";
    json << "\"configured_routes\":[";
    for (std::size_t i = 0; i < config_.upstream_routes.size(); ++i) {
        const auto& route = config_.upstream_routes[i];
        if (i != 0) {
            json << ",";
        }
        json << "{";
        json << "\"hostname\":\"" << JsonEscape(route.hostname) << "\",";
        json << "\"upstream\":\""
             << JsonEscape(route.upstream_host + ":" +
                           std::to_string(static_cast<unsigned>(route.upstream_port)))
             << "\"";
        json << "}";
    }
    json << "],";

    json << "\"workers\":[";
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        const auto& worker = *workers_[i];
        const auto live_sessions =
            worker.io_worker ? worker.io_worker->LiveSessions() : 0;
        const auto live_connectors =
            worker.live_connectors.load(std::memory_order_relaxed);
        total_live_sessions += live_sessions;
        total_live_connectors += live_connectors;
        if (i != 0) {
            json << ",";
        }
        json << "{";
        json << "\"index\":" << static_cast<unsigned>(worker.index) << ",";
        json << "\"pinned_cpu\":" << worker.pinned_cpu << ",";
        json << "\"live_sessions\":" << live_sessions << ",";
        json << "\"live_connectors\":" << live_connectors;
        json << "}";
    }
    json << "],";

    json << "\"total_live_sessions\":" << total_live_sessions << ",";
    json << "\"total_live_connectors\":" << total_live_connectors << ",";
    json << "\"tls\":{";
    json << "\"enabled\":" << (config_.downstream_tls.Enabled() ? "true" : "false") << ",";
    json << "\"context_loaded\":" << (tls_enabled ? "true" : "false") << ",";
    json << "\"last_reload_success_unix\":"
         << last_tls_reload_success_unix_.load(std::memory_order_relaxed) << ",";
    json << "\"last_reload_failure_unix\":"
         << last_tls_reload_failure_unix_.load(std::memory_order_relaxed);
    json << "}";
    json << "}";
    return json.str();
}

bool TcpProxyServer::WriteMetricsSnapshot() const {
    return WriteFileAtomically(config_.metrics.file_path, SnapshotMetricsJson());
}

void TcpProxyServer::StartMetricsWriter() {
    if (config_.metrics.file_path.empty()) {
        return;
    }
    if (metrics_writer_running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    (void)WriteMetricsSnapshot();
    auto interval = config_.metrics.interval;
    if (interval <= std::chrono::milliseconds{0}) {
        interval = std::chrono::milliseconds{1000};
    }
    metrics_thread_ = std::thread([this, interval]() {
        while (metrics_writer_running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(interval);
            if (!metrics_writer_running_.load(std::memory_order_relaxed)) {
                break;
            }
            (void)WriteMetricsSnapshot();
        }
    });
}

void TcpProxyServer::StopMetricsWriter() {
    if (!metrics_writer_running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (metrics_thread_.joinable()) {
        metrics_thread_.join();
    }
    if (!config_.metrics.file_path.empty()) {
        (void)WriteMetricsSnapshot();
    }
}

std::shared_ptr<detail::DownstreamTlsContext>
TcpProxyServer::BuildDownstreamTlsContext() const {
    return detail::BuildDownstreamTlsContext(config_.downstream_tls);
}

std::shared_ptr<detail::DownstreamTlsContext>
TcpProxyServer::CurrentDownstreamTlsContext() const {
    std::lock_guard lock(downstream_tls_context_mu_);
    return downstream_tls_context_;
}

} // namespace iouring::stream
