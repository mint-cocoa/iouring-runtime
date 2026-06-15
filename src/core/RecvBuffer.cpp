#include <iouring/core/RecvBuffer.h>

#include <cstring>

namespace iouring::core::buffer {

RecvBuffer::RecvBuffer(std::uint32_t size)
    : buffer_(std::make_unique<std::byte[]>(size))
    , capacity_(size) {}

std::expected<void, CoreError> RecvBuffer::Append(std::span<const std::byte> data) {
    if (WritableSize() < static_cast<std::uint32_t>(data.size())) {
        Compact();
    }
    if (WritableSize() < static_cast<std::uint32_t>(data.size())) {
        return std::unexpected(CoreError::kResourceExhausted);
    }
    std::memcpy(buffer_.get() + write_pos_, data.data(), data.size());
    write_pos_ += static_cast<std::uint32_t>(data.size());
    return {};
}

std::span<std::byte> RecvBuffer::WriteRegion() {
    return {buffer_.get() + write_pos_, capacity_ - write_pos_};
}

std::span<const std::byte> RecvBuffer::ReadRegion() const {
    return {buffer_.get() + read_pos_, write_pos_ - read_pos_};
}

void RecvBuffer::OnWrite(std::uint32_t bytes) {
    write_pos_ += bytes;
}

void RecvBuffer::OnRead(std::uint32_t bytes) {
    read_pos_ += bytes;
    if (read_pos_ == write_pos_) {
        read_pos_ = 0;
        write_pos_ = 0;
    }
}

void RecvBuffer::Compact() {
    if (read_pos_ == 0) {
        return;
    }
    std::uint32_t remaining = write_pos_ - read_pos_;
    if (remaining > 0) {
        std::memmove(buffer_.get(), buffer_.get() + read_pos_, remaining);
    }
    read_pos_ = 0;
    write_pos_ = remaining;
}

bool RecvBuffer::IsEmpty() const {
    return read_pos_ == write_pos_;
}

bool RecvBuffer::ShouldCompact() const {
    return read_pos_ > capacity_ / 2;
}

std::uint32_t RecvBuffer::ReadableSize() const {
    return write_pos_ - read_pos_;
}

std::uint32_t RecvBuffer::WritableSize() const {
    return capacity_ - write_pos_;
}

} // namespace iouring::core::buffer
