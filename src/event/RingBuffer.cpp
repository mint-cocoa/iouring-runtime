#include <iouring/event/RingBuffer.h>

#include <liburing.h>
#include <iouring/observability/Logging.h>

#include <algorithm>
#include <bit>
#include <cstring>

#include <sys/mman.h>

namespace obs = iouring::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kRing;
}

namespace iouring::event {

namespace {

bool is_power_of_two(std::uint32_t v) {
    return v > 0 && (v & (v - 1)) == 0;
}

unsigned log2u(std::uint32_t v) {
    return static_cast<unsigned>(std::bit_width(v) - 1);
}

} // namespace

std::expected<std::unique_ptr<RingBuffer>, RingError> RingBuffer::Create(io_uring* ring,
                                                        const RingBufferConfig& cfg) {
    if (!ring)
        return std::unexpected(RingError::kBufferRegistrationFailed);

    if (!is_power_of_two(cfg.buf_count) || !is_power_of_two(cfg.buf_size))
        return std::unexpected(RingError::kBufferRegistrationFailed);

    // Single mmap: [io_uring_buf * buf_count] + [buf_size * buf_count]
    const std::size_t meta_size = sizeof(io_uring_buf) * cfg.buf_count;
    const std::size_t data_size = static_cast<std::size_t>(cfg.buf_size) * cfg.buf_count;
    const std::size_t total = meta_size + data_size;

    MmapGuard mmap(::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
                   total);
    if (!mmap)
        return std::unexpected(RingError::kBufferRegistrationFailed);

    auto* br = static_cast<io_uring_buf_ring*>(mmap.Get());
    io_uring_buf_ring_init(br);

    io_uring_buf_reg reg{};
    reg.ring_addr = reinterpret_cast<std::uint64_t>(mmap.Get());
    reg.ring_entries = cfg.buf_count;
    reg.bgid = cfg.group_id;

    if (io_uring_register_buf_ring(ring, &reg, 0) < 0)
        return std::unexpected(RingError::kBufferRegistrationFailed);

    // Initialize all buffers into the ring
    const unsigned shift = log2u(cfg.buf_size);
    const unsigned mask = io_uring_buf_ring_mask(cfg.buf_count);
    auto* data_base = static_cast<std::uint8_t*>(mmap.Get()) + meta_size;

    for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(cfg.buf_count); ++i) {
        std::uint8_t* addr = data_base + (static_cast<std::size_t>(i) << shift);
        io_uring_buf_ring_add(br, addr, cfg.buf_size, i, mask, i);
    }
    io_uring_buf_ring_advance(br, static_cast<int>(cfg.buf_count));

    return std::unique_ptr<RingBuffer>(
        new RingBuffer(ring, cfg, std::move(mmap), br, shift));
}

RingBuffer::RingBuffer(io_uring* ring, const RingBufferConfig& cfg,
                       MmapGuard mmap, io_uring_buf_ring* br, unsigned shift)
    : ring_(ring)
    , cfg_(cfg)
    , mmap_(std::move(mmap))
    , ring_ptr_(br)
    , buf_size_shift_(shift)
    , data_base_(static_cast<std::uint8_t*>(mmap_.Get()) +
                 sizeof(io_uring_buf) * cfg.buf_count)
    , available_(cfg.buf_count) {}

RingBuffer::~RingBuffer() {
    if (ring_ && ring_ptr_) {
        io_uring_unregister_buf_ring(ring_, cfg_.group_id);
    }
    // mmap_ cleaned up by RAII
}

std::uint8_t* RingBuffer::buf_addr(std::uint16_t buf_id) const noexcept {
    return data_base_ + (static_cast<std::size_t>(buf_id) << buf_size_shift_);
}

std::span<const std::byte> RingBuffer::View(std::uint16_t buf_id,
                                             std::uint32_t len) const noexcept {
    if (buf_id >= cfg_.buf_count) {
        obs::LogError(kLogCategory, "RingBuffer::View: buf_id {} out of range (max {})", buf_id, cfg_.buf_count);
        return {};
    }
    const auto* ptr = reinterpret_cast<const std::byte*>(buf_addr(buf_id));
    return {ptr, std::min(len, cfg_.buf_size)};
}

void RingBuffer::Return(std::uint16_t buf_id) noexcept {
    if (buf_id >= cfg_.buf_count) {
        obs::LogError(kLogCategory, "RingBuffer::Return: buf_id {} out of range (max {})", buf_id, cfg_.buf_count);
        return;
    }
    std::uint8_t* addr = buf_addr(buf_id);
    {
        std::lock_guard lk(mtx_);
        LockMark(mtx_);
        unsigned mask = io_uring_buf_ring_mask(cfg_.buf_count);
        io_uring_buf_ring_add(ring_ptr_, addr, cfg_.buf_size, buf_id, mask, 0);
        io_uring_buf_ring_advance(ring_ptr_, 1);
    }
    available_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace iouring::event
