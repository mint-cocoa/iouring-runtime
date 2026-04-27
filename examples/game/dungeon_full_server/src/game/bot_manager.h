#pragma once

#include "../types.h"
#include <string>
#include <unordered_map>
#include <random>
#include <vector>

class Room;
class DungeonGenerator;

struct GridCoord {
    int x = 0, y = 0;
};

struct BotState {
    PlayerId bot_id = 0;
    std::string name;
    float pos_x = 0, pos_y = 0.5f, pos_z = 0;
    float target_x = 0, target_z = 0;
    float vel_x = 0, vel_y = 0, vel_z = 0;
    float rotation_y = 0;
    int32_t hp = 100;
    int32_t max_hp = 100;
    bool is_dead = false;
    bool has_target = false;
    PlayerId chase_target = 0;  // player being chased (0 = none)
    TimePoint last_attack;
    TimePoint death_time;
    float speed = 3.0f;
    std::vector<GridCoord> path;
    int path_idx = 0;

    // Multi-skill cooldowns
    std::unordered_map<int32_t, TimePoint> skill_cooldowns;

    // Aggro tracking
    float aggro_origin_x = 0, aggro_origin_z = 0;
    TimePoint chase_start_time;
    bool is_retreating = false;
};

class BotManager {
public:
    void SpawnBots(Room& room, const DungeonGenerator& dungeon, int count);
    void Update(Room& room, const DungeonGenerator& dungeon, float dt);

private:
    void PickNewWaypoint(BotState& bot, const DungeonGenerator& dungeon);
    void UpdateMovement(BotState& bot, const DungeonGenerator& dungeon, float dt);
    void UpdateCombat(BotState& bot, Room& room, const DungeonGenerator& dungeon);
    void BroadcastBotMove(BotState& bot, Room& room);
    void CheckRespawns(Room& room, const DungeonGenerator& dungeon);
    static std::vector<GridCoord> AStarSearch(
        const DungeonGenerator& dungeon,
        int sx, int sy, int gx, int gy);
    static void WorldToGrid(float wx, float wz, int& gx, int& gy);

    std::unordered_map<PlayerId, BotState> bots_;
    PlayerId next_bot_id_ = 900000;
    std::mt19937 rng_{std::random_device{}()};
};
