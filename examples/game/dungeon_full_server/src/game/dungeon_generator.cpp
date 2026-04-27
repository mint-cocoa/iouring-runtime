#include "dungeon_generator.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <queue>
#include <unordered_set>

void DungeonGenerator::Generate(const std::string& seed, int numPortals, int depth) {
    survivingRooms_.clear();
    props_.clear();
    lights_.clear();
    portals_.clear();
    depth_ = depth;
    std::memset(grid_, 0, sizeof(grid_));

    auto hashVal = std::hash<std::string>{}(seed);
    std::mt19937 rng(static_cast<unsigned>(hashVal));

    RandomFillMap(rng);

    for (int i = 0; i < SMOOTH_ITERATIONS; i++)
        SmoothMap();

    ProcessMap(rng);
    PlaceProps(rng);
    PlaceLights(rng);
    FindSpawnPosition();
    PlacePortals(numPortals);
}

void DungeonGenerator::FillMapData(game::MapData& mapData) const {
    mapData.set_grid_width(GRID_WIDTH);
    mapData.set_grid_height(GRID_HEIGHT);
    mapData.set_cell_size(CELL_SIZE);

    std::string gridBytes(GRID_WIDTH * GRID_HEIGHT, '\0');
    for (int gx = 0; gx < GRID_WIDTH; gx++)
        for (int gy = 0; gy < GRID_HEIGHT; gy++)
            gridBytes[gx * GRID_HEIGHT + gy] = static_cast<char>(grid_[gx][gy]);
    mapData.set_grid(gridBytes);

    for (auto& p : props_) {
        auto* mp = mapData.add_props();
        mp->set_x(p.x);
        mp->set_z(p.z);
        mp->set_prop_type(p.type);
        mp->set_rotation_y(p.rotY);
    }

    for (auto& l : lights_) {
        auto* ml = mapData.add_lights();
        ml->set_x(l.x);
        ml->set_z(l.z);
        ml->set_r(l.r);
        ml->set_g(l.g);
        ml->set_b(l.b);
        ml->set_intensity(l.intensity);
        ml->set_range(l.range);
        ml->set_soft_shadow(l.softShadow);
    }

    auto* sp = mapData.mutable_spawn_position();
    sp->set_x(spawnX_);
    sp->set_y(spawnY_);
    sp->set_z(spawnZ_);

    for (size_t i = 0; i < portals_.size(); ++i) {
        auto* pi = mapData.add_portals();
        pi->set_portal_id(static_cast<uint32_t>(i));
        pi->set_x(portals_[i].x);
        pi->set_z(portals_[i].z);
        pi->set_target_zone(portals_[i].target_zone);
        pi->set_target_name(portals_[i].target_name);
    }
}

// === Cellular Automata ===

void DungeonGenerator::RandomFillMap(std::mt19937& rng) {
    // Deeper zones → higher fill → more complex/dense caves
    int fillPct = RANDOM_FILL_PERCENT + std::min(depth_ * 2, 10);  // 45 → 47 → 49 → ...55 max
    std::uniform_int_distribution<int> dist(0, 99);
    for (int x = 0; x < GRID_WIDTH; x++) {
        for (int y = 0; y < GRID_HEIGHT; y++) {
            if (x == 0 || x == GRID_WIDTH - 1 || y == 0 || y == GRID_HEIGHT - 1)
                grid_[x][y] = 1;
            else
                grid_[x][y] = (dist(rng) < fillPct) ? 1 : 0;
        }
    }
}

void DungeonGenerator::SmoothMap() {
    int newMap[GRID_WIDTH][GRID_HEIGHT];
    for (int x = 0; x < GRID_WIDTH; x++) {
        for (int y = 0; y < GRID_HEIGHT; y++) {
            int neighbours = GetSurroundingWallCount(x, y);
            if (neighbours > 4)
                newMap[x][y] = 1;
            else if (neighbours < 4)
                newMap[x][y] = 0;
            else
                newMap[x][y] = grid_[x][y];
        }
    }
    std::memcpy(grid_, newMap, sizeof(grid_));
}

int DungeonGenerator::GetSurroundingWallCount(int gridX, int gridY) const {
    int wallCount = 0;
    for (int nx = gridX - 1; nx <= gridX + 1; nx++) {
        for (int ny = gridY - 1; ny <= gridY + 1; ny++) {
            if (IsInMapRange(nx, ny)) {
                if (nx != gridX || ny != gridY)
                    wallCount += grid_[nx][ny];
            } else {
                wallCount++;
            }
        }
    }
    return wallCount;
}

