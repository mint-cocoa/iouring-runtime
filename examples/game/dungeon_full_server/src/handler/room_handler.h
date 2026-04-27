#pragma once
#include <cstdint>
#include <cstddef>

class GameSession;

namespace handler {
void HandleRoomList(GameSession& session, const std::byte* data, std::uint32_t len);
void HandleCreateRoom(GameSession& session, const std::byte* data, std::uint32_t len);
void HandleJoinRoom(GameSession& session, const std::byte* data, std::uint32_t len);
void HandlePortal(GameSession& session, const std::byte* data, std::uint32_t len);
}
