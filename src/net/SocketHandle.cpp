#include <iouring/net/SocketHandle.h>

#include <unistd.h>

#include <utility>

namespace iouring::net {

SocketHandle::SocketHandle(int fd) : fd_(fd) {}

SocketHandle::~SocketHandle() {
    Reset();
}

SocketHandle::SocketHandle(SocketHandle&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

SocketHandle& SocketHandle::operator=(SocketHandle&& other) noexcept {
    if (this != &other) {
        Reset();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

int SocketHandle::Get() const {
    return fd_;
}

bool SocketHandle::Valid() const {
    return fd_ >= 0;
}

SocketHandle::operator bool() const {
    return Valid();
}

int SocketHandle::Release() {
    return std::exchange(fd_, -1);
}

void SocketHandle::Reset(int new_fd) {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = new_fd;
}

} // namespace iouring::net
