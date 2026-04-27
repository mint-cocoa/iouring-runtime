#pragma once

#include "../types.h"
#include "Game.pb.h"
#include <array>
#include <string>

class Room;

struct SkillData {
    int32_t skill_id;
    std::string name;
    int32_t damage;
    float range;
    uint32_t cooldown_ms;
    bool is_self_target;
};

class CombatSystem {
public:
    static constexpr std::array<SkillData, 4> kSkills = {{
        {0, "Basic Attack",  10,  2.0f,  500, false},
        {1, "Power Strike",  25,  2.5f, 2000, false},
        {2, "Fireball",      40,  8.0f, 3000, false},
        {3, "Heal",         -30,  0.0f, 5000, true},
    }};

    static game::S_SkillData BuildSkillDataPacket();

    bool ProcessAttack(Room& room, PlayerId attacker, PlayerId target,
                       int32_t skill_id, float fire_x, float fire_y, float fire_z,
                       float dir_x, float dir_y, float dir_z);

    bool ProcessProjectileHit(Room& room, PlayerId attacker, PlayerId target, int32_t skill_id);

    void CheckRespawns(Room& room, TimePoint now);
};
