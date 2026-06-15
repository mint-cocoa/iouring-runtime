#pragma once

#include <iouring/event/JobQueue.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

namespace iouring::event {

struct ReservedJob {
    std::chrono::steady_clock::time_point execute_at;
    std::weak_ptr<JobQueue> target;
    std::move_only_function<void()> job;

    bool operator>(const ReservedJob& other) const {
        return execute_at > other.execute_at;
    }
};

class JobTimer {
public:
    void Reserve(std::chrono::milliseconds delay, std::weak_ptr<JobQueue> target,
                 std::move_only_function<void()> job);

    // Move expired jobs to their target JobQueues.
    void DistributeExpired();

private:
    std::mutex mutex_;
    std::priority_queue<ReservedJob, std::vector<ReservedJob>,
                        std::greater<ReservedJob>> queue_;
};

} // namespace iouring::event
