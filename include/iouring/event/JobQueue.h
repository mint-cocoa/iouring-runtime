#pragma once

#include <iouring/event/GlobalQueue.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace iouring::event {

/// Serialized job queue with atomic batch processing.
///
/// Push() is thread-safe (any thread). Execute() is called by reactors.
/// Guarantees:
///   - Only one thread executes jobs at any time (serialization).
///   - Batch draining: all queued jobs are processed in one sweep.
///   - Time-budgeted: Execute() yields to GlobalQueue when deadline exceeded.
class JobQueue : public std::enable_shared_from_this<JobQueue> {
public:
    explicit JobQueue(GlobalQueue& gq);
    virtual ~JobQueue() = default;

    /// Push a job. If count transitions 0→1, registers with GlobalQueue.
    void Push(std::move_only_function<void()> job);

    /// Process all queued jobs (no time limit). Useful for testing.
    void Execute();

    /// Process queued jobs until deadline. If more remain, re-registers with GlobalQueue.
    void Execute(std::chrono::steady_clock::time_point deadline);

private:
    GlobalQueue& gq_;
    std::mutex mutex_;
    std::vector<std::move_only_function<void()>> jobs_;
    std::atomic<std::int32_t> job_count_ = 0;
};

} // namespace iouring::event
