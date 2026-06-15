#include <iouring/core/SendQueue.h>

#include <iterator>

namespace iouring::core::buffer {

SendQueue::SendQueue(std::uint32_t max_pending)
    : max_pending_(max_pending) {
    pending_.reserve(32);
}

PushResult SendQueue::Push(SendBufferRef buf) {
    std::lock_guard lk(mutex_);
    bool overflow = pending_.size() >= max_pending_;
    if (overflow) {
        ++overflow_count_;
        return {false, true, pending_.size(), pending_bytes_};
    }

    bool first = !registered_;
    pending_bytes_ += buf ? buf->Size() : 0;
    pending_.push_back(std::move(buf));
    if (pending_.size() > peak_depth_) {
        peak_depth_ = pending_.size();
    }
    if (pending_bytes_ > peak_pending_bytes_) {
        peak_pending_bytes_ = pending_bytes_;
    }
    registered_ = true;
    return {first, false, pending_.size(), pending_bytes_};
}

std::vector<SendBufferRef> SendQueue::Drain(std::size_t max_count) {
    std::lock_guard lk(mutex_);
    std::vector<SendBufferRef> out;
    if (max_count == 0 || max_count >= pending_.size()) {
        out.swap(pending_);
        pending_bytes_ = 0;
        return out;
    }

    out.reserve(max_count);
    auto split = pending_.begin() + static_cast<std::ptrdiff_t>(max_count);
    for (auto it = pending_.begin(); it != split; ++it) {
        pending_bytes_ -= *it ? (*it)->Size() : 0;
    }
    std::move(pending_.begin(), split, std::back_inserter(out));
    pending_.erase(pending_.begin(), split);
    return out;
}

std::size_t SendQueue::DrainInto(std::vector<SendBufferRef>& out,
                                 std::size_t max_count) {
    std::lock_guard lk(mutex_);
    out.clear();
    if (pending_.empty()) {
        return 0;
    }

    if (max_count == 0 || max_count >= pending_.size()) {
        out.swap(pending_);
        pending_bytes_ = 0;
        return out.size();
    }

    out.reserve(max_count);
    auto split = pending_.begin() + static_cast<std::ptrdiff_t>(max_count);
    for (auto it = pending_.begin(); it != split; ++it) {
        pending_bytes_ -= *it ? (*it)->Size() : 0;
    }
    std::move(pending_.begin(), split, std::back_inserter(out));
    pending_.erase(pending_.begin(), split);
    return out.size();
}

void SendQueue::MarkSent() {
    std::lock_guard lk(mutex_);
    if (pending_.empty()) {
        registered_ = false;
    }
}

SendQueue::Stats SendQueue::Snapshot() const {
    std::lock_guard lk(mutex_);
    return Stats{
        .current_depth = pending_.size(),
        .pending_bytes = pending_bytes_,
        .peak_depth = peak_depth_,
        .peak_pending_bytes = peak_pending_bytes_,
        .overflow_count = overflow_count_,
    };
}

} // namespace iouring::core::buffer
