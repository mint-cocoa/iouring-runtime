#pragma once

#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/Concepts.h>
#include "../types.h"
#include <spdlog/spdlog.h>
#include <cstring>

struct PacketBuilder {
    template<iouring_runtime::core::ProtobufMessage T>
    static iouring_runtime::core::buffer::SendBufferRef Build(
        iouring_runtime::core::buffer::BufferPool& pool,
        MsgId msg_id,
        const T& proto)
    {
        auto payload_size = static_cast<std::uint32_t>(proto.ByteSizeLong());
        std::uint32_t total = 4 + payload_size;

        auto result = pool.Allocate(total);
        if (!result) {
            spdlog::error("PacketBuilder: failed to allocate buffer, size={}", total);
            return nullptr;
        }

        auto buf = std::move(*result);
        auto writable = buf->Writable();
        auto* w = writable.data();

        auto size_val = static_cast<std::uint16_t>(total);
        auto id_val   = static_cast<std::uint16_t>(msg_id);
        std::memcpy(w,     &size_val, 2);
        std::memcpy(w + 2, &id_val,   2);
        proto.SerializeToArray(w + 4, static_cast<int>(payload_size));

        buf->Commit(total);
        return buf;
    }
};
