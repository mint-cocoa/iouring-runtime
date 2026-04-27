#pragma once

#include "../types.h"
#include "Common.pb.h"
#include <cstdint>
#include <string>
#include <vector>
#include <random>

class DungeonGenerator {
public:
    static constexpr int GRID_WIDTH = 120;
    static constexpr int GRID_HEIGHT = 70;
    static constexpr float CELL_SIZE = 1.0f;
    static constexpr int RANDOM_FILL_PERCENT = 52;
    static constexpr int SMOOTH_ITERATIONS = 5;
    static constexpr int WALL_THRESHOLD_SIZE = 15;
    static constexpr int ROOM_THRESHOLD_SIZE = 20;
    static constexpr int PASSAGE_RADIUS = 1;

    void Generate(const std::string& seed, int numPortals = 3, int depth = 0);
    void FillMapData(game::MapData& mapData) const;
    bool GetRandomFloorPosition(std::mt19937& rng, float& wx, float& wz) const;

    struct PortalData {
        float x, z;
        uint32_t target_zone;  // 0 = auto-create
        std::string target_name;
    };
    const std::vector<PortalData>& GetPortals() const { return portals_; }

    int GetTile(int gx, int gy) const {
        if (gx < 0 || gx >= GRID_WIDTH || gy < 0 || gy >= GRID_HEIGHT) return 1;
        return grid_[gx][gy];
    }
    void GridToWorldPublic(int gx, int gy, float& x, float& z) const {
        GridToWorld(gx, gy, x, z);
    }

private:
    struct Coord {
        int tileX = 0;
        int tileY = 0;
        Coord() = default;
        Coord(int x, int y) : tileX(x), tileY(y) {}
    };

    struct Room {
        std::vector<Coord> tiles;
        std::vector<Coord> edgeTiles;
        std::vector<int> connectedRoomIndices;
        int roomSize = 0;
        bool isAccessibleFromMainRoom = false;
        bool isMainRoom = false;
    };

    struct PropData {
        float x, z;
        int type;
        float rotY;
    };

    struct LightData {
        float x, z;
        float r, g, b;
        float intensity;
        float range;
        bool softShadow;
    };

    void RandomFillMap(std::mt19937& rng);
    void SmoothMap();
    int GetSurroundingWallCount(int gridX, int gridY) const;
    bool IsInMapRange(int x, int y) const;

    void ProcessMap(std::mt19937& rng);
    std::vector<std::vector<Coord>> GetRegions(int tileType) const;
    std::vector<Coord> GetRegionTiles(int startX, int startY) const;

    void ConnectClosestRooms(bool forceAccessibilityFromMainRoom = false);
    void CreatePassage(int roomIdxA, int roomIdxB, Coord tileA, Coord tileB);
    void DrawCircle(Coord c, int r);
    std::vector<Coord> GetLine(Coord from, Coord to) const;

    void PlaceProps(std::mt19937& rng);
    void PlaceLights(std::mt19937& rng);
    void FindSpawnPosition();
    void PlacePortals(int numPortals);

    void GridToWorld(int gx, int gy, float& x, float& z) const;

    int grid_[GRID_WIDTH][GRID_HEIGHT]{};
    std::vector<Room> survivingRooms_;
    std::vector<PropData> props_;
    std::vector<LightData> lights_;
    std::vector<PortalData> portals_;
    float spawnX_ = 0.f;
    float spawnY_ = 0.5f;
    float spawnZ_ = 0.f;
    int depth_ = 0;
};
