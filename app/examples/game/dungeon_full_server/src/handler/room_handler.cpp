#include "room_handler.h"
#include "../net/game_session.h"
#include "../net/io_worker.h"
#include "../net/io_worker_pool.h"
#include "../game/player_context.h"
#include "../game/room.h"
#include "../game/room_manager.h"
#include "../game/dungeon_generator.h"
#include "../system/combat_system.h"
#include <random>
#include "Auth.pb.h"
#include "Game.pb.h"
#include "Common.pb.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace handler {

void HandleRoomList(GameSession& session, const std::byte* data, std::uint32_t len) {
    if (session.GetState() != SessionState::InLobby) return;

    auto* rm = session.GetRoomManager();
    auto list = rm->GetRoomList();

    game::S_RoomList reply;
    for (auto& info : list) {
        auto* ri = reply.add_rooms();
        ri->set_zone_id(info.id);
        ri->set_room_name(info.name);
        ri->set_player_count(info.player_count);
        ri->set_max_players(500);
    }
    session.SendPacket(MsgId::S_ROOM_LIST, reply);
}

void HandleCreateRoom(GameSession& session, const std::byte* data, std::uint32_t len) {
    if (session.GetState() != SessionState::InLobby) return;

    game::C_CreateRoom pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    auto* ctx = session.GetPlayerCtx();
    if (!ctx) return;

    auto* rm = session.GetRoomManager();
    auto* room = rm->CreateRoom(pkt.room_name());
    if (!room) {
        game::S_CreateRoom reply;
        reply.set_success(false);
        session.SendPacket(MsgId::S_CREATE_ROOM, reply);
        return;
    }

    auto request = *ctx;
    room->Push([room, request = std::move(request)] mutable {
        room->TryCreateEnter(std::move(request));
    });
}

void HandleJoinRoom(GameSession& session, const std::byte* data, std::uint32_t len) {
    if (session.GetState() != SessionState::InLobby) return;

    game::C_JoinRoom pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    auto* ctx = session.GetPlayerCtx();
    if (!ctx) return;

    auto* rm = session.GetRoomManager();
    auto* room = rm->FindRoom(pkt.zone_id());

    if (!room) {
        game::S_JoinRoom reply;
        reply.set_success(false);
        reply.set_error("Room not found");
        session.SendPacket(MsgId::S_JOIN_ROOM, reply);
        return;
    }

    auto request = *ctx;
    room->Push([room, request = std::move(request)] mutable {
        room->TryJoin(std::move(request));
    });
}

void HandlePortal(GameSession& session, const std::byte* data, std::uint32_t len) {
    if (session.GetState() != SessionState::InRoom) return;

    game::C_Portal pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    auto* ctx = session.GetPlayerCtx();
    if (!ctx || !ctx->room) return;

    auto* old_room = ctx->room;
    auto* rm = session.GetRoomManager();
    auto portal_id = pkt.portal_id();

    auto request = *ctx;
    old_room->Push([old_room, rm, request = std::move(request), portal_id] mutable {
        old_room->TryPortal(std::move(request), rm, portal_id);
    });
}

}  // namespace handler