bool DungeonGenerator::IsInMapRange(int x, int y) const {
    return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
}

// === Region Processing ===

void DungeonGenerator::ProcessMap(std::mt19937& rng) {
    auto wallRegions = GetRegions(1);
    for (auto& region : wallRegions) {
        if (static_cast<int>(region.size()) < WALL_THRESHOLD_SIZE) {
            for (auto& tile : region)
                grid_[tile.tileX][tile.tileY] = 0;
        }
    }

    auto roomRegions = GetRegions(0);
    survivingRooms_.clear();

    for (auto& region : roomRegions) {
        if (static_cast<int>(region.size()) < ROOM_THRESHOLD_SIZE) {
            for (auto& tile : region)
                grid_[tile.tileX][tile.tileY] = 1;
        } else {
            Room room;
            room.tiles = region;
            room.roomSize = static_cast<int>(region.size());

            for (auto& tile : region) {
                for (int x = tile.tileX - 1; x <= tile.tileX + 1; x++) {
                    for (int y = tile.tileY - 1; y <= tile.tileY + 1; y++) {
                        if (x == tile.tileX || y == tile.tileY) {
                            if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT
                                && grid_[x][y] == 1) {
                                room.edgeTiles.push_back(tile);
                            }
                        }
                    }
                }
            }
            survivingRooms_.push_back(std::move(room));
        }
    }

    if (survivingRooms_.empty()) return;

    std::sort(survivingRooms_.begin(), survivingRooms_.end(),
        [](const Room& a, const Room& b) { return a.roomSize > b.roomSize; });

    survivingRooms_[0].isMainRoom = true;
    survivingRooms_[0].isAccessibleFromMainRoom = true;

    ConnectClosestRooms(false);
}

void DungeonGenerator::ConnectClosestRooms(bool forceAccessibilityFromMainRoom) {
    std::vector<int> roomListA, roomListB;

    if (forceAccessibilityFromMainRoom) {
        for (int i = 0; i < static_cast<int>(survivingRooms_.size()); i++) {
            if (survivingRooms_[i].isAccessibleFromMainRoom)
                roomListB.push_back(i);
            else
                roomListA.push_back(i);
        }
    } else {
        for (int i = 0; i < static_cast<int>(survivingRooms_.size()); i++) {
            roomListA.push_back(i);
            roomListB.push_back(i);
        }
    }

    int bestDistance = 0;
    Coord bestTileA, bestTileB;
    int bestRoomA = -1, bestRoomB = -1;
    bool possibleConnectionFound = false;

    for (int idxA : roomListA) {
        auto& roomA = survivingRooms_[idxA];

        if (!forceAccessibilityFromMainRoom) {
            possibleConnectionFound = false;
            if (!roomA.connectedRoomIndices.empty())
                continue;
        }

        for (int idxB : roomListB) {
            if (idxA == idxB) continue;
            auto& roomB = survivingRooms_[idxB];

            bool connected = false;
            for (int ci : roomA.connectedRoomIndices) {
                if (ci == idxB) { connected = true; break; }
            }
            if (connected) continue;

            for (size_t tA = 0; tA < roomA.edgeTiles.size(); tA++) {
                for (size_t tB = 0; tB < roomB.edgeTiles.size(); tB++) {
                    auto& tileA = roomA.edgeTiles[tA];
                    auto& tileB = roomB.edgeTiles[tB];
                    int dx = tileA.tileX - tileB.tileX;
                    int dy = tileA.tileY - tileB.tileY;
                    int dist = dx * dx + dy * dy;

                    if (dist < bestDistance || !possibleConnectionFound) {
                        bestDistance = dist;
                        possibleConnectionFound = true;
                        bestTileA = tileA;
                        bestTileB = tileB;
                        bestRoomA = idxA;
                        bestRoomB = idxB;
                    }
                }
            }
        }

        if (possibleConnectionFound && !forceAccessibilityFromMainRoom)
            CreatePassage(bestRoomA, bestRoomB, bestTileA, bestTileB);
    }

    if (possibleConnectionFound && forceAccessibilityFromMainRoom) {
        CreatePassage(bestRoomA, bestRoomB, bestTileA, bestTileB);
        ConnectClosestRooms(true);
    }

    if (!forceAccessibilityFromMainRoom)
        ConnectClosestRooms(true);
}

