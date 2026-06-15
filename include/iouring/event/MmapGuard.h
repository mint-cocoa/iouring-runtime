#pragma once

#include <cstddef>

namespace iouring::event {

class MmapGuard {
public:
    MmapGuard() = default;

    MmapGuard(void* ptr, std::size_t len);

    ~MmapGuard();

    // Move only
    MmapGuard(MmapGuard&& other) noexcept;

    MmapGuard& operator=(MmapGuard&& other) noexcept;

    MmapGuard(const MmapGuard&) = delete;
    MmapGuard& operator=(const MmapGuard&) = delete;

    void* Get() const;
    std::size_t Size() const;
    bool Valid() const;
    explicit operator bool() const;

    void Reset();

private:
    void* ptr_ = nullptr;
    std::size_t len_ = 0;
};

} // namespace iouring::event
