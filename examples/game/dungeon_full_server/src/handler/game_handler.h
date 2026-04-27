#pragma once
#include <cstdint>
#include <cstddef>

class Room;
struct PlayerState;

namespace handler {
void HandleMove(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len);
void HandleAttack(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len);
void HandleFire(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len);
void HandlePickup(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len);
void HandleUseItem(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len);
}
