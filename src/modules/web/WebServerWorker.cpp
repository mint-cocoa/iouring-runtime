#include <iouring_runtime/web/WebServer.h>

#include "common/CpuAffinity.h"

#include <iouring_runtime/observability/Logging.h>
#include <iouring_runtime/observability/Profiler.h>

#include <pthread.h>
#include <sched.h>

#include <cstdio>
#include <thread>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kWeb;
}

namespace iouring_runtime::web {

void WebServer::StopAccepting() {
    for (auto& worker : workers_) {
        worker->io_worker->StopAccepting();
    }
}

void WebServer::DrainSessions(bool force_close) {
    for (auto& worker : workers_) {
        worker->io_worker->DrainSessions(force_close);
    }
}

bool WebServer::WaitForZeroSessions(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        bool all_zero = true;
        for (const auto& worker : workers_) {
            if (worker->io_worker->LiveSessions() != 0) {
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

void WebServer::ConfigureWorkerThread(Worker& worker) {
    char tracy_thread_name[32] = {};
    std::snprintf(tracy_thread_name, sizeof(tracy_thread_name),
                  "web-worker-%u", static_cast<unsigned>(worker.index));
    TracyCSetThreadName(tracy_thread_name);

    ConfigureWorkerAffinity(worker);
}

} // namespace iouring_runtime::web
