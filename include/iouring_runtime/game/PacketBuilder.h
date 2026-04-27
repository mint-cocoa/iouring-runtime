#pragma once

#include <iouring_runtime/core/Concepts.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/game/Types.h>

#include <cstdint>
#include <cstring>

namespace iouring_runtime::game {

struct PacketBuilder {
    static constexpr std::uint32_t kHeaderSize = 4;
    static constexpr std::uint32_t kMaxPacketSize = 8192;

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

} // namespace iouring_runtime::game
