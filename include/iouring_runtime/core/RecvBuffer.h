#pragma once

#include <iouring_runtime/core/Error.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <span>

namespace iouring_runtime::core::buffer {

// Fallback buffer for incomplete packet reassembly.
//
// In the fast path (buffer empty), provided buffer data is parsed in-place
// with zero memcpy. Only when a packet spans multiple recv completions
// does data get copied here (slow path).
class RecvBuffer {
public:
    explicit RecvBuffer(std::uint32_t size = 65536)
        : buffer_(std::make_unique<std::byte[]>(size))
        , capacity_(size) {}

    // Append data from provided buffer into this reassembly buffer.
    // Returns CoreError::kResourceExhausted if buffer is full and data would be truncated.
    [[nodiscard]] std::expected<void, CoreError> Append(std::span<const std::byte> data) {
        if (WritableSize() < static_cast<std::uint32_t>(data.size()))
            Compact();
        if (WritableSize() < static_cast<std::uint32_t>(data.size()))
            return std::unexpected(CoreError::kResourceExhausted);
        std::memcpy(buffer_.get() + write_pos_, data.data(), data.size());
        write_pos_ += static_cast<std::uint32_t>(data.size());
        return {};
    }

    std::span<std::byte> WriteRegion() {
        return {buffer_.get() + write_pos_, capacity_ - write_pos_};
    }

    std::span<const std::byte> ReadRegion() const {
        return {buffer_.get() + read_pos_, write_pos_ - read_pos_};
    }

    void OnWrite(std::uint32_t bytes) {
        write_pos_ += bytes;
    }

    void OnRead(std::uint32_t bytes) {
        read_pos_ += bytes;
        if (read_pos_ == write_pos_) {
            read_pos_ = 0;
            write_pos_ = 0;
        }
    }

    void Compact() {
        if (read_pos_ == 0) return;
        std::uint32_t remaining = write_pos_ - read_pos_;
        if (remaining > 0)
            std::memmove(buffer_.get(), buffer_.get() + read_pos_, remaining);
        read_pos_ = 0;
        write_pos_ = remaining;
    }

    bool IsEmpty() const { return read_pos_ == write_pos_; }
    bool ShouldCompact() const { return read_pos_ > capacity_ / 2; }
    std::uint32_t ReadableSize() const { return write_pos_ - read_pos_; }
    std::uint32_t WritableSize() const { return capacity_ - write_pos_; }

private:
    std::unique_ptr<std::byte[]> buffer_;
    std::uint32_t capacity_;
    std::uint32_t read_pos_ = 0;
    std::uint32_t write_pos_ = 0;
};

} // namespace iouring_runtime::core::buffer
