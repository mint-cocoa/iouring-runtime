#include "DungeonRoom.h"

#include <cstdint>
#include <iostream>

void DungeonRoom::OnPacket(iouring_runtime::game::PlayerState& player,
                           iouring_runtime::game::PacketId packet_id,
                           std::span<const std::byte> payload) {
    std::cout << "room=" << Id()
              << " player=" << player.player_id
              << " packet=" << static_cast<std::uint16_t>(packet_id)
              << " payload=" << payload.size() << "\n";
}
