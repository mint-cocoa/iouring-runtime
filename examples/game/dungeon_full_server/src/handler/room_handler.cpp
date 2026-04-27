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

static void SendSkillData(GameSession& session) {
    auto pkt = CombatSystem::BuildSkillDataPacket();
    session.SendPacket(MsgId::S_SKILL_DATA, pkt);
}

static void GenerateMapForRoom(Room* room) {
    auto seed = std::to_string(room->Id()) + "_" + room->Name();
    int depth = room->Depth();

    room->GetDungeon().Generate(seed, 3, depth);

    game::MapData md;
    room->GetDungeon().FillMapData(md);
    room->SetMapData(std::move(md));

    spdlog::info("Room[{}]: dungeon generated, grid={}x{}, props={}, lights={}",
                 room->Id(),
                 room->MapData().grid_width(),
                 room->MapData().grid_height(),
                 room->MapData().props_size(),
                 room->MapData().lights_size());
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

    // 던전 맵 생성
    GenerateMapForRoom(room);

    // S_CREATE_ROOM을 세션 스레드에서 직접 전송 (Room.Push 비동기 지연 방지)
    // 스폰 위치를 던전에서 미리 계산
    static thread_local std::mt19937 rng{std::random_device{}()};
    float rx = 0, rz = 0;
    if (!room->GetDungeon().GetRandomFloorPosition(rng, rx, rz)) {
        auto& spawn = room->SpawnPosition();
        rx = spawn.x(); rz = spawn.z();
    }

    auto zone_id = room->Id();

    // S_CREATE_ROOM 응답 구성 + 즉시 전송
    {
        game::S_CreateRoom reply;
        reply.set_success(true);
        reply.set_zone_id(zone_id);
        auto* pi = reply.mutable_player();
        pi->set_player_id(ctx->player_id);
        pi->set_name(ctx->char_name);
        pi->set_hp(100);
        pi->set_max_hp(100);
        pi->set_level(ctx->level);
        auto* pos = pi->mutable_position();
        pos->set_x(rx);
        pos->set_y(0.5f);
        pos->set_z(rz);

        *reply.mutable_map_data() = room->MapData();

        SendSkillData(session);
        session.SendPacket(MsgId::S_CREATE_ROOM, reply);
    }

    ctx->room = room;
    session.SetState(SessionState::InRoom);

    // Room에 플레이어 추가 (비동기 — 틱/브로드캐스트용)
    //
    // Order: SpawnBots first, then AddPlayer. This guarantees that when the
    // player joins, AddPlayer's S_PlayerList snapshot already contains all
    // 15 bots, so the client receives them atomically in a single packet
    // as its "room-entry snapshot" rather than as a later burst of
    // broadcasts. The client's OnPlayerListResponse handler treats this
    // exactly like the initial room state it was designed for.
    float spawn_x = rx, spawn_z = rz;
    room->Push([room, ctx, spawn_x, spawn_z] {
        room->SpawnBots(15);
        room->AddPlayer(ctx, spawn_x, 0.5f, spawn_z);
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

    if (!room || room->IsFull()) {
        game::S_JoinRoom reply;
        reply.set_success(false);
        reply.set_error(room ? "Room is full" : "Room not found");
        session.SendPacket(MsgId::S_JOIN_ROOM, reply);
        return;
    }

    // S_JOIN_ROOM을 세션 스레드에서 직접 전송 (Room.Push 비동기 지연 방지)
    static thread_local std::mt19937 rng{std::random_device{}()};
    float rx = 0, rz = 0;
    if (!room->GetDungeon().GetRandomFloorPosition(rng, rx, rz)) {
        auto& spawn = room->SpawnPosition();
        rx = spawn.x(); rz = spawn.z();
    }

    auto zone_id = room->Id();

    {
        game::S_JoinRoom reply;
        reply.set_success(true);
        reply.set_zone_id(zone_id);
        auto* pi = reply.mutable_player();
        pi->set_player_id(ctx->player_id);
        pi->set_name(ctx->char_name);
        pi->set_hp(100);
        pi->set_max_hp(100);
        pi->set_level(ctx->level);
        auto* pos = pi->mutable_position();
        pos->set_x(rx);
        pos->set_y(0.5f);
        pos->set_z(rz);

        *reply.mutable_map_data() = room->MapData();

        SendSkillData(session);
        session.SendPacket(MsgId::S_JOIN_ROOM, reply);
    }

    ctx->room = room;
    session.SetState(SessionState::InRoom);

    float spawn_x = rx, spawn_z = rz;
    room->Push([room, ctx, spawn_x, spawn_z] {
        room->AddPlayer(ctx, spawn_x, 0.5f, spawn_z);
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
    auto pid = ctx->player_id;
    auto portal_id = pkt.portal_id();

    // Check portal exists
    auto& portals = old_room->GetDungeon().GetPortals();
    if (portal_id >= portals.size()) return;

    // Check if this portal already has a connection
    auto& conns = old_room->Connections();
    Room* new_room = nullptr;

    auto cit = conns.find(portal_id);
    if (cit != conns.end()) {
        // Existing connection — travel to linked room
        new_room = rm->FindRoom(cit->second);
        if (!new_room) {
            // Room was cleaned up — remove stale connection, create fresh
            conns.erase(cit);
        }
    }

    if (!new_room) {
        // Create new room, depth = old_room depth + 1
        int newDepth = old_room->Depth() + 1;
        std::string name = "Zone_" + std::to_string(rm->NextId());
        new_room = rm->CreateRoom(name);
        if (!new_room) {
            game::S_Portal reply;
            reply.set_success(false);
            reply.set_error("Failed to create zone");
            session.SendPacket(MsgId::S_PORTAL, reply);
            return;
        }
        new_room->SetDepth(newDepth);
        GenerateMapForRoom(new_room);
        int botCount = std::min(10 + newDepth * 3, 30);  // 10 → 13 → 16 → ... max 30
        new_room->Push([new_room, botCount] {
            new_room->SpawnBots(botCount);
        });

        // Bidirectional link: old_room[portal_id] → new_room
        conns[portal_id] = new_room->Id();

        // Link one of new_room's portals back to old_room (first unlinked portal)
        auto& new_portals = new_room->GetDungeon().GetPortals();
        auto& new_conns = new_room->Connections();
        for (uint32_t i = 0; i < new_portals.size(); ++i) {
            if (new_conns.find(i) == new_conns.end()) {
                new_conns[i] = old_room->Id();
                break;
            }
        }
    }

    // Remove from old room
    old_room->Push([old_room, pid] {
        old_room->RemovePlayer(pid);
    });

    // Spawn near the portal that links back to old_room
    static thread_local std::mt19937 rng{std::random_device{}()};
    float rx = 0, rz = 0;
    bool foundPortalSpawn = false;
    {
        auto& np = new_room->GetDungeon().GetPortals();
        auto& nc = new_room->Connections();
        for (uint32_t i = 0; i < np.size(); ++i) {
            auto it = nc.find(i);
            if (it != nc.end() && it->second == old_room->Id()) {
                // Spawn offset from portal position
                std::uniform_real_distribution<float> off(-2.0f, 2.0f);
                rx = np[i].x + off(rng);
                rz = np[i].z + off(rng);
                foundPortalSpawn = true;
                break;
            }
        }
    }
    if (!foundPortalSpawn) {
        if (!new_room->GetDungeon().GetRandomFloorPosition(rng, rx, rz)) {
            auto& spawn = new_room->SpawnPosition();
            rx = spawn.x(); rz = spawn.z();
        }
    }

    // Send S_PORTAL response
    {
        game::S_Portal reply;
        reply.set_success(true);
        reply.set_zone_id(new_room->Id());
        auto* pi = reply.mutable_player();
        pi->set_player_id(ctx->player_id);
        pi->set_name(ctx->char_name);
        pi->set_hp(100);
        pi->set_max_hp(100);
        pi->set_level(ctx->level);
        auto* pos = pi->mutable_position();
        pos->set_x(rx); pos->set_y(0.5f); pos->set_z(rz);
        *reply.mutable_map_data() = new_room->MapData();
        session.SendPacket(MsgId::S_PORTAL, reply);
    }

    ctx->room = new_room;
    float spawn_x = rx, spawn_z = rz;
    new_room->Push([new_room, ctx, spawn_x, spawn_z] {
        new_room->AddPlayer(ctx, spawn_x, 0.5f, spawn_z);
    });

    spdlog::info("Portal: player {} moved to zone {} (from zone {})",
                 pid, new_room->Id(), old_room->Id());
}

}  // namespace handler
