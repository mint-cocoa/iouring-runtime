#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace iouring_runtime::core::buffer {

inline constexpr std::size_t kDefaultPausedRecvByteLimit = 1024 * 1024;

class PausedRecvQueue {
public:
    struct Chunk {
        std::vector<std::byte> data;
    };

    struct Stats {
        std::size_t current_bytes = 0;
        std::size_t peak_bytes = 0;
        std::uint64_t events = 0;
        std::uint64_t overflow_count = 0;
        std::uint64_t dropped_bytes = 0;
    };

    explicit PausedRecvQueue(
        std::size_t byte_limit = kDefaultPausedRecvByteLimit)
        : byte_limit_(byte_limit) {}

    [[nodiscard]] bool Push(std::span<const std::byte> data) {
        if (data.empty()) {
            return true;
        }

        if (byte_limit_ != 0 && data.size() > byte_limit_ - bytes_) {
            ++overflow_count_;
            dropped_bytes_ += data.size();
            return false;
        }

        auto& chunk = chunks_.emplace_back();
        chunk.data.resize(data.size());
        std::memcpy(chunk.data.data(), data.data(), data.size());
        bytes_ += data.size();
        peak_bytes_ = std::max(peak_bytes_, bytes_);
        ++events_;
        return true;
    }

    [[nodiscard]] std::optional<Chunk> Pop() {
        if (chunks_.empty()) {
            return std::nullopt;
        }

        Chunk chunk = std::move(chunks_.front());
        chunks_.pop_front();
        bytes_ -= chunk.data.size();
        return chunk;
    }

    bool Empty() const noexcept { return chunks_.empty(); }
    std::size_t Bytes() const noexcept { return bytes_; }
    std::size_t ByteLimit() const noexcept { return byte_limit_; }
    std::uint64_t DroppedBytes() const noexcept { return dropped_bytes_; }

    Stats Snapshot() const noexcept {
        return Stats{
            .current_bytes = bytes_,
            .peak_bytes = peak_bytes_,
            .events = events_,
            .overflow_count = overflow_count_,
            .dropped_bytes = dropped_bytes_,
        };
    }

private:
    std::deque<Chunk> chunks_;
    std::size_t byte_limit_ = kDefaultPausedRecvByteLimit;
    std::size_t bytes_ = 0;
    std::size_t peak_bytes_ = 0;
    std::uint64_t events_ = 0;
    std::uint64_t overflow_count_ = 0;
    std::uint64_t dropped_bytes_ = 0;
};

} // namespace iouring_runtime::core::buffer