void DungeonGenerator::CreatePassage(int roomIdxA, int roomIdxB, Coord tileA, Coord tileB) {
    survivingRooms_[roomIdxA].connectedRoomIndices.push_back(roomIdxB);
    survivingRooms_[roomIdxB].connectedRoomIndices.push_back(roomIdxA);

    auto setAccessible = [this](int idx, auto& self) -> void {
        if (!survivingRooms_[idx].isAccessibleFromMainRoom) {
            survivingRooms_[idx].isAccessibleFromMainRoom = true;
            for (int ci : survivingRooms_[idx].connectedRoomIndices)
                self(ci, self);
        }
    };

    if (survivingRooms_[roomIdxA].isAccessibleFromMainRoom)
        setAccessible(roomIdxB, setAccessible);
    else if (survivingRooms_[roomIdxB].isAccessibleFromMainRoom)
        setAccessible(roomIdxA, setAccessible);

    auto line = GetLine(tileA, tileB);
    for (auto& c : line)
        DrawCircle(c, PASSAGE_RADIUS);
}

void DungeonGenerator::DrawCircle(Coord c, int r) {
    for (int x = -r; x <= r; x++) {
        for (int y = -r; y <= r; y++) {
            if (x * x + y * y <= r * r) {
                int drawX = c.tileX + x;
                int drawY = c.tileY + y;
                if (IsInMapRange(drawX, drawY))
                    grid_[drawX][drawY] = 0;
            }
        }
    }
}

std::vector<DungeonGenerator::Coord> DungeonGenerator::GetLine(Coord from, Coord to) const {
    std::vector<Coord> line;
    int x = from.tileX;
    int y = from.tileY;
    int dx = to.tileX - from.tileX;
    int dy = to.tileY - from.tileY;

    bool inverted = false;
    int step = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
    int gradientStep = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);
    int longest = std::abs(dx);
    int shortest = std::abs(dy);

    if (longest < shortest) {
        inverted = true;
        longest = std::abs(dy);
        shortest = std::abs(dx);
        step = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);
        gradientStep = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
    }

    int gradientAccumulation = longest / 2;
    for (int i = 0; i < longest; i++) {
        line.emplace_back(x, y);
        if (inverted) y += step; else x += step;
        gradientAccumulation += shortest;
        if (gradientAccumulation >= longest) {
            if (inverted) x += gradientStep; else y += gradientStep;
            gradientAccumulation -= longest;
        }
    }
    return line;
}

std::vector<std::vector<DungeonGenerator::Coord>> DungeonGenerator::GetRegions(int tileType) const {
    std::vector<std::vector<Coord>> regions;
    std::vector<std::vector<bool>> mapFlags(GRID_WIDTH, std::vector<bool>(GRID_HEIGHT, false));

    for (int x = 0; x < GRID_WIDTH; x++) {
        for (int y = 0; y < GRID_HEIGHT; y++) {
            if (!mapFlags[x][y] && grid_[x][y] == tileType) {
                auto region = GetRegionTiles(x, y);
                for (auto& tile : region)
                    mapFlags[tile.tileX][tile.tileY] = true;
                regions.push_back(std::move(region));
            }
        }
    }
    return regions;
}

std::vector<DungeonGenerator::Coord> DungeonGenerator::GetRegionTiles(int startX, int startY) const {
    std::vector<Coord> tiles;
    std::vector<std::vector<bool>> mapFlags(GRID_WIDTH, std::vector<bool>(GRID_HEIGHT, false));
    int tileType = grid_[startX][startY];

    std::queue<Coord> q;
    q.push(Coord(startX, startY));
    mapFlags[startX][startY] = true;

    while (!q.empty()) {
        Coord tile = q.front();
        q.pop();
        tiles.push_back(tile);

        for (int x = tile.tileX - 1; x <= tile.tileX + 1; x++) {
            for (int y = tile.tileY - 1; y <= tile.tileY + 1; y++) {
                if (IsInMapRange(x, y) && (y == tile.tileY || x == tile.tileX)) {
                    if (!mapFlags[x][y] && grid_[x][y] == tileType) {
                        mapFlags[x][y] = true;
                        q.push(Coord(x, y));
                    }
                }
            }
        }
    }
    return tiles;
}

// === Props & Lights ===

