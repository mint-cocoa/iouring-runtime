#pragma once

#include <chrono>
#include <cstdint>

namespace iouring_runtime::game {

using PlayerId = std::uint64_t;
using RoomId = std::uint32_t;
using CharacterId = std::uint64_t;
using PartyId = std::uint64_t;
using TimePoint = std::chrono::steady_clock::time_point;
using PacketId = std::uint16_t;

} // namespace iouring_runtime::game
