#include <iouring/core/SendBuffer.h>

#include <algorithm>

namespace iouring::core::buffer {

std::span<std::byte> SendBuffer::Writable() {
    return {buffer_ + write_size_, alloc_size_ - write_size_};
}

void SendBuffer::Commit(std::uint32_t size) {
    write_size_ += size;
}

std::span<const std::byte> SendBuffer::Data() const {
    return {buffer_, write_size_};
}

std::uint32_t SendBuffer::Size() const {
    return write_size_;
}

SendBuffer::SendBuffer(SendBufferChunk* owner, std::byte* buf, std::uint32_t alloc)
    : owner_(owner)
    , buffer_(buf)
    , alloc_size_(alloc) {}

SendBufferChunk::SendBufferChunk(BufferPool* pool, std::uint32_t chunk_size)
    : pool_(pool)
    , buffer_(chunk_size) {}

std::optional<SendBufferRef> SendBufferChunk::Open(std::uint32_t size) {
    if (offset_ + size > static_cast<std::uint32_t>(buffer_.size())) {
        return std::nullopt;
    }

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

void SendBufferChunk::Reset() noexcept {
    offset_ = 0;
}

bool SendBufferChunk::Full() const noexcept {
    return offset_ >= static_cast<std::uint32_t>(buffer_.size());
}

bool SendBufferChunk::Unused() const noexcept {
    return ref_count_.load(std::memory_order_acquire) == 0;
}

void SendBufferChunk::OnRelease() {
    if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        pool_->OnChunkUnused(this);
    }
}

BufferPool::BufferPool(std::uint32_t chunk_size, std::uint32_t max_chunks)
    : chunk_size_(chunk_size)
    , max_chunks_(max_chunks) {}

std::expected<SendBufferRef, CoreError> BufferPool::Allocate(std::uint32_t size) {
    std::scoped_lock lock(mutex_);
    LockMark(mutex_);
    FlushPendingGc();

    if (!active_chunks_.empty()) {
        if (auto buf = active_chunks_.back()->Open(size)) {
            return std::move(*buf);
        }
    }

    auto chunk = AcquireChunk();
    if (!chunk) {
        return std::unexpected(CoreError::kResourceExhausted);
    }

    active_chunks_.push_back(std::move(chunk));
    auto buf = active_chunks_.back()->Open(size);
    if (!buf) {
        return std::unexpected(CoreError::kResourceExhausted);
    }
    return std::move(*buf);
}

void BufferPool::OnChunkUnused(SendBufferChunk* chunk) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        pending_gc_.Push(chunk);
        return;
    }
    LockMark(mutex_);
    MoveToFree(chunk);
}

void BufferPool::FlushPendingGc() {
    pending_gc_.Drain([this](SendBufferChunk*&& chunk) {
        MoveToFree(chunk);
    });
}

void BufferPool::MoveToFree(SendBufferChunk* chunk) {
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

std::unique_ptr<SendBufferChunk> BufferPool::AcquireChunk() {
    if (!free_chunks_.empty()) {
        auto chunk = std::move(free_chunks_.back());
        free_chunks_.pop_back();
        return chunk;
    }
    if (active_chunks_.size() >= max_chunks_) {
        return nullptr;
    }
    return std::make_unique<SendBufferChunk>(this, chunk_size_);
}

} // namespace iouring::core::buffer
