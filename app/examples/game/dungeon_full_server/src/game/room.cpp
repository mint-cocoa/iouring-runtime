#include "room.h"
#include "room_manager.h"
#include "../net/io_worker_pool.h"
#include "../net/io_worker.h"
#include "../net/game_session.h"
#include "../handler/game_handler.h"
#include "../handler/social_handler.h"
#include "../system/combat_system.h"
#include <spdlog/spdlog.h>

#include "Game.pb.h"
#include "Auth.pb.h"
#include "Common.pb.h"
#include "Inventory.pb.h"

#include <algorithm>
#include <utility>
#include <memory>
#include <random>

Room::Room(RoomId id, std::string name,
           iouring_runtime::core::job::GlobalQueue& gq,
           IoWorkerPool* workers)
    : iouring_runtime::game::Room(id, std::move(name), gq)
    , workers_(workers)
{
}

iouring_runtime::core::buffer::BufferPool& Room::GetPool() {
    return workers_->GetWorker(0)->Pool();
}

namespace {

void GenerateMapForRoom(Room& room) {
    auto seed = std::to_string(room.Id()) + "_" + room.Name();
    int depth = room.Depth();

    room.GetDungeon().Generate(seed, 3, depth);

    game::MapData md;
    room.GetDungeon().FillMapData(md);
    room.SetMapData(std::move(md));

    spdlog::info("Room[{}]: dungeon generated, grid={}x{}, props={}, lights={}",
                 room.Id(),
                 room.MapData().grid_width(),
                 room.MapData().grid_height(),
                 room.MapData().props_size(),
                 room.MapData().lights_size());
}

template<iouring_runtime::core::ProtobufMessage T>
void EnqueuePacket(IoWorkerPool* workers, Room& room,
                   iouring_runtime::core::SessionId sid,
                   iouring_runtime::core::ContextId wid, MsgId msg_id,
                   const T& proto) {
    auto* worker = workers->GetWorker(wid);
    if (!worker) {
        return;
    }

    auto buffer = iouring_runtime::game::PacketBuilder::Build(
        worker->Pool(), static_cast<iouring_runtime::game::PacketId>(msg_id),
        proto);
    if (!buffer) {
        return;
    }

    worker->EnqueueOutbound(OutboundMessage{
        .session_id = sid,
        .room_id = room.Id(),
        .room_seq = room.NextRoomSeqForOutbox(),
        .buffer = std::move(buffer),
    });
}

void EnqueueSessionEnterState(IoWorkerPool* workers, Room& room,
                              const PlayerContext& request) {
    auto* worker = workers->GetWorker(request.worker_id);
    if (!worker) {
        return;
    }

    auto weak = request.session;
    const auto player_id = request.player_id;
    worker->EnqueueOutbound(OutboundMessage{
        .session_id = request.session_id,
        .room_id = room.Id(),
        .room_seq = room.NextRoomSeqForOutbox(),
        .task = [weak = std::move(weak), player_id, room = &room] {
            auto base = weak.lock();
            if (!base) {
                return;
            }

            auto session = std::dynamic_pointer_cast<GameSession>(base);
            if (!session) {
                return;
            }

            auto* ctx = session->GetPlayerCtx();
            if (!ctx || ctx->player_id != player_id) {
                return;
            }

            ctx->room = room;
            session->SetState(SessionState::InRoom);
        },
    });
}

bool PickSpawn(Room& room, float& x, float& z) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    if (room.GetDungeon().GetRandomFloorPosition(rng, x, z)) {
        return true;
    }

    auto& spawn = room.SpawnPosition();
    x = spawn.x();
    z = spawn.z();
    return false;
}

} // namespace

void Room::SendTo(PlayerState& ps, MsgId msg_id,
                  iouring_runtime::core::buffer::SendBufferRef buf) {
    (void)msg_id;
    if (!buf || !ps.worker_ring) return;  // bots have no session
    auto sid = ps.session_id;
    auto wid = ps.worker_id;
    auto* worker = workers_->GetWorker(wid);
    if (!worker) return;

    worker->EnqueueOutbound(OutboundMessage{
        .session_id = sid,
        .room_id = Id(),
        .room_seq = next_room_seq_++,
        .buffer = std::move(buf),
    });
}

