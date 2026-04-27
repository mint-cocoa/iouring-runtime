#include "bot_manager.h"
#include "room.h"
#include "dungeon_generator.h"
#include "../system/combat_system.h"
#include "Game.pb.h"
#include "Common.pb.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <format>
#include <queue>
#include <limits>

static constexpr float kBotSpeed = 3.0f;
static constexpr float kChaseRange = 8.0f;   // start chasing enemy within this range
static constexpr float kAttackRange = 2.0f;   // basic attack range
static constexpr int   kAttackCooldownMs = 500;
static constexpr int   kRespawnSeconds = 5;
static constexpr float kAggroResetDist = 15.0f;
static constexpr int   kAggroResetSeconds = 10;
static constexpr float kRetreatHpRatio = 0.4f;
static constexpr float kHealHpRatio = 0.3f;
static constexpr float kFireballRange = 8.0f;

// Check if a world position is walkable (not a wall)
static bool IsWalkable(const DungeonGenerator& dungeon, float wx, float wz) {
    // Reverse GridToWorld: world -> grid
    // GridToWorld: x = (gx - GRID_WIDTH/2) * CELL_SIZE, z = (gy - GRID_HEIGHT/2) * CELL_SIZE
    int gx = static_cast<int>(std::round(wx / DungeonGenerator::CELL_SIZE + DungeonGenerator::GRID_WIDTH / 2.0f));
    int gy = static_cast<int>(std::round(wz / DungeonGenerator::CELL_SIZE + DungeonGenerator::GRID_HEIGHT / 2.0f));
    return dungeon.GetTile(gx, gy) == 0;
}

void BotManager::WorldToGrid(float wx, float wz, int& gx, int& gy) {
    gx = static_cast<int>(std::round(
        wx / DungeonGenerator::CELL_SIZE + DungeonGenerator::GRID_WIDTH  / 2.0f));
    gy = static_cast<int>(std::round(
        wz / DungeonGenerator::CELL_SIZE + DungeonGenerator::GRID_HEIGHT / 2.0f));
}

