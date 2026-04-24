#include <iouring_runtime/proxy/TcpProxyServer.h>

#include "AcmeChallengeSession.h"
#include "DownstreamTlsContext.h"
#include "ProxyCommon.h"
#include "ProxyConnector.h"
#include "ProxySessions.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cctype>
#include <csignal>
#include <fstream>
#include <netdb.h>
#include <optional>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace iouring_runtime::proxy {

namespace {

std::atomic<bool> g_stop_requested{false};
std::atomic<bool> g_reload_requested{false};

void ProxyStopSignalHandler(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

void ProxyReloadSignalHandler(int) {
    g_reload_requested.store(true, std::memory_order_relaxed);
}

std::vector<int> ExpandCpuToken(const std::string& token) {
    const auto dash = token.find('-');
    if (dash == std::string::npos) {
        return {std::stoi(token)};
    }

    const int first = std::stoi(token.substr(0, dash));
    const int last = std::stoi(token.substr(dash + 1));
    std::vector<int> cpus;
    cpus.reserve(static_cast<std::size_t>(last - first + 1));
    for (int cpu = first; cpu <= last; ++cpu) {
        cpus.push_back(cpu);
    }
    return cpus;
}

std::vector<int> ParseCpuList(const std::string& text) {
    std::vector<int> cpus;
    std::stringstream ss(text);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        auto expanded = ExpandCpuToken(token);
        cpus.insert(cpus.end(), expanded.begin(), expanded.end());
    }
    return cpus;
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::vector<int> OrderedOnlineCpus() {
    auto cpus = ParseCpuList(ReadTextFile("/sys/devices/system/cpu/online"));
    if (!cpus.empty()) {
        return cpus;
    }

    const long cpu_count = ::sysconf(_SC_NPROCESSORS_ONLN);
    cpus.reserve(cpu_count > 0 ? static_cast<std::size_t>(cpu_count) : 0U);
    for (int cpu = 0; cpu < cpu_count; ++cpu) {
        cpus.push_back(cpu);
    }
    return cpus;
}

std::vector<int> OrderedPhysicalFirstCpus() {
    std::vector<int> ordered;
    std::set<int> seen;
    for (int cpu : OrderedOnlineCpus()) {
        const std::string path =
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
            "/topology/thread_siblings_list";
        auto siblings = ParseCpuList(ReadTextFile(path));
        if (siblings.empty()) {
            siblings.push_back(cpu);
        }
        const int primary = siblings.front();
        if (seen.insert(primary).second) {
            ordered.push_back(primary);
        }
    }

    for (int cpu : OrderedOnlineCpus()) {
        if (seen.insert(cpu).second) {
            ordered.push_back(cpu);
        }
    }
    return ordered;
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
        spdlog::error("TcpProxyServer: getaddrinfo({}:{}) failed: {}",
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
        spdlog::error("TcpProxyServer: no usable upstream address for {}:{}",
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
    bool any_challenge_listener = !config_.certbot.Enabled();

    for (std::uint16_t i = 0; i < config_.worker_count; ++i) {
        auto worker = std::make_unique<detail::TcpProxyWorker>();
        auto* raw_worker = worker.get();
        worker->index = i;

        core::ring::IoRingConfig ring_config;
        ring_config.queue_depth = config_.ring.queue_depth;
        ring_config.buf_ring.buf_count = config_.ring.buf_count;
        ring_config.buf_ring.buf_size = config_.ring.buf_size;
        ring_config.buf_ring.group_id = static_cast<std::uint16_t>(i + 1);
        ring_config.submit_batch_size = config_.ring.submit_batch_size;
        ring_config.cqe_batch_budget = config_.ring.cqe_batch_budget;

        auto ring_result = core::ring::IoRing::Create(ring_config);
        if (!ring_result) {
            spdlog::error("TcpProxyServer: failed to create IoRing for worker {}",
                          i);
            continue;
        }
        worker->ring = std::move(*ring_result);

        core::io::SessionFactory proxy_factory =
            [this, raw_worker](int fd, core::ring::IoRing& ring,
                               core::buffer::BufferPool& pool, core::ContextId)
                -> core::io::SessionRef {
            auto bridge = std::make_shared<detail::ProxyBridge>(
                config_.pending_connect_buffer_limit);

            auto tls_context = CurrentDownstreamTlsContext();
            core::io::SessionRef session;
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
                    config_.pending_connect_buffer_limit);
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

        core::Address addr{config_.listen_host, config_.listen_port};
        worker->listener = std::make_shared<core::io::Listener>(
            *worker->ring, worker->pool, addr, std::move(proxy_factory), i,
            config_.max_sessions_per_worker);
        worker->listener->SetSessionCountFn([raw_worker]() {
            return raw_worker->live_sessions.load(std::memory_order_relaxed) +
                   raw_worker->live_connectors.load(std::memory_order_relaxed);
        });

        auto listen_result = worker->listener->Start();
        if (!listen_result) {
            spdlog::error("TcpProxyServer: worker {} failed to listen on {}:{}",
                          i, config_.listen_host, config_.listen_port);
            continue;
        }
        any_main_listener = true;

        if (config_.certbot.Enabled()) {
            core::io::SessionFactory challenge_factory =
                [this, raw_worker](int fd, core::ring::IoRing& ring,
                                   core::buffer::BufferPool& pool, core::ContextId)
                    -> core::io::SessionRef {
                auto session = detail::CreateAcmeChallengeSession(
                    fd, ring, pool, config_.certbot.challenge_webroot,
                    config_.backpressure.send_queue_max_pending);
                detail::ConfigureProxySession(session, *raw_worker, config_);
                return session;
            };

            core::Address challenge_addr{config_.certbot.challenge_host,
                                         config_.certbot.challenge_port};
            worker->challenge_listener = std::make_shared<core::io::Listener>(
                *worker->ring, worker->pool, challenge_addr,
                std::move(challenge_factory), i,
                config_.max_sessions_per_worker);
            worker->challenge_listener->SetSessionCountFn([raw_worker]() {
                return raw_worker->live_sessions.load(std::memory_order_relaxed) +
                       raw_worker->live_connectors.load(std::memory_order_relaxed);
            });

            auto challenge_result = worker->challenge_listener->Start();
            if (!challenge_result) {
                spdlog::error(
                    "TcpProxyServer: worker {} failed to listen on ACME challenge {}:{}",
                    i, config_.certbot.challenge_host, config_.certbot.challenge_port);
            } else {
                any_challenge_listener = true;
            }
        }

        worker->thread = std::thread([this, raw_worker]() {
            WorkerLoop(*raw_worker);
        });
        workers_.push_back(std::move(worker));
    }

    if (!any_main_listener || !any_challenge_listener || workers_.empty()) {
        running_.store(false, std::memory_order_release);
        Stop();
        spdlog::error("TcpProxyServer: failed to start required listeners");
        return;
    }

    const bool tls_enabled = CurrentDownstreamTlsContext() != nullptr;
    if (tls_enabled && config_.certbot.Enabled()) {
        spdlog::info(
            "TcpProxyServer: listening on {}:{} -> {} ({} workers, downstream TLS enabled, ACME challenge on {}:{})",
            config_.listen_host, config_.listen_port, upstream_endpoint_->display,
            workers_.size(), config_.certbot.challenge_host,
            config_.certbot.challenge_port);
        return;
    }

    if (tls_enabled) {
        spdlog::info(
            "TcpProxyServer: listening on {}:{} -> {} ({} workers, downstream TLS enabled)",
            config_.listen_host, config_.listen_port, upstream_endpoint_->display,
            workers_.size());
        return;
    }

    if (config_.certbot.Enabled()) {
        spdlog::info(
            "TcpProxyServer: listening on {}:{} -> {} ({} workers, ACME challenge on {}:{})",
            config_.listen_host, config_.listen_port, upstream_endpoint_->display,
            workers_.size(), config_.certbot.challenge_host,
            config_.certbot.challenge_port);
        return;
    }

    spdlog::info("TcpProxyServer: listening on {}:{} -> {} ({} workers)",
                 config_.listen_host, config_.listen_port,
                 upstream_endpoint_->display, workers_.size());
}

void TcpProxyServer::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel) && workers_.empty()) {
        return;
    }
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
        spdlog::warn(
            "TcpProxyServer: graceful drain timed out after {}ms, forcing close",
            config_.shutdown.drain_timeout.count());
        CancelConnectors();
        DrainSessions(true);
        WaitForZeroConnections(config_.shutdown.force_close_timeout);
    }

    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    workers_.clear();
    {
        std::lock_guard lock(downstream_tls_context_mu_);
        downstream_tls_context_.reset();
    }
    upstream_endpoint_.reset();
    upstream_routes_.clear();
    spdlog::info("TcpProxyServer: stopped");
}

