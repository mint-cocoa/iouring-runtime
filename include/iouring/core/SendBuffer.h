#pragma once

#include <iouring/core/Error.h>
#include <iouring/core/MpscQueue.h>
#include <iouring/observability/Profiler.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace iouring::core::buffer {

class SendBufferChunk;
class BufferPool;
class SendBuffer;
using SendBufferRef = std::shared_ptr<SendBuffer>;

class SendBuffer {
public:
    std::span<std::byte> Writable();
    void Commit(std::uint32_t size);
    std::span<const std::byte> Data() const;
    std::uint32_t Size() const;

private:
    friend class SendBufferChunk;

    SendBuffer(SendBufferChunk* owner, std::byte* buf, std::uint32_t alloc);

    SendBufferChunk* owner_;
    std::byte* buffer_;
    std::uint32_t alloc_size_;
    std::uint32_t write_size_ = 0;
};

class SendBufferChunk {
public:
    static constexpr std::uint32_t kDefaultChunkSize = 4 * 1024 * 1024;

    explicit SendBufferChunk(BufferPool* pool,
                             std::uint32_t chunk_size = kDefaultChunkSize);

    std::optional<SendBufferRef> Open(std::uint32_t size);
    void Reset() noexcept;
    bool Full() const noexcept;
    bool Unused() const noexcept;

private:
    void OnRelease();  // defined after BufferPool

    BufferPool* pool_;
    std::vector<std::byte> buffer_;
    std::uint32_t offset_ = 0;
    std::atomic<std::uint32_t> ref_count_{0};
};

class BufferPool {
public:
    static constexpr std::uint32_t kMaxFreeChunks = 8;

    explicit BufferPool(std::uint32_t chunk_size = SendBufferChunk::kDefaultChunkSize,
                        std::uint32_t max_chunks = 256);

    std::expected<SendBufferRef, CoreError> Allocate(std::uint32_t size);

private:
    friend class SendBufferChunk;

    void OnChunkUnused(SendBufferChunk* chunk);
    void FlushPendingGc();
    void MoveToFree(SendBufferChunk* chunk);
    std::unique_ptr<SendBufferChunk> AcquireChunk();

    TracyLockable(std::mutex, mutex_);
    MpscQueue<SendBufferChunk*> pending_gc_;
    std::vector<std::unique_ptr<SendBufferChunk>> active_chunks_;
    std::vector<std::unique_ptr<SendBufferChunk>> free_chunks_;
    std::uint32_t chunk_size_;
    std::uint32_t max_chunks_;
};

// Defined out-of-line after BufferPool is complete.

} // namespace iouring::core::buffer
