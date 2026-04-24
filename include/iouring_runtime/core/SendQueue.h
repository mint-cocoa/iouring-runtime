#pragma once

#include <iouring_runtime/core/SendBuffer.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <vector>

namespace iouring_runtime::core::buffer {

struct PushResult {
    bool needs_register;  // First push since last drain → caller must register send
    bool overflowed;      // Queue capacity exceeded
    std::size_t current_depth;
};

class SendQueue {
public:
    struct Stats {
        std::size_t current_depth = 0;
        std::size_t peak_depth = 0;
        std::uint64_t overflow_count = 0;
    };

    explicit SendQueue(std::uint32_t max_pending = 4096)
        : max_pending_(max_pending) {
        pending_.reserve(32);
    }

    // Push a send buffer. Returns flags decided atomically under one lock.
    PushResult Push(SendBufferRef buf) {
        std::lock_guard lk(mutex_);
        bool overflow = pending_.size() >= max_pending_;
        if (overflow) {
            ++overflow_count_;
            return {false, true, pending_.size()};
        }

        bool first = !registered_;
        pending_.push_back(std::move(buf));
        if (pending_.size() > peak_depth_) {
            peak_depth_ = pending_.size();
        }
        registered_ = true;
        return {first, false, pending_.size()};
    }

    // Drain pending buffers (called from IO thread after send completes).
    // max_count == 0 drains all buffers.
    std::vector<SendBufferRef> Drain(std::size_t max_count = 0) {
        std::lock_guard lk(mutex_);
        std::vector<SendBufferRef> out;
        if (max_count == 0 || max_count >= pending_.size()) {
            out.swap(pending_);
            return out;
        }

        out.reserve(max_count);
        auto split = pending_.begin() + static_cast<std::ptrdiff_t>(max_count);
        std::move(pending_.begin(), split, std::back_inserter(out));
        pending_.erase(pending_.begin(), split);
        return out;
    }

    // Mark send cycle as complete — next Push will set needs_register again.
    void MarkSent() {
        std::lock_guard lk(mutex_);
        if (pending_.empty())
            registered_ = false;
    }

    Stats Snapshot() const {
        std::lock_guard lk(mutex_);
        return Stats{
            .current_depth = pending_.size(),
            .peak_depth = peak_depth_,
            .overflow_count = overflow_count_,
        };
    }

private:
    mutable std::mutex mutex_;
    std::vector<SendBufferRef> pending_;
    bool registered_ = false;
    std::uint32_t max_pending_;
    std::size_t peak_depth_ = 0;
    std::uint64_t overflow_count_ = 0;
};

} // namespace iouring_runtime::core::buffer
