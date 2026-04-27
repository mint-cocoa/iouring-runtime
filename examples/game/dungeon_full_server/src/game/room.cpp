#include "room.h"
#include "../net/io_worker_pool.h"
#include "../net/io_worker.h"
#include "../handler/game_handler.h"
#include "../handler/social_handler.h"
#include "../system/combat_system.h"
#include <spdlog/spdlog.h>

#include "Game.pb.h"
#include "Common.pb.h"
#include "Inventory.pb.h"

Room::Room(RoomId id, const std::string& name,
           iouring_runtime::core::job::GlobalQueue& gq,
           IoWorkerPool* workers)
    : JobQueue(gq)
    , id_(id)
    , name_(name)
    , workers_(workers)
{
}

iouring_runtime::core::buffer::BufferPool& Room::GetPool() {
    return workers_->GetWorker(0)->Pool();
}

void Room::SendTo(PlayerState& ps, MsgId msg_id,
                  iouring_runtime::core::buffer::SendBufferRef buf) {
    if (!buf || !ps.worker_ring) return;  // bots have no session
    auto sid = ps.session_id;
    auto wid = ps.worker_id;
    auto* worker = workers_->GetWorker(wid);
    if (!worker) return;

    worker->Ring()->RunOnRing([worker, sid, buf = std::move(buf)] {
        auto* sess = worker->FindSession(sid);
        if (sess) sess->Send(std::move(buf));
    });
}

void Room::BroadcastAll(MsgId msg_id, iouring_runtime::core::buffer::SendBufferRef buf) {
    if (!buf) return;
    for (auto& [_, ps] : players_) {
        if (ps.session_id == 0) continue;   // bot — no session to send to
        if (!ps.scene_ready) continue;      // client not ready for broadcasts yet
        auto sid = ps.session_id;
        auto wid = ps.worker_id;
        auto* worker = workers_->GetWorker(wid);
        if (!worker) continue;

        worker->Ring()->RunOnRing([worker, sid, buf] {
            auto* sess = worker->FindSession(sid);
            if (sess) sess->Send(buf);
        });
    }
}

void Room::BroadcastExcept(PlayerId exclude, MsgId msg_id,
                           iouring_runtime::core::buffer::SendBufferRef buf) {
    if (!buf) return;
    for (auto& [pid, ps] : players_) {
        if (pid == exclude) continue;
        if (ps.session_id == 0) continue;  // skip bot entries — no session
        if (!ps.scene_ready) continue;     // client not ready for broadcasts yet
        auto sid = ps.session_id;
        auto wid = ps.worker_id;
        auto* worker = workers_->GetWorker(wid);
        if (!worker) continue;

        worker->Ring()->RunOnRing([worker, sid, buf] {
            auto* sess = worker->FindSession(sid);
            if (sess) sess->Send(buf);
        });
    }
}

void Room::AddPlayer(PlayerContext* ctx, float spawn_x, float spawn_y, float spawn_z) {
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
    ps.player_id = ctx->player_id;
    ps.name = ctx->char_name;
    ps.level = ctx->level;
    ps.hp = 100;
    ps.max_hp = 100;
    ps.pos_x = jx;
    ps.pos_y = spawn_y;
    ps.pos_z = jz;
    ps.worker_ring = ctx->worker_ring;
    ps.session_id = ctx->session_id;
    ps.worker_id = ctx->worker_id;
    // scene_ready stays false until the client sends C_SCENE_READY.
    // While false, the player is excluded from all broadcasts and will
    // NOT receive the initial snapshot — that is sent by HandleSceneReady.

    players_[ps.player_id] = std::move(ps);
    ClearEmpty();
    spdlog::info("Room[{}]: player {} joined at ({:.1f},{:.1f},{:.1f}), count={}",
                 id_, ctx->player_id, spawn_x, spawn_y, spawn_z, players_.size());
}

void Room::HandleSceneReady(PlayerId pid) {
    auto it = players_.find(pid);
    if (it == players_.end()) {
        spdlog::warn("Room[{}]: HandleSceneReady for unknown player {}", id_, pid);
        return;
    }
    auto& ps = it->second;
    if (ps.scene_ready) {
        spdlog::debug("Room[{}]: player {} already scene_ready", id_, pid);
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
                  id_, pid, list_msg.players_size());
}

void Room::RemovePlayer(PlayerId pid) {
    auto it = players_.find(pid);
    if (it == players_.end()) return;

    players_.erase(it);

    game::S_Despawn despawn_msg;
    despawn_msg.set_player_id(pid);
    BroadcastAll(MsgId::S_DESPAWN, despawn_msg);

    if (players_.empty()) MarkEmpty();
    spdlog::info("Room[{}]: player {} left, count={}", id_, pid, players_.size());
}

void Room::HandlePacket(PlayerId pid, std::uint16_t msg_id,
                        const std::byte* data, std::uint32_t len) {
    auto it = players_.find(pid);
    if (it == players_.end()) return;
    auto& ps = it->second;

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
            spdlog::warn("Room[{}]: unhandled packet msg_id={} from player={}", id_, msg_id, pid);
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
