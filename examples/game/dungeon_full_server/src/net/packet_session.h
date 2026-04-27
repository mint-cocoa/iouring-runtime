#pragma once

#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/RecvBuffer.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <cstring>

class PacketSession : public iouring_runtime::core::io::Session {
public:
    using Session::Session;

protected:
    void OnRecv(std::span<const std::byte> data) override;
    virtual void OnPacket(std::uint16_t msg_id, const std::byte* data, std::uint32_t len) = 0;

private:
    iouring_runtime::core::buffer::RecvBuffer recv_buf_;
    static constexpr std::uint32_t kHeaderSize = 4;
    static constexpr std::uint32_t kMaxPacket  = 8192;

    static std::uint16_t ReadLE16(const std::byte* p) {
        std::uint16_t val;
        std::memcpy(&val, p, 2);
        return val;
    }
};