void Room::BroadcastAll(MsgId msg_id, iouring_runtime::core::buffer::SendBufferRef buf) {
    (void)msg_id;
    if (!buf) return;
    for (auto& [_, ps] : players_) {
        if (ps.session_id == 0) continue;   // bot — no session to send to
        if (!ps.scene_ready) continue;      // client not ready for broadcasts yet
        auto sid = ps.session_id;
        auto wid = ps.worker_id;
        auto* worker = workers_->GetWorker(wid);
        if (!worker) continue;

        worker->EnqueueOutbound(OutboundMessage{
            .session_id = sid,
            .room_id = Id(),
            .room_seq = next_room_seq_++,
            .buffer = buf,
        });
    }
}

void Room::BroadcastExcept(PlayerId exclude, MsgId msg_id,
                           iouring_runtime::core::buffer::SendBufferRef buf) {
    (void)msg_id;
    if (!buf) return;
    for (auto& [pid, ps] : players_) {
        if (pid == exclude) continue;
        if (ps.session_id == 0) continue;  // skip bot entries — no session
        if (!ps.scene_ready) continue;     // client not ready for broadcasts yet
        auto sid = ps.session_id;
        auto wid = ps.worker_id;
        auto* worker = workers_->GetWorker(wid);
        if (!worker) continue;

        worker->EnqueueOutbound(OutboundMessage{
            .session_id = sid,
            .room_id = Id(),
            .room_seq = next_room_seq_++,
            .buffer = buf,
        });
    }
}

void Room::TryCreateEnter(PlayerContext ctx) {
    if (ctx.session.expired()) {
        return;
    }

    GenerateMapForRoom(*this);

    float spawn_x = 0.0f;
    float spawn_z = 0.0f;
    PickSpawn(*this, spawn_x, spawn_z);

    SpawnBots(15);
    AddPlayer(ctx, spawn_x, 0.5f, spawn_z);
    EnqueueSessionEnterState(workers_, *this, ctx);

    auto skill_data = CombatSystem::BuildSkillDataPacket();
    EnqueuePacket(workers_, *this, ctx.session_id, ctx.worker_id,
                  MsgId::S_SKILL_DATA, skill_data);

    game::S_CreateRoom reply;
    reply.set_success(true);
    reply.set_zone_id(Id());
    auto* pi = reply.mutable_player();
    pi->set_player_id(ctx.player_id);
    pi->set_name(ctx.char_name);
    pi->set_hp(100);
    pi->set_max_hp(100);
    pi->set_level(ctx.level);
    auto* pos = pi->mutable_position();
    pos->set_x(spawn_x);
    pos->set_y(0.5f);
    pos->set_z(spawn_z);
    *reply.mutable_map_data() = MapData();
    EnqueuePacket(workers_, *this, ctx.session_id, ctx.worker_id,
                  MsgId::S_CREATE_ROOM, reply);
}

void Room::TryJoin(PlayerContext ctx) {
    if (ctx.session.expired()) {
        return;
    }

    if (players_.size() >= kMaxPlayers) {
        game::S_JoinRoom reply;
        reply.set_success(false);
        reply.set_error("Room is full");
        EnqueuePacket(workers_, *this, ctx.session_id, ctx.worker_id,
                      MsgId::S_JOIN_ROOM, reply);
        return;
    }

    float spawn_x = 0.0f;
    float spawn_z = 0.0f;
    PickSpawn(*this, spawn_x, spawn_z);

    AddPlayer(ctx, spawn_x, 0.5f, spawn_z);
    EnqueueSessionEnterState(workers_, *this, ctx);

    auto skill_data = CombatSystem::BuildSkillDataPacket();
    EnqueuePacket(workers_, *this, ctx.session_id, ctx.worker_id,
                  MsgId::S_SKILL_DATA, skill_data);

    game::S_JoinRoom reply;
    reply.set_success(true);
    reply.set_zone_id(Id());
    auto* pi = reply.mutable_player();
    pi->set_player_id(ctx.player_id);
    pi->set_name(ctx.char_name);
    pi->set_hp(100);
    pi->set_max_hp(100);
    pi->set_level(ctx.level);
    auto* pos = pi->mutable_position();
    pos->set_x(spawn_x);
    pos->set_y(0.5f);
    pos->set_z(spawn_z);
    *reply.mutable_map_data() = MapData();
    EnqueuePacket(workers_, *this, ctx.session_id, ctx.worker_id,
                  MsgId::S_JOIN_ROOM, reply);
}

