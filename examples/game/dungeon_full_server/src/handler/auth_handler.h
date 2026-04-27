#pragma once
#include <cstdint>
#include <cstddef>

class GameSession;

namespace handler {
void HandleLogin(GameSession& session, const std::byte* data, std::uint32_t len);
void HandleRegister(GameSession& session, const std::byte* data, std::uint32_t len);
}
