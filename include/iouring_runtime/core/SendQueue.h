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
    std::size_t current_bytes;
};

class SendQueue {
public:
    struct Stats {
        std::size_t current_depth = 0;
        std::size_t pending_bytes = 0;
        std::size_t peak_depth = 0;
        std::size_t peak_pending_bytes = 0;
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

    // Drain pending buffers (called from IO thread after send completes).
    // max_count == 0 drains all buffers.
    std::vector<SendBufferRef> Drain(std::size_t max_count = 0) {
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

    // Drain into caller-owned storage so hot send paths can reuse vector
    // capacity instead of allocating a fresh vector for every send cycle.
    std::size_t DrainInto(std::vector<SendBufferRef>& out,
                          std::size_t max_count = 0) {
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
            .pending_bytes = pending_bytes_,
            .peak_depth = peak_depth_,
            .peak_pending_bytes = peak_pending_bytes_,
            .overflow_count = overflow_count_,
        };
    }

private:
    mutable std::mutex mutex_;
    std::vector<SendBufferRef> pending_;
    bool registered_ = false;
    std::uint32_t max_pending_;
    std::size_t pending_bytes_ = 0;
    std::size_t peak_depth_ = 0;
    std::size_t peak_pending_bytes_ = 0;
    std::uint64_t overflow_count_ = 0;
};

} // namespace iouring_runtime::core::buffer
