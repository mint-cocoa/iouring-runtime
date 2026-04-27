#include "packet_session.h"
#include <spdlog/spdlog.h>

void PacketSession::OnRecv(std::span<const std::byte> data) {
    auto result = recv_buf_.Append(data);
    if (!result) {
        spdlog::warn("PacketSession: recv buffer overflow, disconnecting fd={}", Fd());
        Disconnect();
        return;
    }

    while (recv_buf_.ReadableSize() >= kHeaderSize) {
        auto region = recv_buf_.ReadRegion();
        auto* ptr = region.data();

        std::uint16_t pkt_size = ReadLE16(ptr);

        if (pkt_size < kHeaderSize || pkt_size > kMaxPacket) {
            spdlog::warn("PacketSession: invalid packet size={}, disconnecting fd={}", pkt_size, Fd());
            Disconnect();
            return;
        }

        if (recv_buf_.ReadableSize() < pkt_size)
            break;

        std::uint16_t msg_id = ReadLE16(ptr + 2);
        OnPacket(msg_id, ptr + kHeaderSize, pkt_size - kHeaderSize);
        recv_buf_.OnRead(pkt_size);
    }
}
