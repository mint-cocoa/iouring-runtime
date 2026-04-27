#include "io_worker_pool.h"
#include <spdlog/spdlog.h>

IoWorkerPool::IoWorkerPool(std::uint16_t count) {
    workers_.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
        workers_.push_back(std::make_unique<IoWorker>(i, global_queue_, timer_));
}

void IoWorkerPool::StopAll() {
    for (auto& w : workers_)
        w->Stop();
    spdlog::info("IoWorkerPool: all workers stopped");
}

IoWorker* IoWorkerPool::GetWorker(iouring_runtime::core::ContextId id) {
    if (id < workers_.size())
        return workers_[id].get();
    return nullptr;
}
