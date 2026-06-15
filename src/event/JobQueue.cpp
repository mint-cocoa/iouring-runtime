#include <iouring/event/JobQueue.h>

#include <thread>
#include <utility>

namespace iouring::event {

JobQueue::JobQueue(GlobalQueue& gq) : gq_(gq) {}

void JobQueue::Push(std::move_only_function<void()> job) {
    std::int32_t prev = job_count_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lk(mutex_);
        jobs_.push_back(std::move(job));
    }
    if (prev == 0)
        gq_.Push(this);
}

void JobQueue::Execute() {
    Execute(std::chrono::steady_clock::time_point::max());
}

void JobQueue::Execute(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        std::vector<std::move_only_function<void()>> batch;
        {
            std::lock_guard lk(mutex_);
            if (!jobs_.empty())
                batch = std::exchange(jobs_, {});
        }

        size_t executed_count = 0;
        for (auto& job : batch) {
            job();
            ++executed_count;
            if (std::chrono::steady_clock::now() >= deadline)
                break;
        }

        // Return unexecuted jobs to the front of the queue
        if (executed_count < batch.size()) {
            std::lock_guard lk(mutex_);
            jobs_.insert(jobs_.begin(),
                std::make_move_iterator(batch.begin() + executed_count),
                std::make_move_iterator(batch.end()));
        }

        auto processed = static_cast<std::int32_t>(executed_count);
        std::int32_t remaining =
            job_count_.fetch_sub(processed, std::memory_order_acq_rel) - processed;

        if (remaining <= 0)
            return;

        if (std::chrono::steady_clock::now() >= deadline) {
            gq_.Push(this);
            return;
        }

        if (executed_count == 0) {
            // A Push() incremented count but hasn't finished push_back yet.
            std::this_thread::yield();
        }
    }
}

} // namespace iouring::event
