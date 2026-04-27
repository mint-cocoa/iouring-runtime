#include "social_handler.h"
#include "../game/room.h"
#include "../system/chat_system.h"
#include "../system/party_system.h"
#include "Social.pb.h"

// Room이 PartySystem을 소유해야 하므로 static으로 임시 처리
// TODO: Room 멤버로 이동
static PartySystem g_party_system;

namespace handler {

void HandleChat(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len) {
    game::C_Chat pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;
    ChatSystem::ProcessChat(room, ps, pkt);
}

void HandleCreateParty(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len) {
    g_party_system.CreateParty(room, ps.player_id);
}

void HandleJoinParty(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len) {
    game::C_JoinParty pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;
    g_party_system.JoinParty(room, ps.player_id, pkt.party_id());
}

void HandleLeaveParty(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len) {
    g_party_system.LeaveParty(room, ps.player_id);
}

}  // namespace handler
