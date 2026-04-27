#pragma once

#include <iouring_runtime/game/Types.h>

enum class DungeonMsgId : iouring_runtime::game::PacketId {
    kCLogin = 101,
    kSLogin = 102,
    kCRoomList = 105,
    kSRoomList = 106,
    kCCreateRoom = 107,
    kSCreateRoom = 108,
    kCJoinRoom = 109,
    kSJoinRoom = 110,
    kCPortal = 214,
};

constexpr iouring_runtime::game::PacketId ToPacketId(DungeonMsgId msg_id) {
    return static_cast<iouring_runtime::game::PacketId>(msg_id);
}
