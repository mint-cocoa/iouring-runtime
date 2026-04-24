#pragma once

#include <iouring_runtime/core/Error.h>
#include <iouring_runtime/core/Profiler.h>
#include <iouring_runtime/core/MpscQueue.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace iouring_runtime::core::buffer {

class SendBufferChunk;
class BufferPool;
class SendBuffer;
using SendBufferRef = std::shared_ptr<SendBuffer>;

class SendBuffer {
public:
    std::span<std::byte> Writable() {
        return {buffer_ + write_size_, alloc_size_ - write_size_};
    }

    void Commit(std::uint32_t size) {
        write_size_ += size;
    }

    std::span<const std::byte> Data() const {
        return {buffer_, write_size_};
    }

    std::uint32_t Size() const { return write_size_; }

private:
    friend class SendBufferChunk;

    SendBuffer(SendBufferChunk* owner, std::byte* buf, std::uint32_t alloc)
        : owner_(owner), buffer_(buf), alloc_size_(alloc) {}

    SendBufferChunk* owner_;
    std::byte* buffer_;
    std::uint32_t alloc_size_;
    std::uint32_t write_size_ = 0;
};

class SendBufferChunk {
public:
    static constexpr std::uint32_t kDefaultChunkSize = 4 * 1024 * 1024;

    explicit SendBufferChunk(BufferPool* pool, std::uint32_t chunk_size = kDefaultChunkSize)
        : pool_(pool), buffer_(chunk_size) {}

    std::optional<SendBufferRef> Open(std::uint32_t size) {
        if (offset_ + size > static_cast<std::uint32_t>(buffer_.size()))
            return std::nullopt;

        auto* buf_ptr = buffer_.data() + offset_;
        offset_ += size;
        ref_count_.fetch_add(1, std::memory_order_relaxed);

        return SendBufferRef(
            new SendBuffer(this, buf_ptr, size),
            [](SendBuffer* sb) {
                auto* chunk = sb->owner_;
                delete sb;
                chunk->OnRelease();
            });
    }

    void Reset() noexcept {
        offset_ = 0;
    }

    bool Full() const noexcept {
        return offset_ >= static_cast<std::uint32_t>(buffer_.size());
    }

    bool Unused() const noexcept {
        return ref_count_.load(std::memory_order_acquire) == 0;
    }

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
                        std::uint32_t max_chunks = 256)
        : chunk_size_(chunk_size), max_chunks_(max_chunks) {}

    std::expected<SendBufferRef, CoreError> Allocate(std::uint32_t size) {
        std::scoped_lock lock(mutex_);
        FlushPendingGc();

        // Try current active chunk
        if (!active_chunks_.empty()) {
            if (auto buf = active_chunks_.back()->Open(size))
                return std::move(*buf);
        }

        // Acquire chunk (reuse or new)
        auto chunk = AcquireChunk();
        if (!chunk)
            return std::unexpected(CoreError::kResourceExhausted);

        active_chunks_.push_back(std::move(chunk));
        auto buf = active_chunks_.back()->Open(size);
        if (!buf)
            return std::unexpected(CoreError::kResourceExhausted);
        return std::move(*buf);
    }

private:
    friend class SendBufferChunk;

    void OnChunkUnused(SendBufferChunk* chunk) {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            pending_gc_.Push(chunk);
            return;
        }
        MoveToFree(chunk);
    }

    void FlushPendingGc() {
        pending_gc_.Drain([this](SendBufferChunk*&& chunk) {
            MoveToFree(chunk);
        });
    }

    void MoveToFree(SendBufferChunk* chunk) {
        auto it = std::find_if(active_chunks_.begin(), active_chunks_.end(),
            [chunk](const auto& p) { return p.get() == chunk; });
        if (it != active_chunks_.end()) {
            (*it)->Reset();
            if (free_chunks_.size() < kMaxFreeChunks) {
                free_chunks_.push_back(std::move(*it));
            }
            active_chunks_.erase(it);
        }
    }

    std::unique_ptr<SendBufferChunk> AcquireChunk() {
        if (!free_chunks_.empty()) {
            auto chunk = std::move(free_chunks_.back());
            free_chunks_.pop_back();
            return chunk;
        }
        if (active_chunks_.size() >= max_chunks_)
            return nullptr;
        return std::make_unique<SendBufferChunk>(this, chunk_size_);
    }

    TracyLockable(std::mutex, mutex_);
    MpscQueue<SendBufferChunk*> pending_gc_;
    std::vector<std::unique_ptr<SendBufferChunk>> active_chunks_;
    std::vector<std::unique_ptr<SendBufferChunk>> free_chunks_;
    std::uint32_t chunk_size_;
    std::uint32_t max_chunks_;
};

// Deferred definition
inline void SendBufferChunk::OnRelease() {
    if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        pool_->OnChunkUnused(this);
    }
}

} // namespace iouring_runtime::core::buffer
