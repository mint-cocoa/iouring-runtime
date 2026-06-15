#pragma once

namespace iouring::net {

class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(int fd);

    ~SocketHandle();

    // Move only
    SocketHandle(SocketHandle&& other) noexcept;

    SocketHandle& operator=(SocketHandle&& other) noexcept;

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    int Get() const;
    bool Valid() const;
    explicit operator bool() const;

    // Release ownership without closing
    int Release();

    void Reset(int new_fd = -1);

private:
    int fd_ = -1;
};

} // namespace iouring::net
