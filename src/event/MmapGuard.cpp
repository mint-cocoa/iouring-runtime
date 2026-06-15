#include <iouring/event/MmapGuard.h>

#include <sys/mman.h>

#include <utility>

namespace iouring::event {

MmapGuard::MmapGuard(void* ptr, std::size_t len)
    : ptr_(ptr)
    , len_(len) {
    if (ptr_ == MAP_FAILED) {
        ptr_ = nullptr;
    }
}

MmapGuard::~MmapGuard() {
    Reset();
}

MmapGuard::MmapGuard(MmapGuard&& other) noexcept
    : ptr_(std::exchange(other.ptr_, nullptr))
    , len_(std::exchange(other.len_, 0)) {}

MmapGuard& MmapGuard::operator=(MmapGuard&& other) noexcept {
    if (this != &other) {
        Reset();
        ptr_ = std::exchange(other.ptr_, nullptr);
        len_ = std::exchange(other.len_, 0);
    }
    return *this;
}

void* MmapGuard::Get() const {
    return ptr_;
}

std::size_t MmapGuard::Size() const {
    return len_;
}

bool MmapGuard::Valid() const {
    return ptr_ != nullptr;
}

MmapGuard::operator bool() const {
    return Valid();
}

void MmapGuard::Reset() {
    if (ptr_) {
        ::munmap(ptr_, len_);
        ptr_ = nullptr;
        len_ = 0;
    }
}

} // namespace iouring::event
