#pragma once
#include <cstdint>
#include <cstddef>

class GameSession;

namespace handler {
void HandleCharList(GameSession& session, const std::byte* data, std::uint32_t len);
void HandleCreateChar(GameSession& session, const std::byte* data, std::uint32_t len);
void HandleSelectChar(GameSession& session, const std::byte* data, std::uint32_t len);
}