bool TcpProxyServer::ReloadDownstreamTlsContext() {
    if (!config_.downstream_tls.Enabled()) {
        spdlog::warn("TcpProxyServer: downstream TLS is not configured; reload skipped");
        return false;
    }

    auto tls_context = BuildDownstreamTlsContext();
    if (!tls_context) {
        return false;
    }

    {
        std::lock_guard lock(downstream_tls_context_mu_);
        downstream_tls_context_ = std::move(tls_context);
    }
    spdlog::info("TcpProxyServer: reloaded downstream TLS certificate context");
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
            spdlog::error("TcpProxyServer: upstream route has an empty hostname");
            return {};
        }
        if (route.upstream_port == 0) {
            spdlog::error("TcpProxyServer: upstream route {} has port 0",
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
            spdlog::debug("TcpProxyServer: selected route {} -> {}",
                          route.hostname, route.endpoint.display);
            return route.endpoint;
        }
    }
    return *upstream_endpoint_;
}

bool TcpProxyServer::StartConnector(
    core::ring::IoRing& ring,
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

std::shared_ptr<detail::DownstreamTlsContext>
TcpProxyServer::BuildDownstreamTlsContext() const {
    return detail::BuildDownstreamTlsContext(config_.downstream_tls);
}

std::shared_ptr<detail::DownstreamTlsContext>
TcpProxyServer::CurrentDownstreamTlsContext() const {
    std::lock_guard lock(downstream_tls_context_mu_);
    return downstream_tls_context_;
}

void TcpProxyServer::StopAccepting() {
    for (auto& worker : workers_) {
        worker->ring->Post([listener = worker->listener,
                            challenge_listener = worker->challenge_listener] {
            if (listener) {
                listener->Stop();
            }
            if (challenge_listener) {
                challenge_listener->Stop();
            }
        });
    }
}

void TcpProxyServer::CancelConnectors() {
    for (auto& worker : workers_) {
        std::vector<std::shared_ptr<detail::ProxyConnector>> connectors;
        {
            std::lock_guard lock(worker->connectors_mu);
            connectors.reserve(worker->connectors.size());
            for (const auto& [_, connector] : worker->connectors) {
                connectors.push_back(connector);
            }
        }

        if (connectors.empty()) {
            continue;
        }

        worker->ring->Post([connectors = std::move(connectors)]() mutable {
            for (auto& connector : connectors) {
                if (connector) {
                    connector->Cancel();
                }
            }
        });
    }
}

void TcpProxyServer::DrainSessions(bool force_close) {
    for (auto& worker : workers_) {
        std::vector<core::io::SessionRef> sessions;
        {
            std::lock_guard lock(worker->sessions_mu);
            sessions.reserve(worker->sessions.size());
            for (const auto& [_, session] : worker->sessions) {
                sessions.push_back(session);
            }
        }

        if (sessions.empty()) {
            continue;
        }

        worker->ring->Post([sessions = std::move(sessions), force_close]() mutable {
            for (auto& session : sessions) {
                if (!session || session->Disconnecting()) {
                    continue;
                }
                if (force_close) {
                    session->Disconnect();
                } else {
                    session->DisconnectAfterFlush();
                }
            }
        });
    }
}

bool TcpProxyServer::WaitForZeroConnections(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        bool all_zero = true;
        for (const auto& worker : workers_) {
            if (worker->live_sessions.load(std::memory_order_relaxed) != 0 ||
                worker->live_connectors.load(std::memory_order_relaxed) != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

void TcpProxyServer::ConfigureWorkerAffinity(detail::TcpProxyWorker& worker) {
    if (config_.worker_affinity == TcpProxyConfig::WorkerAffinityMode::kOff) {
        return;
    }

    const auto cpus =
        config_.worker_affinity == TcpProxyConfig::WorkerAffinityMode::kPhysicalCores
            ? OrderedPhysicalFirstCpus()
            : OrderedOnlineCpus();
    if (cpus.empty()) {
        return;
    }

    worker.pinned_cpu = cpus[worker.index % cpus.size()];

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(worker.pinned_cpu, &set);
    const auto native = pthread_self();
    if (::pthread_setaffinity_np(native, sizeof(set), &set) != 0) {
        spdlog::warn("TcpProxyServer: failed to pin worker {} to cpu {}",
                     worker.index, worker.pinned_cpu);
        worker.pinned_cpu = -1;
        return;
    }

    spdlog::info("TcpProxyServer: pinned worker {} to cpu {}",
                 worker.index, worker.pinned_cpu);
}

void TcpProxyServer::WorkerLoop(detail::TcpProxyWorker& worker) {
    ConfigureWorkerAffinity(worker);
    core::ring::IoRing::SetCurrent(worker.ring.get());
    while (running_.load(std::memory_order_relaxed)) {
        worker.ring->Dispatch(config_.ring.io_timeout);
        worker.ring->ProcessPostedTasks();
    }

    worker.ring->ProcessPostedTasks();
    for (int i = 0; i < 8; ++i) {
        worker.ring->Dispatch(std::chrono::milliseconds{0});
    }
}

} // namespace iouring_runtime::proxy
