#include <iouring_runtime/web/WebServer.h>

#include "common/CpuAffinity.h"

#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/observability/Profiler.h>

#include <pthread.h>
#include <sched.h>

#include <cstdio>
#include <thread>
#include <vector>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kWeb;
}

namespace iouring_runtime::web {

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
            ? detail::OrderedPhysicalFirstCpus()
            : detail::OrderedOnlineCpus();
    if (cpus.empty()) {
        return;
    }

    worker.pinned_cpu = cpus[worker.index % cpus.size()];

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(worker.pinned_cpu, &set);
    const auto native = pthread_self();
    if (::pthread_setaffinity_np(native, sizeof(set), &set) != 0) {
        obs::LogWarn(kLogCategory, "WebServer: failed to pin worker {} to cpu {}",
                     worker.index, worker.pinned_cpu);
        worker.pinned_cpu = -1;
        return;
    }

    obs::LogInfo(kLogCategory, "WebServer: pinned worker {} to cpu {}",
                 worker.index, worker.pinned_cpu);
}

void WebServer::WorkerLoop(Worker& worker) {
    char tracy_thread_name[32] = {};
    std::snprintf(tracy_thread_name, sizeof(tracy_thread_name),
                  "web-worker-%u", static_cast<unsigned>(worker.index));
    TracyCSetThreadName(tracy_thread_name);

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