void Room::TryPortal(PlayerContext ctx, RoomManager* room_manager,
                     std::uint32_t portal_id) {
    if (!room_manager || ctx.session.expired()) {
        return;
    }

    auto& portals = dungeon_.GetPortals();
    if (portal_id >= portals.size()) {
        return;
    }

    Room* new_room = nullptr;
    auto connection = connections_.find(portal_id);
    if (connection != connections_.end()) {
        new_room = room_manager->FindRoom(connection->second);
        if (!new_room) {
            connections_.erase(connection);
        }
    }

    if (!new_room) {
        const int new_depth = depth_ + 1;
        std::string name = "Zone_" + std::to_string(room_manager->NextId());
        new_room = room_manager->CreateRoom(name);
        if (!new_room) {
            game::S_Portal reply;
            reply.set_success(false);
            reply.set_error("Failed to create zone");
            EnqueuePacket(workers_, *this, ctx.session_id, ctx.worker_id,
                          MsgId::S_PORTAL, reply);
            return;
        }

        connections_[portal_id] = new_room->Id();
        const auto old_room_id = Id();
        const auto bot_count = std::min(10 + new_depth * 3, 30);
        new_room->Push([new_room, old_room_id, new_depth, bot_count,
                        ctx = std::move(ctx)] mutable {
            new_room->SetDepth(new_depth);
            GenerateMapForRoom(*new_room);
            new_room->SpawnBots(bot_count);

            auto& new_portals = new_room->GetDungeon().GetPortals();
            auto& new_connections = new_room->Connections();
            for (std::uint32_t i = 0; i < new_portals.size(); ++i) {
                if (!new_connections.contains(i)) {
                    new_connections[i] = old_room_id;
                    break;
                }
            }

            new_room->EnterFromPortal(std::move(ctx), old_room_id);
        });
    } else {
        const auto old_room_id = Id();
        new_room->Push([new_room, old_room_id, ctx = std::move(ctx)] mutable {
            new_room->EnterFromPortal(std::move(ctx), old_room_id);
        });
    }

    RemovePlayer(ctx.player_id);
}

void Room::EnterFromPortal(PlayerContext ctx, RoomId old_room_id) {
    if (ctx.session.expired()) {
        return;
    }

    static thread_local std::mt19937 rng{std::random_device{}()};
    float spawn_x = 0.0f;
    float spawn_z = 0.0f;
    bool found_portal_spawn = false;

    auto& portals = dungeon_.GetPortals();
    for (std::uint32_t i = 0; i < portals.size(); ++i) {
        auto connection = connections_.find(i);
        if (connection != connections_.end() && connection->second == old_room_id) {
            std::uniform_real_distribution<float> offset(-2.0f, 2.0f);
            spawn_x = portals[i].x + offset(rng);
            spawn_z = portals[i].z + offset(rng);
            found_portal_spawn = true;
            break;
        }
    }

    if (!found_portal_spawn) {
        PickSpawn(*this, spawn_x, spawn_z);
    }

    game::S_Portal reply;
    reply.set_success(true);
    reply.set_zone_id(Id());
    auto* pi = reply.mutable_player();
    pi->set_player_id(ctx.player_id);
    pi->set_name(ctx.char_name);
    pi->set_hp(100);
    pi->set_max_hp(100);
    pi->set_level(ctx.level);
    auto* pos = pi->mutable_position();
    pos->set_x(spawn_x);
    pos->set_y(0.5f);
    pos->set_z(spawn_z);
    *reply.mutable_map_data() = MapData();

    EnqueueSessionEnterState(workers_, *this, ctx);
    EnqueuePacket(workers_, *this, ctx.session_id, ctx.worker_id,
                  MsgId::S_PORTAL, reply);
    AddPlayer(ctx, spawn_x, 0.5f, spawn_z);
}

void Room::AddPlayer(const PlayerContext& ctx, float spawn_x, float spawn_y, float spawn_z) {
    // Jitter spawn to avoid stacking on the same position
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> jitter(-2.0f, 2.0f);
    float jx = spawn_x + jitter(rng);
    float jz = spawn_z + jitter(rng);
    // Validate walkable via grid tile check
    int gx = (int)(jx / DungeonGenerator::CELL_SIZE + DungeonGenerator::GRID_WIDTH / 2.0f);
    int gz = (int)(jz / DungeonGenerator::CELL_SIZE + DungeonGenerator::GRID_HEIGHT / 2.0f);
    if (dungeon_.GetTile(gx, gz) != 0) { jx = spawn_x; jz = spawn_z; }

    PlayerState ps;
    ps.player_id = ctx.player_id;
    ps.name = ctx.char_name;
    ps.level = ctx.level;
    ps.hp = 100;
    ps.max_hp = 100;
    ps.pos_x = jx;
    ps.pos_y = spawn_y;
    ps.pos_z = jz;
    ps.worker_ring = ctx.worker_ring;
    ps.session_id = ctx.session_id;
    ps.worker_id = ctx.worker_id;
    // scene_ready stays false until the client sends C_SCENE_READY.
    // While false, the player is excluded from all broadcasts and will
    // NOT receive the initial snapshot — that is sent by HandleSceneReady.

    players_[ps.player_id] = std::move(ps);
    iouring_runtime::game::Room::AddPlayer({ctx.player_id});
    ClearEmpty();
    spdlog::info("Room[{}]: player {} joined at ({:.1f},{:.1f},{:.1f}), count={}",
                 Id(), ctx.player_id, spawn_x, spawn_y, spawn_z, players_.size());
}

