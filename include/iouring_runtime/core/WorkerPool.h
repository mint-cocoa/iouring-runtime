#pragma once

#include <iouring_runtime/core/Worker.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace iouring_runtime::core::io {

using WorkerSessionFactoryBuilder =
    std::move_only_function<SessionFactory(ContextId worker_id)>;

struct WorkerPoolConfig {
    Address address{};
    ring::IoRingConfig ring{};
    std::uint16_t worker_count{1};
    std::uint32_t max_sessions_per_worker{0};
    std::chrono::milliseconds io_timeout{1};
    std::chrono::milliseconds drain_timeout{1000};
    std::chrono::milliseconds force_close_timeout{200};
};

class WorkerPool {
public:
    WorkerPool(WorkerPoolConfig config, WorkerSessionFactoryBuilder factory_builder,
               WorkerHooks hooks = {});
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    bool Start();
    void Stop();

    Worker* GetWorker(ContextId id) noexcept;
    const Worker* GetWorker(ContextId id) const noexcept;
    std::uint16_t Count() const noexcept {
        return static_cast<std::uint16_t>(workers_.size());
    }

private:
    WorkerPoolConfig config_;
    WorkerSessionFactoryBuilder factory_builder_;
    WorkerHooks hooks_;
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace iouring_runtime::core::io