void DungeonGenerator::PlaceProps(std::mt19937& rng) {
    std::uniform_int_distribution<int> propTypeDist(0, 4);
    std::uniform_int_distribution<int> rotDist(0, 3);
    std::uniform_real_distribution<float> chanceDist(0.f, 1.f);

    for (auto& room : survivingRooms_) {
        int propCount = std::clamp(room.roomSize / 12, 2, 15);
        std::unordered_set<int> usedTiles;
        std::uniform_int_distribution<int> tileDist(0, static_cast<int>(room.tiles.size()) - 1);

        for (int p = 0; p < propCount; p++) {
            Coord tile;
            int attempts = 0;
            do {
                tile = room.tiles[tileDist(rng)];
                attempts++;
            } while (usedTiles.count(tile.tileX * 1000 + tile.tileY) && attempts < 20);

            usedTiles.insert(tile.tileX * 1000 + tile.tileY);
            float wx, wz;
            GridToWorld(tile.tileX, tile.tileY, wx, wz);
            props_.push_back({wx, wz, propTypeDist(rng), static_cast<float>(rotDist(rng)) * 90.f});
        }
    }

    for (int gx = 0; gx < GRID_WIDTH; gx++) {
        for (int gy = 0; gy < GRID_HEIGHT; gy++) {
            if (grid_[gx][gy] != 0) continue;
            int floorNeighbours = 0;
            for (int dx = -1; dx <= 1; dx++) {
                for (int dy = -1; dy <= 1; dy++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = gx + dx, ny = gy + dy;
                    if (IsInMapRange(nx, ny) && grid_[nx][ny] == 0) floorNeighbours++;
                }
            }
            if (floorNeighbours <= 3 && chanceDist(rng) < 0.04f) {
                float wx, wz;
                GridToWorld(gx, gy, wx, wz);
                std::uniform_real_distribution<float> rotFullDist(0.f, 360.f);
                props_.push_back({wx, wz, propTypeDist(rng), rotFullDist(rng)});
            }
        }
    }
}

void DungeonGenerator::PlaceLights(std::mt19937& rng) {
    // Deeper zones → dimmer, cooler lights
    float dimFactor = std::max(0.4f, 1.0f - depth_ * 0.1f);  // 1.0 → 0.9 → 0.8 → ...
    float rBase = 0.8f * dimFactor, gBase = 0.6f * dimFactor, bBase = 0.4f + depth_ * 0.05f;
    std::uniform_real_distribution<float> rDist(rBase, std::min(rBase + 0.2f, 1.0f));
    std::uniform_real_distribution<float> gDist(gBase, std::min(gBase + 0.3f, 1.0f));
    std::uniform_real_distribution<float> bDist(bBase, std::min(bBase + 0.3f, 1.0f));

    for (auto& room : survivingRooms_) {
        float roomRadius = std::sqrt(static_cast<float>(room.roomSize)) * CELL_SIZE * 0.5f;
        int lightCount = std::clamp(room.roomSize / 40, 1, 5);
        int stride = std::max(1, static_cast<int>(room.tiles.size()) / lightCount);

        for (int li = 0; li < lightCount; li++) {
            int idx = std::min(li * stride + stride / 2, static_cast<int>(room.tiles.size()) - 1);
            auto& tile = room.tiles[idx];
            float lx = (tile.tileX - GRID_WIDTH / 2.f) * CELL_SIZE;
            float lz = (tile.tileY - GRID_HEIGHT / 2.f) * CELL_SIZE;
            lights_.push_back({lx, lz, rDist(rng), gDist(rng), bDist(rng),
                              20.f, std::max(roomRadius * 1.2f, 7.f), (li == 0)});
        }
    }

    std::unordered_set<int> roomTileSet;
    for (auto& room : survivingRooms_)
        for (auto& tile : room.tiles)
            roomTileSet.insert(tile.tileX * 10000 + tile.tileY);

    for (int gx = 0; gx < GRID_WIDTH; gx += 3) {
        for (int gy = 0; gy < GRID_HEIGHT; gy += 3) {
            if (grid_[gx][gy] != 0) continue;
            if (roomTileSet.count(gx * 10000 + gy)) continue;
            float wx, wz;
            GridToWorld(gx, gy, wx, wz);
            lights_.push_back({wx, wz, 0.8f, 0.65f, 0.4f, 12.f, CELL_SIZE * 5.f, false});
        }
    }
}