void Room::AddPlayer(PlayerContext* ctx, float spawn_x, float spawn_y, float spawn_z) {
    if (!ctx) {
        return;
    }
    AddPlayer(*ctx, spawn_x, spawn_y, spawn_z);
}

void Room::HandleSceneReady(PlayerId pid) {
    auto it = players_.find(pid);
    if (it == players_.end()) {
        spdlog::warn("Room[{}]: HandleSceneReady for unknown player {}", Id(), pid);
        return;
    }
    auto& ps = it->second;
    if (ps.scene_ready) {
        spdlog::debug("Room[{}]: player {} already scene_ready", Id(), pid);
        return;
    }
    ps.scene_ready = true;

    // 1. Build the initial snapshot for the newly-ready player. This
    //    includes every other entity in the room — other players and all
    //    bots — in a single S_PlayerList packet. The client's existing
    //    OnPlayerListResponse handler iterates this list and spawns each
    //    remote entity atomically.
    game::S_PlayerList list_msg;
    for (auto& [other_pid, other] : players_) {
        if (other_pid == pid) continue;  // client already knows about itself
        auto* p = list_msg.add_players();
        p->set_player_id(other.player_id);
        p->set_name(other.name);
        p->set_hp(other.hp);
        p->set_max_hp(other.max_hp);
        p->set_level(other.level);
        auto* epos = p->mutable_position();
        epos->set_x(other.pos_x);
        epos->set_y(other.pos_y);
        epos->set_z(other.pos_z);
        p->set_rotation_y(other.rotation_y);
    }
    SendTo(ps, MsgId::S_PLAYER_LIST, list_msg);

    // 2. Broadcast this player's spawn to everyone else in the room. Only
    //    players with scene_ready == true will actually receive it; bots
    //    and not-yet-ready players are skipped by BroadcastExcept.
    game::S_Spawn spawn_msg;
    auto* pi = spawn_msg.mutable_player();
    pi->set_player_id(ps.player_id);
    pi->set_name(ps.name);
    pi->set_hp(ps.hp);
    pi->set_max_hp(ps.max_hp);
    pi->set_level(ps.level);
    auto* spos = pi->mutable_position();
    spos->set_x(ps.pos_x);
    spos->set_y(ps.pos_y);
    spos->set_z(ps.pos_z);
    pi->set_rotation_y(ps.rotation_y);
    BroadcastExcept(pid, MsgId::S_SPAWN, spawn_msg);

    spdlog::debug("Room[{}]: player {} scene_ready, snapshot sent ({} entities)",
                  Id(), pid, list_msg.players_size());
}

void Room::RemovePlayer(PlayerId pid) {
    auto it = players_.find(pid);
    if (it == players_.end()) return;

    players_.erase(it);
    iouring_runtime::game::Room::RemovePlayer(pid);

    game::S_Despawn despawn_msg;
    despawn_msg.set_player_id(pid);
    BroadcastAll(MsgId::S_DESPAWN, despawn_msg);

    if (players_.empty()) MarkEmpty();
    spdlog::info("Room[{}]: player {} left, count={}", Id(), pid, players_.size());
}

