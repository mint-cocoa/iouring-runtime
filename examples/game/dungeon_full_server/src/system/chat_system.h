#pragma once
#include "../types.h"

class Room;
struct PlayerState;
namespace game { class C_Chat; }

class ChatSystem {
public:
    static void ProcessChat(Room& room, PlayerState& sender, const game::C_Chat& msg);
};
