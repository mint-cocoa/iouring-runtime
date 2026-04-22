#pragma once

#include <sys/mman.h>
#include <cstddef>
#include <utility>

namespace iouring_runtime::core::ring {

class MmapGuard {
public:
    MmapGuard() = default;

    MmapGuard(void* ptr, std::size_t len) : ptr_(ptr), len_(len) {
        if (ptr_ == MAP_FAILED)
            ptr_ = nullptr;
    }

    ~MmapGuard() { Reset(); }

    // Move only
    MmapGuard(MmapGuard&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr))
        , len_(std::exchange(other.len_, 0)) {}

    MmapGuard& operator=(MmapGuard&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = std::exchange(other.ptr_, nullptr);
            len_ = std::exchange(other.len_, 0);
        }
        return *this;
    }

    MmapGuard(const MmapGuard&) = delete;
    MmapGuard& operator=(const MmapGuard&) = delete;

    void* Get() const { return ptr_; }
    std::size_t Size() const { return len_; }
    bool Valid() const { return ptr_ != nullptr; }
    explicit operator bool() const { return Valid(); }

    void Reset() {
        if (ptr_) {
            ::munmap(ptr_, len_);
            ptr_ = nullptr;
            len_ = 0;
        }
    }

private:
    void* ptr_ = nullptr;
    std::size_t len_ = 0;
};

} // namespace iouring_runtime::core::ring
