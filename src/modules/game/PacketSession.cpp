#include <iouring_runtime/game/PacketSession.h>

#include <iouring_runtime/observability/Logging.h>

#include <algorithm>
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

bool PacketSession::AppendToRecvBuffer(std::span<const std::byte> data) {
    if (data.empty()) {
        return true;
    }

    auto append = recv_buffer_.Append(data);
    if (!append) {
        obs::LogWarn(kLogCategory,
                     "PacketSession[fd={}]: recv buffer overflow", Fd());
        Disconnect();
        return false;
    }

    return true;
}

bool PacketSession::ValidatePacketSize(std::uint16_t packet_size) {
    if (packet_size >= PacketBuilder::kHeaderSize &&
        packet_size <= PacketBuilder::kMaxPacketSize) {
        return true;
    }

    obs::LogWarn(kLogCategory,
                 "PacketSession[fd={}]: invalid packet size {}",
                 Fd(), packet_size);
    Disconnect();
    return false;
}

bool PacketSession::CompleteBufferedPacket(std::span<const std::byte> data,
                                           std::size_t& offset) {
    if (recv_buffer_.ReadableSize() < PacketBuilder::kHeaderSize) {
        const auto needed = PacketBuilder::kHeaderSize - recv_buffer_.ReadableSize();
        const auto copied = std::min<std::size_t>(needed, data.size() - offset);
        if (!AppendToRecvBuffer(data.subspan(offset, copied))) {
            return false;
        }
        offset += copied;

        if (recv_buffer_.ReadableSize() < PacketBuilder::kHeaderSize) {
            return true;
        }
    }

    const auto region = recv_buffer_.ReadRegion();
    const auto packet_size = ReadLe16(region.data());
    if (!ValidatePacketSize(packet_size)) {
        return false;
    }

    const auto needed = packet_size - recv_buffer_.ReadableSize();
    const auto copied = std::min<std::size_t>(needed, data.size() - offset);
    if (!AppendToRecvBuffer(data.subspan(offset, copied))) {
        return false;
    }
    offset += copied;

    if (recv_buffer_.ReadableSize() < packet_size) {
        return true;
    }

    const auto packet = recv_buffer_.ReadRegion();
    const auto packet_id = ReadLe16(packet.data() + sizeof(std::uint16_t));
    OnPacket(packet_id, packet.data() + PacketBuilder::kHeaderSize,
             packet_size - PacketBuilder::kHeaderSize);
    recv_buffer_.OnRead(packet_size);
    return true;
}

void PacketSession::OnRecv(std::span<const std::byte> data) {
    std::size_t offset = 0;

    while (offset < data.size()) {
        if (!recv_buffer_.IsEmpty()) {
            if (!CompleteBufferedPacket(data, offset)) {
                return;
            }
            if (!recv_buffer_.IsEmpty()) {
                break;
            }
            continue;
        }

        const auto remaining = data.size() - offset;
        if (remaining < PacketBuilder::kHeaderSize) {
            if (!AppendToRecvBuffer(data.subspan(offset))) {
                return;
            }
            break;
        }

        const auto* current = data.data() + offset;
        const auto packet_size = ReadLe16(current);
        if (!ValidatePacketSize(packet_size)) {
            return;
        }

        if (remaining < packet_size) {
            if (!AppendToRecvBuffer(data.subspan(offset))) {
                return;
            }
            break;
        }

        const auto packet_id = ReadLe16(current + sizeof(std::uint16_t));
        OnPacket(packet_id, current + PacketBuilder::kHeaderSize,
                 packet_size - PacketBuilder::kHeaderSize);
        offset += packet_size;
    }

    if (recv_buffer_.ShouldCompact()) {
        recv_buffer_.Compact();
    }
}

} // namespace iouring_runtime::game
