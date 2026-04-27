#pragma once
#include <cstdint>
#include <chrono>

using PlayerId  = std::uint64_t;
using RoomId    = std::uint32_t;
using CharId    = std::uint64_t;
using PartyId   = std::uint64_t;
using TimePoint = std::chrono::steady_clock::time_point;

enum class MsgId : std::uint16_t {
    // Auth
    C_LOGIN       = 101, S_LOGIN       = 102,
    C_ENTER_GAME  = 103, S_ENTER_GAME  = 104,
    C_ROOM_LIST   = 105, S_ROOM_LIST   = 106,
    C_CREATE_ROOM = 107, S_CREATE_ROOM = 108,
    C_JOIN_ROOM   = 109, S_JOIN_ROOM   = 110,
    C_REGISTER    = 111, S_REGISTER    = 112,
    C_CHAR_LIST   = 113, S_CHAR_LIST   = 114,
    C_CREATE_CHAR = 115, S_CREATE_CHAR = 116,
    C_SELECT_CHAR = 117, S_SELECT_CHAR = 118,
    C_SCENE_READY = 119,  // client → server: scene loaded, handlers registered, ready for snapshot
    // Game
    C_MOVE        = 201, S_MOVE        = 202,
    S_SPAWN       = 203, S_DESPAWN     = 204,
    C_ATTACK      = 205, S_ATTACK      = 206,
    S_DAMAGE      = 207, S_PLAYER_LIST = 208,
    S_SKILL_DATA  = 209, S_RESPAWN     = 210,
    C_FIRE        = 211, S_FIRE        = 212,
    S_SCOREBOARD  = 213,
    C_PORTAL      = 214, S_PORTAL      = 215,
    S_GROUND_ITEM_SPAWN = 216, S_GROUND_ITEM_DESPAWN = 217,
    C_PICKUP      = 218, S_PICKUP      = 219,
    // Social
    C_CHAT         = 301, S_CHAT         = 302,
    C_CREATE_PARTY = 303, S_CREATE_PARTY = 304,
    C_JOIN_PARTY   = 305, S_JOIN_PARTY   = 306,
    C_LEAVE_PARTY  = 307, S_LEAVE_PARTY  = 308,
    S_PARTY_UPDATE = 309,
    // Inventory
    S_INVENTORY_INIT = 501,
    C_USE_ITEM  = 502, S_USE_ITEM  = 503,
    C_DROP_ITEM = 504, S_DROP_ITEM = 505,
    C_MOVE_ITEM = 506, S_MOVE_ITEM = 507,
    S_ITEM_ADD  = 508, S_ITEM_REMOVE = 509,
    // Currency
    S_CURRENCY_INIT   = 601,
    C_PURCHASE        = 602, S_PURCHASE = 603,
    S_CURRENCY_UPDATE = 604,
};
