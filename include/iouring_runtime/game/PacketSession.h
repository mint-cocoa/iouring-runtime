#pragma once

#include <iouring_runtime/core/RecvBuffer.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/game/PacketBuilder.h>

#include <cstdint>
#include <span>

namespace iouring_runtime::game {

class PacketSession : public core::io::Session {
public:
    using Session::Session;

    template<core::ProtobufMessage T>
    void SendPacket(PacketId msg_id, const T& proto) {
        auto buffer = PacketBuilder::Build(Pool(), msg_id, proto);
        if (buffer) {
            Send(std::move(buffer));
        }
    }

protected:
    void OnRecv(std::span<const std::byte> data) final;
    virtual void OnPacket(PacketId msg_id, const std::byte* data,
                          std::uint32_t len) = 0;

private:
    static std::uint16_t ReadLe16(const std::byte* data);

    core::buffer::RecvBuffer recv_buffer_;
};

} // namespace iouring_runtime::game
