#pragma once

#include <iouring_runtime/core/MmapGuard.h>
#include <iouring_runtime/core/Profiler.h>
#include <iouring_runtime/core/Error.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <span>

struct io_uring;
struct io_uring_buf_ring;

namespace iouring_runtime::core::ring {

struct RingBufferConfig {
    std::uint32_t buf_count{1024};  // Must be power-of-two
    std::uint32_t buf_size{8192};   // Must be power-of-two
    std::uint16_t group_id{1};
};

// RAII wrapper for io_uring provided buffer ring.
// Single mmap region: [buf_ring metadata | buffer data]
class RingBuffer {
public:
    static std::expected<std::unique_ptr<RingBuffer>, RingError> Create(io_uring* ring,
                                                      const RingBufferConfig& cfg);
    ~RingBuffer();

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // View buffer contents by id (valid until Return)
    std::span<const std::byte> View(std::uint16_t buf_id, std::uint32_t len) const noexcept;

    // Return buffer to the ring for reuse
    void Return(std::uint16_t buf_id) noexcept;

    std::uint16_t GroupId() const noexcept { return cfg_.group_id; }
    std::uint32_t BufSize() const noexcept { return cfg_.buf_size; }
    std::uint32_t BufCount() const noexcept { return cfg_.buf_count; }
    std::uint32_t Available() const noexcept {
        return available_.load(std::memory_order_relaxed);
    }

private:
    RingBuffer(io_uring* ring, const RingBufferConfig& cfg,
               MmapGuard mmap, io_uring_buf_ring* br, unsigned shift);

    std::uint8_t* buf_addr(std::uint16_t buf_id) const noexcept;

    io_uring* ring_;
    RingBufferConfig cfg_;
    MmapGuard mmap_;
    io_uring_buf_ring* ring_ptr_;
    unsigned buf_size_shift_;
    std::uint8_t* data_base_;

    TracyLockable(std::mutex, mtx_);  // protects buf_ring_add/advance
    std::atomic<std::uint32_t> available_;
};

} // namespace iouring_runtime::core::ring
