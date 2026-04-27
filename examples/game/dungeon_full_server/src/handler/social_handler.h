#pragma once
#include <cstdint>
#include <cstddef>

class Room;
struct PlayerState;

namespace handler {
void HandleChat(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len);
void HandleCreateParty(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len);
void HandleJoinParty(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len);
void HandleLeaveParty(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len);
}
