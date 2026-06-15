#pragma once

#include <iouring/core/RecvBuffer.h>
#include <iouring/net/Session.h>
#include <iouring/game/PacketBuilder.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace iouring::game {

class PacketSession : public net::Session {
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
    bool AppendToRecvBuffer(std::span<const std::byte> data);
    bool ValidatePacketSize(std::uint16_t packet_size);
    bool CompleteBufferedPacket(std::span<const std::byte> data,
                                std::size_t& offset);

    core::buffer::RecvBuffer recv_buffer_;
};

} // namespace iouring::game
