#pragma once

#include <iouring/core/Concepts.h>
#include <iouring/core/SendBuffer.h>
#include <iouring/game/Types.h>

#include <cstdint>
#include <cstring>
#include <limits>

namespace iouring::game {

struct PacketBuilder {
    static constexpr std::uint32_t kHeaderSize = 4;
    static constexpr std::uint32_t kMaxPacketSize =
        std::numeric_limits<std::uint16_t>::max();

    template<core::ProtobufMessage T>
    static core::buffer::SendBufferRef Build(core::buffer::BufferPool& pool,
                                             PacketId msg_id,
                                             const T& proto) {
        const auto payload_size =
            static_cast<std::uint32_t>(proto.ByteSizeLong());
        const std::uint32_t total_size = kHeaderSize + payload_size;
        if (total_size > kMaxPacketSize) {
            return nullptr;
        }

        auto result = pool.Allocate(total_size);
        if (!result) {
            return nullptr;
        }

        auto buffer = std::move(*result);
        auto writable = buffer->Writable();
        auto* out = writable.data();

        const auto packet_size = static_cast<std::uint16_t>(total_size);
        const auto packet_id = static_cast<std::uint16_t>(msg_id);
        std::memcpy(out, &packet_size, sizeof(packet_size));
        std::memcpy(out + sizeof(packet_size), &packet_id, sizeof(packet_id));

        if (payload_size != 0 &&
            !proto.SerializeToArray(out + kHeaderSize,
                                    static_cast<int>(payload_size))) {
            return nullptr;
        }

        buffer->Commit(total_size);
        return buffer;
    }
};

} // namespace iouring::game
