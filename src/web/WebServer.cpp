#include <iouring_runtime/web/WebServer.h>

#include <iouring_runtime/web/HttpSession.h>

#include <spdlog/spdlog.h>

#include <csignal>
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <vector>

namespace iouring_runtime::web {

namespace {

std::atomic<bool> g_stop_requested{false};

void WebServerSignalHandler(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
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

} // namespace

WebServer::WebServer(const WebServerConfig& config)
    : config_(config) {}

WebServer::~WebServer() {
    Stop();
}

void WebServer::Route(HttpMethod method, std::string path, HttpHandler handler) {
    router_.Route(method, std::move(path), std::move(handler));
}

void WebServer::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const Router* router = &router_;
    for (std::uint16_t i = 0; i < config_.worker_count; ++i) {
        auto worker = std::make_unique<Worker>();
        Worker* raw_worker = worker.get();
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
            spdlog::error("WebServer: failed to create IoRing for worker {}", i);
            continue;
        }
        worker->ring = std::move(*ring_result);

        const auto parser_options = config_.parser;
        const auto timeouts = config_.timeouts;
        const auto backpressure = config_.backpressure;
        core::io::SessionFactory factory =
            [router, parser_options, timeouts, backpressure, raw_worker](
                int fd, core::ring::IoRing& ring,
                core::buffer::BufferPool& pool, core::ContextId)
                -> core::io::SessionRef {
            auto session = std::make_shared<HttpSession>(
                fd, ring, pool, *router, backpressure.send_queue_max_pending,
                parser_options, timeouts.request);
            session->SetSessionId(raw_worker->next_session_id.fetch_add(
                1, std::memory_order_relaxed));
            session->SetInactivityTimeout(timeouts.inactivity);
            session->SetBackpressureWatermarks(
                backpressure.send_queue_high_watermark,
                backpressure.send_queue_low_watermark);
            session->SetDisconnectOnHighWatermark(
                backpressure.disconnect_on_high_watermark);
            session->SetConnectedCallback([raw_worker](core::io::SessionRef session_ref) {
                raw_worker->live_sessions.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard lock(raw_worker->sessions_mu);
                raw_worker->sessions.emplace(session_ref.get(), std::move(session_ref));
            });
            session->SetDisconnectCallback(
                [raw_worker](core::io::SessionRef session_ref) {
                    raw_worker->live_sessions.fetch_sub(1, std::memory_order_relaxed);
                    std::lock_guard lock(raw_worker->sessions_mu);
                    raw_worker->sessions.erase(session_ref.get());
                });
            return session;
        };

        core::Address addr{config_.host, config_.port};
        worker->listener = std::make_shared<core::io::Listener>(
            *worker->ring, worker->pool, addr, std::move(factory), i,
            config_.max_sessions_per_worker);
        worker->listener->SetSessionCountFn([raw_worker]() {
            return raw_worker->live_sessions.load(std::memory_order_relaxed);
        });

        auto listen_result = worker->listener->Start();
        if (!listen_result) {
            spdlog::error("WebServer: worker {} failed to listen on {}:{}",
                          i, config_.host, config_.port);
            continue;
        }

        worker->thread = std::thread([this, raw_worker]() {
            WorkerLoop(*raw_worker);
        });
        workers_.push_back(std::move(worker));
    }

    if (workers_.empty()) {
        running_.store(false, std::memory_order_release);
        spdlog::error("WebServer: failed to start any workers");
        return;
    }

    spdlog::info("WebServer: listening on {}:{} ({} workers)",
                 config_.host, config_.port, workers_.size());
}

void WebServer::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel) && workers_.empty()) {
        return;
    }
    if (workers_.empty()) {
        return;
    }

    StopAccepting();
    DrainSessions(false);
    if (!WaitForZeroSessions(config_.shutdown.drain_timeout)) {
        spdlog::warn(
            "WebServer: graceful drain timed out after {}ms, forcing close",
            config_.shutdown.drain_timeout.count());
        DrainSessions(true);
        WaitForZeroSessions(config_.shutdown.force_close_timeout);
    }

    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    workers_.clear();
    spdlog::info("WebServer: stopped");
}

void WebServer::InstallStopSignalHandlers() {
    std::signal(SIGINT, WebServerSignalHandler);
    std::signal(SIGTERM, WebServerSignalHandler);
}

void WebServer::RequestStop() noexcept {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

bool WebServer::StopRequested() noexcept {
    return g_stop_requested.load(std::memory_order_relaxed);
}

void WebServer::WaitForStopSignal(std::chrono::milliseconds poll_interval) {
    if (poll_interval <= std::chrono::milliseconds{0}) {
        poll_interval = std::chrono::milliseconds{100};
    }
    while (!StopRequested()) {
        std::this_thread::sleep_for(poll_interval);
    }
}

void WebServer::ResetStopRequestedForTests() noexcept {
    g_stop_requested.store(false, std::memory_order_relaxed);
}

void WebServer::StopAccepting() {
    for (auto& worker : workers_) {
        worker->ring->Post([listener = worker->listener] {
            if (listener) {
                listener->Stop();
            }
        });
    }
}

void WebServer::DrainSessions(bool force_close) {
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

bool WebServer::WaitForZeroSessions(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        bool all_zero = true;
        for (const auto& worker : workers_) {
            if (worker->live_sessions.load(std::memory_order_relaxed) != 0) {
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

void WebServer::ConfigureWorkerAffinity(Worker& worker) {
    if (config_.worker_affinity == WebServerConfig::WorkerAffinityMode::kOff) {
        return;
    }

    const auto cpus =
        config_.worker_affinity == WebServerConfig::WorkerAffinityMode::kPhysicalCores
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
        spdlog::warn("WebServer: failed to pin worker {} to cpu {}",
                     worker.index, worker.pinned_cpu);
        worker.pinned_cpu = -1;
        return;
    }

    spdlog::info("WebServer: pinned worker {} to cpu {}",
                 worker.index, worker.pinned_cpu);
}

void WebServer::WorkerLoop(Worker& worker) {
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

} // namespace iouring_runtime::web
