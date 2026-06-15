#include <iouring/event/JobTimer.h>

namespace iouring::event {

void JobTimer::Reserve(std::chrono::milliseconds delay, std::weak_ptr<JobQueue> target,
                       std::move_only_function<void()> job) {
    auto execute_at = std::chrono::steady_clock::now() + delay;
    std::lock_guard lk(mutex_);
    queue_.push(ReservedJob{
        .execute_at = execute_at,
        .target = std::move(target),
        .job = std::move(job),
    });
}

void JobTimer::DistributeExpired() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lk(mutex_);
    while (!queue_.empty() && queue_.top().execute_at <= now) {
        auto rj = std::move(const_cast<ReservedJob&>(queue_.top()));
        queue_.pop();
        if (auto owner = rj.target.lock()) {
            owner->Push(std::move(rj.job));
        }
    }
}

} // namespace iouring::event
