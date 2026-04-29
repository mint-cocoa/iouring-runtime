#include <iouring_runtime/proxy/TcpProxyServer.h>

#include "common/CpuAffinity.h"
#include "ProxyCommon.h"
#include "ProxyConnector.h"

#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/observability/Profiler.h>

#include <pthread.h>
#include <sched.h>

#include <cstdio>
#include <thread>
#include <vector>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kProxy;
}

namespace iouring_runtime::proxy {

void TcpProxyServer::StopAccepting() {
    for (auto& worker : workers_) {
        worker->io_worker->StopAccepting();
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

        worker->io_worker->Post([connectors = std::move(connectors)]() mutable {
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
        worker->io_worker->DrainSessions(force_close);
    }
}

bool TcpProxyServer::WaitForZeroConnections(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        bool all_zero = true;
        for (const auto& worker : workers_) {
            if (worker->io_worker->LiveSessions() != 0 ||
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
            ? ::iouring_runtime::detail::OrderedPhysicalFirstCpus()
            : ::iouring_runtime::detail::OrderedOnlineCpus();
    if (cpus.empty()) {
        return;
    }

    worker.pinned_cpu = cpus[worker.index % cpus.size()];

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(worker.pinned_cpu, &set);
    const auto native = pthread_self();
    if (::pthread_setaffinity_np(native, sizeof(set), &set) != 0) {
        obs::LogWarn(kLogCategory, "TcpProxyServer: failed to pin worker {} to cpu {}",
                     worker.index, worker.pinned_cpu);
        worker.pinned_cpu = -1;
        return;
    }

    obs::LogInfo(kLogCategory, "TcpProxyServer: pinned worker {} to cpu {}",
                 worker.index, worker.pinned_cpu);
}

void TcpProxyServer::ConfigureWorkerThread(detail::TcpProxyWorker& worker) {
    char tracy_thread_name[32] = {};
    std::snprintf(tracy_thread_name, sizeof(tracy_thread_name),
                  "proxy-worker-%u", static_cast<unsigned>(worker.index));
    TracyCSetThreadName(tracy_thread_name);

    ConfigureWorkerAffinity(worker);
}

} // namespace iouring_runtime::proxy
