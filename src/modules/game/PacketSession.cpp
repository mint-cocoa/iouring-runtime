#include <iouring_runtime/game/PacketSession.h>

#include <iouring_runtime/observability/Logging.h>

#include <cstring>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kSession;
}

namespace iouring_runtime::game {

std::uint16_t PacketSession::ReadLe16(const std::byte* data) {
    std::uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

void PacketSession::OnRecv(std::span<const std::byte> data) {
    auto append = recv_buffer_.Append(data);
    if (!append) {
        obs::LogWarn(kLogCategory,
                     "PacketSession[fd={}]: recv buffer overflow", Fd());
        Disconnect();
        return;
    }

    while (recv_buffer_.ReadableSize() >= PacketBuilder::kHeaderSize) {
        const auto region = recv_buffer_.ReadRegion();
        const auto* current = region.data();
        const auto packet_size = ReadLe16(current);

        if (packet_size < PacketBuilder::kHeaderSize ||
            packet_size > PacketBuilder::kMaxPacketSize) {
            obs::LogWarn(kLogCategory,
                         "PacketSession[fd={}]: invalid packet size {}",
                         Fd(), packet_size);
            Disconnect();
            return;
        }

        if (recv_buffer_.ReadableSize() < packet_size) {
            break;
        }

        const auto packet_id = ReadLe16(current + sizeof(std::uint16_t));
        OnPacket(packet_id, current + PacketBuilder::kHeaderSize,
                 packet_size - PacketBuilder::kHeaderSize);
        recv_buffer_.OnRead(packet_size);
    }

    if (recv_buffer_.ShouldCompact()) {
        recv_buffer_.Compact();
    }
}

} // namespace iouring_runtime::game
