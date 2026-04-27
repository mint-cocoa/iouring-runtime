#pragma once

#include <iouring_runtime/game/Room.h>

class DungeonRoom final : public iouring_runtime::game::Room {
public:
    using Room::Room;

protected:
    void OnPacket(iouring_runtime::game::PlayerState& player,
                  iouring_runtime::game::PacketId packet_id,
                  std::span<const std::byte> payload) override;
};
