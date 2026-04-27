#include "projectile_manager.h"
#include "room.h"
#include "dungeon_generator.h"
#include "../system/combat_system.h"
#include <cmath>
#include <spdlog/spdlog.h>

static constexpr float kHitRadius = 1.5f;

void ProjectileManager::SpawnProjectile(PlayerId owner, float px, float py, float pz,
                                         float dx, float dy, float dz, int32_t skill_id) {
    float len = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (len < 0.001f) return;
    dx /= len; dy /= len; dz /= len;

    Projectile p;
    p.id = next_id_++;
    p.owner_id = owner;
    p.pos_x = px; p.pos_y = py; p.pos_z = pz;
    p.dir_x = dx; p.dir_y = dy; p.dir_z = dz;
    p.skill_id = skill_id;
    projectiles_.push_back(p);
}

bool ProjectileManager::CheckWallCollision(const DungeonGenerator& dungeon,
                                            float old_x, float old_z,
                                            float new_x, float new_z) const {
    float dx = new_x - old_x;
    float dz = new_z - old_z;
    float dist = std::sqrt(dx*dx + dz*dz);
    int steps = static_cast<int>(dist / 0.5f) + 1;

    for (int i = 1; i <= steps; ++i) {
        float t = static_cast<float>(i) / steps;
        float sx = old_x + dx * t;
        float sz = old_z + dz * t;

        int gx = static_cast<int>(std::round(
            sx / DungeonGenerator::CELL_SIZE + DungeonGenerator::GRID_WIDTH / 2.0f));
        int gy = static_cast<int>(std::round(
            sz / DungeonGenerator::CELL_SIZE + DungeonGenerator::GRID_HEIGHT / 2.0f));

        if (dungeon.GetTile(gx, gy) != 0) return true;
    }
    return false;
}

void ProjectileManager::Update(Room& room, const DungeonGenerator& dungeon, float dt) {
    static CombatSystem combat;

    for (int i = static_cast<int>(projectiles_.size()) - 1; i >= 0; --i) {
        auto& p = projectiles_[i];

        float old_x = p.pos_x, old_z = p.pos_z;
        float move = p.speed * dt;
        p.pos_x += p.dir_x * move;
        p.pos_y += p.dir_y * move;
        p.pos_z += p.dir_z * move;
        p.traveled += move;

        if (p.traveled > p.max_distance) {
            projectiles_.erase(projectiles_.begin() + i);
            continue;
        }

        if (CheckWallCollision(dungeon, old_x, old_z, p.pos_x, p.pos_z)) {
            projectiles_.erase(projectiles_.begin() + i);
            continue;
        }

        bool hit = false;
        for (auto& [pid, ps] : room.Players()) {
            if (pid == p.owner_id || ps.is_dead) continue;

            float ddx = ps.pos_x - p.pos_x;
            float ddz = ps.pos_z - p.pos_z;
            float dist = std::sqrt(ddx*ddx + ddz*ddz);

            if (dist < kHitRadius) {
                combat.ProcessProjectileHit(room, p.owner_id, pid, p.skill_id);
                hit = true;
                break;
            }
        }

        if (hit) {
            projectiles_.erase(projectiles_.begin() + i);
        }
    }
}
