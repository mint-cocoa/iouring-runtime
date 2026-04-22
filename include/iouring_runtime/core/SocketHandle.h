#pragma once

#include <unistd.h>
#include <utility>

namespace iouring_runtime::core::io {

class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(int fd) : fd_(fd) {}

    ~SocketHandle() { Reset(); }

    // Move only
    SocketHandle(SocketHandle&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    int Get() const { return fd_; }
    bool Valid() const { return fd_ >= 0; }
    explicit operator bool() const { return Valid(); }

    // Release ownership without closing
    int Release() { return std::exchange(fd_, -1); }

    void Reset(int new_fd = -1) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = new_fd;
    }

private:
    int fd_ = -1;
};

} // namespace iouring_runtime::core::io