bool DungeonGenerator::GetRandomFloorPosition(std::mt19937& rng, float& wx, float& wz) const {
    // Collect open tiles (at least 4 floor neighbours in 8-dir)
    std::vector<Coord> openTiles;
    for (auto& r : survivingRooms_) {
        for (auto& t : r.tiles) {
            int floorN = 0;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    if ((dx || dy) && IsInMapRange(t.tileX+dx, t.tileY+dy)
                        && grid_[t.tileX+dx][t.tileY+dy] == 0) floorN++;
            if (floorN >= 5) openTiles.push_back(t);
        }
    }
    if (openTiles.empty()) {
        // Fallback: any floor tile
        for (auto& r : survivingRooms_)
            for (auto& t : r.tiles) openTiles.push_back(t);
    }
    if (openTiles.empty()) return false;

    std::uniform_int_distribution<int> pick(0, static_cast<int>(openTiles.size()) - 1);
    auto& tile = openTiles[pick(rng)];
    GridToWorld(tile.tileX, tile.tileY, wx, wz);
    return true;
}

void DungeonGenerator::FindSpawnPosition() {
    if (survivingRooms_.empty()) {
        spawnX_ = 0.f; spawnY_ = 0.5f; spawnZ_ = 0.f;
        return;
    }

    // Pick the tile in the main room with the most floor neighbours (most open space)
    auto& mainRoom = survivingRooms_[0];
    int bestScore = -1;
    Coord bestTile = mainRoom.tiles[0];

    for (auto& tile : mainRoom.tiles) {
        int score = 0;
        for (int dx = -2; dx <= 2; ++dx) {
            for (int dy = -2; dy <= 2; ++dy) {
                int nx = tile.tileX + dx, ny = tile.tileY + dy;
                if (IsInMapRange(nx, ny) && grid_[nx][ny] == 0) score++;
            }
        }
        if (score > bestScore) {
            bestScore = score;
            bestTile = tile;
        }
    }

    GridToWorld(bestTile.tileX, bestTile.tileY, spawnX_, spawnZ_);
    spawnY_ = 0.5f;
}

void DungeonGenerator::GridToWorld(int gx, int gy, float& x, float& z) const {
    x = (gx - GRID_WIDTH / 2.f) * CELL_SIZE;
    z = (gy - GRID_HEIGHT / 2.f) * CELL_SIZE;
}

void DungeonGenerator::PlacePortals(int numPortals) {
    // Collect open floor tiles (5+ floor neighbours in 8-dir) with spawn distance
    struct Candidate { float dist, wx, wz; int openness; };
    std::vector<Candidate> candidates;
    for (int gx = 2; gx < GRID_WIDTH - 2; ++gx) {
        for (int gy = 2; gy < GRID_HEIGHT - 2; ++gy) {
            if (grid_[gx][gy] != 0) continue;
            // Count floor neighbours in 2-ring
            int open = 0;
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -2; dy <= 2; ++dy)
                    if (IsInMapRange(gx+dx, gy+dy) && grid_[gx+dx][gy+dy] == 0) open++;
            if (open < 15) continue;  // need at least 15/25 floor in 5x5 area

            float wx, wz;
            GridToWorld(gx, gy, wx, wz);
            float dx = wx - spawnX_, dz = wz - spawnZ_;
            candidates.push_back({dx*dx + dz*dz, wx, wz, open});
        }
    }

    // Sort by distance descending — pick from far, spread out
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.dist > b.dist; });

    // Separation scales with map size to ensure good spread
    float mapDiag = (float)(GRID_WIDTH * GRID_WIDTH + GRID_HEIGHT * GRID_HEIGHT) * CELL_SIZE * CELL_SIZE;
    float kMinSeparation = mapDiag * 0.08f;  // ~8% of map diagonal squared

    int placed = 0;
    for (const auto& c : candidates) {
        if (placed >= numPortals) break;
        if (c.dist < 36.0f) break;  // at least 6m from spawn

        // Check separation from spawn and other portals
        bool tooClose = false;
        for (const auto& p : portals_) {
            float dx = p.x - c.wx, dz = p.z - c.wz;
            if (dx*dx + dz*dz < kMinSeparation) { tooClose = true; break; }
        }
        if (tooClose) continue;

        char name[32];
        snprintf(name, sizeof(name), "Portal %c", 'A' + placed);
        portals_.push_back({c.wx, c.wz, 0, name});  // target_zone 0 = unlinked
        placed++;
    }
}