std::vector<GridCoord> BotManager::AStarSearch(
    const DungeonGenerator& dungeon,
    int sx, int sy, int gx, int gy)
{
    constexpr int W = DungeonGenerator::GRID_WIDTH;
    constexpr int H = DungeonGenerator::GRID_HEIGHT;

    if (sx < 0 || sx >= W || sy < 0 || sy >= H) return {};
    if (gx < 0 || gx >= W || gy < 0 || gy >= H) return {};
    if (dungeon.GetTile(gx, gy) != 0) return {};
    if (sx == gx && sy == gy) return {};

    auto idx = [W](int x, int y) { return y * W + x; };

    std::vector<float> g(W * H, std::numeric_limits<float>::max());
    std::vector<int>   parent(W * H, -1);

    struct Node {
        int x, y;
        float f;
        bool operator>(const Node& o) const { return f > o.f; }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    g[idx(sx, sy)] = 0;
    open.push({sx, sy, static_cast<float>(std::abs(sx-gx) + std::abs(sy-gy))});

    constexpr int dx[] = {0, 0, 1, -1};
    constexpr int dy[] = {1, -1, 0, 0};

    while (!open.empty()) {
        auto [cx, cy, cf] = open.top(); open.pop();

        if (cx == gx && cy == gy) break;
        if (cf - static_cast<float>(std::abs(cx-gx) + std::abs(cy-gy)) > g[idx(cx, cy)] + 0.001f)
            continue;  // stale node

        for (int i = 0; i < 4; ++i) {
            int nx = cx + dx[i];
            int ny = cy + dy[i];
            if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
            if (dungeon.GetTile(nx, ny) != 0) continue;
            float ng = g[idx(cx, cy)] + 1.0f;
            if (ng < g[idx(nx, ny)]) {
                g[idx(nx, ny)] = ng;
                parent[idx(nx, ny)] = idx(cx, cy);
                float h = static_cast<float>(std::abs(nx-gx) + std::abs(ny-gy));
                open.push({nx, ny, ng + h});
            }
        }
    }

    if (g[idx(gx, gy)] == std::numeric_limits<float>::max()) return {};

    std::vector<GridCoord> path;
    for (int cur = idx(gx, gy); cur != idx(sx, sy); cur = parent[cur]) {
        if (cur == -1) return {};
        path.push_back({cur % W, cur / W});
    }
    std::reverse(path.begin(), path.end());
    return path;
}

void BotManager::SpawnBots(Room& room, const DungeonGenerator& dungeon, int count) {
    const auto& spawn = room.SpawnPosition();
    float base_x = spawn.x();
    float base_z = spawn.z();

    std::uniform_real_distribution<float> offset(-3.0f, 3.0f);

    // Count real sessions present before spawning. In the normal room-creation
    // path bots are spawned before any player joins, so this is 0 and no
    // broadcast is needed — players will pick up the bots via the
    // HandleSceneReady snapshot. The batch broadcast below only matters for
    // "mid-game" reinforcement spawns where real sessions are already
    // present, and it sends a single S_PlayerList rather than N individual
    // S_Spawn packets.
    int session_count_before = 0;
    for (auto& [_, ps] : room.Players()) {
        if (ps.session_id != 0) ++session_count_before;
    }

    game::S_PlayerList batch;

    for (int i = 0; i < count; ++i) {
        PlayerId id = next_bot_id_++;
        BotState bot;
        bot.bot_id = id;
        bot.name = std::format("Bot_{}", id);
        bot.pos_x = base_x + offset(rng_);
        bot.pos_y = 0.5f;
        bot.pos_z = base_z + offset(rng_);
        bot.target_x = bot.pos_x;
        bot.target_z = bot.pos_z;
        bot.hp = 100;
        bot.max_hp = 100;
        bot.rotation_y = 0;

        PlayerState ps;
        ps.player_id = id;
        ps.name = bot.name;
        ps.hp = bot.hp;
        ps.max_hp = bot.max_hp;
        ps.level = 1;
        ps.pos_x = bot.pos_x;
        ps.pos_y = bot.pos_y;
        ps.pos_z = bot.pos_z;
        ps.worker_ring = nullptr;
        ps.session_id = 0;
        ps.worker_id = 0;
        room.Players()[id] = std::move(ps);

        // Append this bot to the batch (no per-bot broadcast).
        auto* pi = batch.add_players();
        pi->set_player_id(id);
        pi->set_name(bot.name);
        pi->set_hp(bot.hp);
        pi->set_max_hp(bot.max_hp);
        pi->set_level(1);
        auto* pos = pi->mutable_position();
        pos->set_x(bot.pos_x);
        pos->set_y(bot.pos_y);
        pos->set_z(bot.pos_z);
        pi->set_rotation_y(bot.rotation_y);

        bots_[id] = std::move(bot);
    }

    // Single broadcast to all real sessions (if any).
    if (batch.players_size() > 0 && session_count_before > 0) {
        room.BroadcastAll(MsgId::S_PLAYER_LIST, batch);
    }
    spdlog::info("Room: spawned {} bots", count);
}

void BotManager::PickNewWaypoint(BotState& bot, const DungeonGenerator& dungeon) {
    std::uniform_int_distribution<int> dist_x(0, DungeonGenerator::GRID_WIDTH - 1);
    std::uniform_int_distribution<int> dist_y(0, DungeonGenerator::GRID_HEIGHT - 1);

    // Pick a nearby walkable tile (prefer short distances for natural movement)
    for (int attempt = 0; attempt < 50; ++attempt) {
        int gx = dist_x(rng_);
        int gy = dist_y(rng_);
        if (dungeon.GetTile(gx, gy) == 0) {
            float wx, wz;
            dungeon.GridToWorldPublic(gx, gy, wx, wz);

            // Skip if too far (prefer nearby waypoints for natural patrol)
            float dx = wx - bot.pos_x;
            float dz = wz - bot.pos_z;
            if (dx * dx + dz * dz > 15.0f * 15.0f) continue;

            bot.target_x = wx;
            bot.target_z = wz;
            bot.has_target = true;
            bot.chase_target = 0;
            return;
        }
    }
}

void BotManager::UpdateMovement(BotState& bot, const DungeonGenerator& dungeon, float dt) {
    if (!bot.has_target && bot.chase_target == 0) return;

    // 경로 소진 시 A* 재계산
    if (bot.path.empty() || bot.path_idx >= static_cast<int>(bot.path.size())) {
        int sgx, sgy, tgx, tgy;
        WorldToGrid(bot.pos_x, bot.pos_z, sgx, sgy);
        WorldToGrid(bot.target_x, bot.target_z, tgx, tgy);

        bot.path = AStarSearch(dungeon, sgx, sgy, tgx, tgy);
        bot.path_idx = 0;

        if (bot.path.empty()) {
            bot.has_target = false;
            bot.vel_x = 0;
            bot.vel_z = 0;
            return;
        }
    }

    // 다음 경로 노드를 향해 이동
    float node_wx, node_wz;
    const auto& node = bot.path[bot.path_idx];
    dungeon.GridToWorldPublic(node.x, node.y, node_wx, node_wz);

    float dx = node_wx - bot.pos_x;
    float dz = node_wz - bot.pos_z;
    float dist = std::sqrt(dx * dx + dz * dz);

    if (dist < 0.3f) {
        ++bot.path_idx;
        if (bot.path_idx >= static_cast<int>(bot.path.size())) {
            bot.has_target = false;
            bot.vel_x = 0;
            bot.vel_z = 0;
        }
        return;
    }

    float nx = dx / dist;
    float nz = dz / dist;
    bot.vel_x = nx * bot.speed;
    bot.vel_z = nz * bot.speed;
    bot.pos_x += bot.vel_x * dt;
    bot.pos_z += bot.vel_z * dt;
    bot.rotation_y = std::atan2(nx, nz) * (180.0f / 3.14159265f);
}

void BotManager::UpdateCombat(BotState& bot, Room& room, const DungeonGenerator& dungeon) {
    auto now = std::chrono::steady_clock::now();

    // Try heal if HP low
    if (bot.hp < static_cast<int32_t>(bot.max_hp * kHealHpRatio)) {
        auto heal_cd = bot.skill_cooldowns.find(3);
        if (heal_cd == bot.skill_cooldowns.end() || now >= heal_cd->second) {
            static CombatSystem combat;
            combat.ProcessAttack(room, bot.bot_id, bot.bot_id, 3,
                                 bot.pos_x, bot.pos_y, bot.pos_z, 0, 0, 0);
            bot.skill_cooldowns[3] = now + std::chrono::milliseconds(5000);
            auto it = room.Players().find(bot.bot_id);
            if (it != room.Players().end()) bot.hp = it->second.hp;
        }
    }

    // Find nearest player (not other bots)
    PlayerId nearest_id = 0;
    float nearest_dist = kChaseRange;
    float target_x = 0, target_z = 0;

    for (auto& [pid, ps] : room.Players()) {
        if (pid == bot.bot_id || ps.is_dead) continue;
        float dx = ps.pos_x - bot.pos_x;
        float dz = ps.pos_z - bot.pos_z;
        float dist = std::sqrt(dx * dx + dz * dz);
        if (dist < nearest_dist) {
            nearest_dist = dist;
            nearest_id = pid;
            target_x = ps.pos_x;
            target_z = ps.pos_z;
        }
    }

    if (nearest_id == 0) {
        bot.chase_target = 0;
        bot.is_retreating = false;
        return;
    }

    // Aggro reset check
    if (bot.chase_target != 0) {
        float dx = bot.pos_x - bot.aggro_origin_x;
        float dz = bot.pos_z - bot.aggro_origin_z;
        float from_origin = std::sqrt(dx*dx + dz*dz);
        auto chase_secs = std::chrono::duration_cast<std::chrono::seconds>(
            now - bot.chase_start_time).count();
        if (from_origin > kAggroResetDist || chase_secs > kAggroResetSeconds) {
            bot.chase_target = 0;
            bot.has_target = false;
            bot.is_retreating = false;
            return;
        }
    }

    // Face the target
    float dir_x = target_x - bot.pos_x;
    float dir_z = target_z - bot.pos_z;
    float len = std::sqrt(dir_x * dir_x + dir_z * dir_z);
    if (len > 0.0f) { dir_x /= len; dir_z /= len; }
    bot.rotation_y = std::atan2(dir_x, dir_z) * (180.0f / 3.14159265f);

    // Retreat: HP low and enemy close
    if (bot.hp < static_cast<int32_t>(bot.max_hp * kRetreatHpRatio) && nearest_dist < 3.0f) {
        bot.is_retreating = true;
        bot.target_x = bot.pos_x - dir_x * 8.0f;
        bot.target_z = bot.pos_z - dir_z * 8.0f;
        bot.has_target = true;
        bot.chase_target = 0;

        // Fireball while retreating
        auto fb_cd = bot.skill_cooldowns.find(2);
        if (fb_cd == bot.skill_cooldowns.end() || now >= fb_cd->second) {
            room.Projectiles().SpawnProjectile(
                bot.bot_id, bot.pos_x, bot.pos_y, bot.pos_z,
                dir_x, 0.0f, dir_z, 2);
            bot.skill_cooldowns[2] = now + std::chrono::milliseconds(3000);

            game::S_Fire fire_msg;
            fire_msg.set_player_id(bot.bot_id);
            auto* fp = fire_msg.mutable_fire_pos();
            fp->set_x(bot.pos_x); fp->set_y(bot.pos_y); fp->set_z(bot.pos_z);
            auto* fd = fire_msg.mutable_fire_dir();
            fd->set_x(dir_x); fd->set_y(0); fd->set_z(dir_z);
            room.BroadcastAll(MsgId::S_FIRE, fire_msg);
        }
        return;
    }

    bot.is_retreating = false;

    if (nearest_dist <= kAttackRange) {
        // Melee range
        bot.vel_x = 0;
        bot.vel_z = 0;
        bot.has_target = false;

        // Try Power Strike first
        int use_skill = 0;
        auto ps_cd = bot.skill_cooldowns.find(1);
        if (ps_cd == bot.skill_cooldowns.end() || now >= ps_cd->second) {
            use_skill = 1;
        }

        auto cd = bot.skill_cooldowns.find(use_skill);
        if (cd == bot.skill_cooldowns.end() || now >= cd->second) {
            static CombatSystem combat;
            combat.ProcessAttack(room, bot.bot_id, nearest_id, use_skill,
                                 bot.pos_x, bot.pos_y, bot.pos_z,
                                 dir_x, 0.0f, dir_z);
            int cd_ms = (use_skill == 1) ? 2000 : 500;
            bot.skill_cooldowns[use_skill] = now + std::chrono::milliseconds(cd_ms);
        }
    } else if (nearest_dist <= kFireballRange) {
        // Ranged: fire projectile
        auto fb_cd = bot.skill_cooldowns.find(2);
        if (fb_cd == bot.skill_cooldowns.end() || now >= fb_cd->second) {
            room.Projectiles().SpawnProjectile(
                bot.bot_id, bot.pos_x, bot.pos_y, bot.pos_z,
                dir_x, 0.0f, dir_z, 2);
            bot.skill_cooldowns[2] = now + std::chrono::milliseconds(3000);

            game::S_Fire fire_msg;
            fire_msg.set_player_id(bot.bot_id);
            auto* fp = fire_msg.mutable_fire_pos();
            fp->set_x(bot.pos_x); fp->set_y(bot.pos_y); fp->set_z(bot.pos_z);
            auto* fd = fire_msg.mutable_fire_dir();
            fd->set_x(dir_x); fd->set_y(0); fd->set_z(dir_z);
            room.BroadcastAll(MsgId::S_FIRE, fire_msg);
        }

        // Chase closer
        if (bot.chase_target == 0) {
            bot.aggro_origin_x = bot.pos_x;
            bot.aggro_origin_z = bot.pos_z;
            bot.chase_start_time = now;
        }
        bot.chase_target = nearest_id;
        bot.target_x = target_x;
        bot.target_z = target_z;
        bot.has_target = true;
    } else {
        // Start chasing
        if (bot.chase_target == 0) {
            bot.aggro_origin_x = bot.pos_x;
            bot.aggro_origin_z = bot.pos_z;
            bot.chase_start_time = now;
        }
        bot.chase_target = nearest_id;
        bot.target_x = target_x;
        bot.target_z = target_z;
        bot.has_target = true;
    }
}

void BotManager::BroadcastBotMove(BotState& bot, Room& room) {
    auto it = room.Players().find(bot.bot_id);
    if (it == room.Players().end()) return;

    auto& ps = it->second;
    ps.pos_x = bot.pos_x;
    ps.pos_y = bot.pos_y;
    ps.pos_z = bot.pos_z;
    ps.vel_x = bot.vel_x;
    ps.vel_y = 0;
    ps.vel_z = bot.vel_z;
    ps.rotation_y = bot.rotation_y;

    game::S_Move move_msg;
    move_msg.set_player_id(bot.bot_id);
    auto* pos = move_msg.mutable_position();
    pos->set_x(bot.pos_x);
    pos->set_y(bot.pos_y);
    pos->set_z(bot.pos_z);
    auto* vel = move_msg.mutable_velocity();
    vel->set_x(bot.vel_x);
    vel->set_y(0);
    vel->set_z(bot.vel_z);
    move_msg.set_rotation_y(bot.rotation_y);
    int state = 0;  // idle
    if (bot.is_retreating || bot.chase_target != 0) state = 2;  // run
    else if (bot.vel_x != 0 || bot.vel_z != 0) state = 1;  // walk (patrol)
    move_msg.set_state(state);

    room.BroadcastAll(MsgId::S_MOVE, move_msg);
}

void BotManager::CheckRespawns(Room& room, const DungeonGenerator& dungeon) {
    auto now = std::chrono::steady_clock::now();

    for (auto& [id, bot] : bots_) {
        if (!bot.is_dead) continue;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - bot.death_time).count();
        if (elapsed < kRespawnSeconds) continue;

        float rx = 0, rz = 0;
        if (!dungeon.GetRandomFloorPosition(rng_, rx, rz)) {
            auto& spawn = room.SpawnPosition();
            rx = spawn.x(); rz = spawn.z();
        }

        bot.is_dead = false;
        bot.hp = bot.max_hp;
        bot.pos_x = rx;
        bot.pos_y = 0.5f;
        bot.pos_z = rz;
        bot.has_target = false;
        bot.chase_target = 0;
        bot.vel_x = 0;
        bot.vel_z = 0;

        auto it = room.Players().find(id);
        if (it != room.Players().end()) {
            auto& ps = it->second;
            ps.hp = bot.hp;
            ps.is_dead = false;
            ps.pos_x = bot.pos_x;
            ps.pos_y = bot.pos_y;
            ps.pos_z = bot.pos_z;
        }

        game::S_Respawn respawn_msg;
        auto* pi = respawn_msg.mutable_player();
        pi->set_player_id(id);
        pi->set_name(bot.name);
        pi->set_hp(bot.hp);
        pi->set_max_hp(bot.max_hp);
        pi->set_level(1);
        auto* pos = pi->mutable_position();
        pos->set_x(bot.pos_x);
        pos->set_y(bot.pos_y);
        pos->set_z(bot.pos_z);
        room.BroadcastAll(MsgId::S_RESPAWN, respawn_msg);

        spdlog::info("Bot {} respawned", id);
    }
}

void BotManager::Update(Room& room, const DungeonGenerator& dungeon, float dt) {
    // Sync death state from room
    for (auto& [id, bot] : bots_) {
        auto it = room.Players().find(id);
        if (it != room.Players().end()) {
            auto& ps = it->second;
            if (ps.is_dead && !bot.is_dead) {
                bot.is_dead = true;
                bot.death_time = ps.death_time;
                bot.hp = 0;
                bot.vel_x = 0;
                bot.vel_z = 0;
                bot.has_target = false;
                bot.chase_target = 0;
                bot.path.clear();   // 추가
                bot.path_idx = 0;   // 추가
            }
        }
    }

    for (auto& [id, bot] : bots_) {
        if (bot.is_dead) continue;

        // Combat first (may override movement)
        UpdateCombat(bot, room, dungeon);

        // Movement (chase or patrol)
        if (!bot.has_target && bot.chase_target == 0) {
            PickNewWaypoint(bot, dungeon);
        }
        UpdateMovement(bot, dungeon, dt);

        BroadcastBotMove(bot, room);
    }

    CheckRespawns(room, dungeon);
}
