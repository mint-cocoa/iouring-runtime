#pragma once

#include "../types.h"
#include <vector>
#include <cstdint>

class Room;
class DungeonGenerator;

struct Projectile {
    uint32_t id;
    PlayerId owner_id;
    float pos_x, pos_y, pos_z;
    float dir_x, dir_y, dir_z;
    float speed = 80.0f;
    float max_distance = 50.0f;
    float traveled = 0.0f;
    int32_t skill_id = 0;
};

class ProjectileManager {
public:
    void SpawnProjectile(PlayerId owner, float px, float py, float pz,
                         float dx, float dy, float dz, int32_t skill_id = 0);
    void Update(Room& room, const DungeonGenerator& dungeon, float dt);

private:
    bool CheckWallCollision(const DungeonGenerator& dungeon,
                            float old_x, float old_z, float new_x, float new_z) const;
    std::vector<Projectile> projectiles_;
    uint32_t next_id_ = 1;
};
