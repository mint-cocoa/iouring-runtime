#pragma once

#include <iouring/core/Error.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace iouring::core::buffer {

// Fallback buffer for incomplete packet reassembly.
//
// In the fast path (buffer empty), provided buffer data is parsed in-place
// with zero memcpy. Only when a packet spans multiple recv completions
// does data get copied here (slow path).
class RecvBuffer {
public:
    explicit RecvBuffer(std::uint32_t size = 65536);

    // Append data from provided buffer into this reassembly buffer.
    // Returns CoreError::kResourceExhausted if buffer is full and data would be truncated.
    [[nodiscard]] std::expected<void, CoreError> Append(std::span<const std::byte> data);

    std::span<std::byte> WriteRegion();

    std::span<const std::byte> ReadRegion() const;

    void OnWrite(std::uint32_t bytes);

    void OnRead(std::uint32_t bytes);

    void Compact();

    bool IsEmpty() const;
    bool ShouldCompact() const;
    std::uint32_t ReadableSize() const;
    std::uint32_t WritableSize() const;

private:
    std::unique_ptr<std::byte[]> buffer_;
    std::uint32_t capacity_;
    std::uint32_t read_pos_ = 0;
    std::uint32_t write_pos_ = 0;
};

} // namespace iouring::core::buffer