void Room::OnPacket(iouring_runtime::game::PlayerState& player,
                    iouring_runtime::game::PacketId msg_id,
                    std::span<const std::byte> payload) {
    const auto pid = player.player_id;
    auto it = players_.find(pid);
    if (it == players_.end()) return;
    auto& ps = it->second;
    const auto* data = payload.data();
    const auto len = static_cast<std::uint32_t>(payload.size());

    switch (static_cast<MsgId>(msg_id)) {
        case MsgId::C_SCENE_READY:  HandleSceneReady(pid); break;
        case MsgId::C_MOVE:    handler::HandleMove(*this, ps, data, len); break;
        case MsgId::C_ATTACK:  handler::HandleAttack(*this, ps, data, len); break;
        case MsgId::C_FIRE:    handler::HandleFire(*this, ps, data, len); break;
        case MsgId::C_CHAT:         handler::HandleChat(*this, ps, data, len); break;
        case MsgId::C_CREATE_PARTY: handler::HandleCreateParty(*this, ps, data, len); break;
        case MsgId::C_JOIN_PARTY:   handler::HandleJoinParty(*this, ps, data, len); break;
        case MsgId::C_LEAVE_PARTY:  handler::HandleLeaveParty(*this, ps, data, len); break;
        case MsgId::C_PICKUP:       handler::HandlePickup(*this, ps, data, len); break;
        case MsgId::C_USE_ITEM:     handler::HandleUseItem(*this, ps, data, len); break;
        default:
            spdlog::warn("Room[{}]: unhandled packet msg_id={} from player={}",
                         Id(), msg_id, pid);
            break;
    }
}

void Room::OnTick() {
    static CombatSystem combat;
    combat.CheckRespawns(*this, std::chrono::steady_clock::now());
    projectile_manager_.Update(*this, dungeon_, 0.05f);
    bot_manager_.Update(*this, dungeon_, 0.05f);  // 50ms tick

    // Ground item lifetime decay
    for (auto it = ground_items_.begin(); it != ground_items_.end(); ) {
        it->second.lifetime -= 0.05f;
        if (it->second.lifetime <= 0) {
            game::S_GroundItemDespawn despawn;
            despawn.set_ground_id(it->first);
            BroadcastAll(MsgId::S_GROUND_ITEM_DESPAWN, despawn);
            it = ground_items_.erase(it);
        } else {
            ++it;
        }
    }

    // Scoreboard broadcast every 5s
    if (++scoreboardCounter_ >= kScoreboardTicks) {
        scoreboardCounter_ = 0;
        game::S_Scoreboard sb;
        for (auto& [pid, ps] : players_) {
            auto* e = sb.add_entries();
            e->set_player_id(pid);
            e->set_name(ps.name);
            e->set_kills(ps.kills);
            e->set_deaths(ps.deaths);
            e->set_level(ps.level);
        }
        BroadcastAll(MsgId::S_SCOREBOARD, sb);
    }
}

void Room::ScheduleTick(iouring_runtime::core::job::JobTimer& timer) {
    timer_ = &timer;
    timer.Reserve(kTickInterval, weak_from_this(), [this] {
        OnTick();
        if (timer_)
            ScheduleTick(*timer_);
    });
}

void Room::SpawnGroundItem(int32_t item_def_id, float x, float y, float z,
                            const std::string& label) {
    uint64_t gid = nextGroundId_++;
    GroundItem gi;
    gi.ground_id = gid;
    gi.item_def_id = item_def_id;
    gi.x = x; gi.y = y; gi.z = z;
    gi.label = label;
    ground_items_[gid] = gi;

    game::S_GroundItemSpawn msg;
    msg.set_ground_id(gid);
    msg.set_item_def_id(item_def_id);
    auto* pos = msg.mutable_position();
    pos->set_x(x); pos->set_y(y); pos->set_z(z);
    msg.set_label(label);
    BroadcastAll(MsgId::S_GROUND_ITEM_SPAWN, msg);
}

bool Room::TryPickup(PlayerId pid, uint64_t ground_id) {
    auto it = ground_items_.find(ground_id);
    if (it == ground_items_.end()) return false;

    auto pit = players_.find(pid);
    if (pit == players_.end()) return false;

    auto& ps = pit->second;
    auto& gi = it->second;
    float dx = ps.pos_x - gi.x;
    float dz = ps.pos_z - gi.z;
    if (dx * dx + dz * dz > 9.0f) return false;  // 3m radius

    // Add to player inventory via S_ITEM_ADD
    static thread_local std::mt19937 rng{std::random_device{}()};
    game::S_ItemAdd item_msg;
    auto* item = item_msg.mutable_item();
    item->set_instance_id(static_cast<int64_t>(rng()));
    item->set_item_def_id(gi.item_def_id);
    item->set_slot(static_cast<int32_t>(rng() % 100));
    item->set_quantity(1);
    item->set_durability(100);
    SendTo(ps, MsgId::S_ITEM_ADD, item_msg);

    // Despawn ground item for all
    game::S_GroundItemDespawn despawn;
    despawn.set_ground_id(ground_id);
    BroadcastAll(MsgId::S_GROUND_ITEM_DESPAWN, despawn);

    ground_items_.erase(it);
    return true;
}
