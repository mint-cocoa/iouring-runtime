#pragma once

#include <iouring/core/SendBuffer.h>

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>

namespace iouring::core::buffer {

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

    explicit SendQueue(std::uint32_t max_pending = 4096);

    // Push a send buffer. Returns flags decided atomically under one lock.
    PushResult Push(SendBufferRef buf);

    // Drain pending buffers (called from IO thread after send completes).
    // max_count == 0 drains all buffers.
    std::vector<SendBufferRef> Drain(std::size_t max_count = 0);

    // Drain into caller-owned storage so hot send paths can reuse vector
    // capacity instead of allocating a fresh vector for every send cycle.
    std::size_t DrainInto(std::vector<SendBufferRef>& out,
                          std::size_t max_count = 0);

    // Mark send cycle as complete — next Push will set needs_register again.
    void MarkSent();

    Stats Snapshot() const;

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

} // namespace iouring::core::buffer
