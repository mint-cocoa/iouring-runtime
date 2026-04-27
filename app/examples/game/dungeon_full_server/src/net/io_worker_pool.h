#pragma once

#include "io_worker.h"
#include <vector>
#include <memory>

class IoWorkerPool {
public:
    explicit IoWorkerPool(std::uint16_t count);

    void StopAll();

    IoWorker*                         GetWorker(iouring_runtime::core::ContextId id);
    iouring_runtime::core::job::GlobalQueue&     GetGlobalQueue() { return global_queue_; }
    iouring_runtime::core::job::JobTimer&        GetTimer()       { return timer_; }
    std::uint16_t                     Count() const    { return static_cast<std::uint16_t>(workers_.size()); }

private:
    std::vector<std::unique_ptr<IoWorker>> workers_;
    iouring_runtime::core::job::GlobalQueue global_queue_;
    iouring_runtime::core::job::JobTimer timer_;
};
