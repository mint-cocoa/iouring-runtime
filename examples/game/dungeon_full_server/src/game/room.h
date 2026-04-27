#pragma once

#include "../types.h"
#include "player_context.h"
#include "bot_manager.h"
#include "projectile_manager.h"
#include "dungeon_generator.h"
#include "Common.pb.h"
#include <iouring_runtime/core/JobQueue.h>
#include <iouring_runtime/core/JobTimer.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/Concepts.h>
#include "../net/packet_builder.h"
#include <unordered_map>
#include <string>

class IoWorkerPool;

struct PlayerState {
    PlayerId player_id = 0;
    std::string name;
    int32_t hp = 100;
    int32_t max_hp = 100;
    int32_t level = 1;
    float pos_x = 0, pos_y = 0, pos_z = 0;
    float rotation_y = 0;
    float vel_x = 0, vel_y = 0, vel_z = 0;
    int32_t move_state = 0;

    iouring_runtime::core::ring::IoRing* worker_ring = nullptr;
    iouring_runtime::core::SessionId session_id = 0;
    iouring_runtime::core::ContextId worker_id = 0;

    // Server-Initiated Flow Control: set to true only after the client
    // has loaded the game scene, registered its packet handlers, and sent
    // C_SCENE_READY. While false, the player is excluded from all room
    // broadcasts — preventing bursts of spawn/move packets from arriving
    // before the client can consume them. Bots always have this flag set
    // to true (they don't need a snapshot), but they also have
    // session_id == 0 which causes them to be skipped for a different
    // reason (no session to send to).
    bool scene_ready = false;

    int64_t gold = 0;
    PartyId party_id = 0;

    std::unordered_map<int32_t, TimePoint> skill_cooldowns;
    bool is_dead = false;
    TimePoint death_time;
    int32_t kills = 0;
    int32_t deaths = 0;
};

struct GroundItem {
    uint64_t ground_id = 0;
    int32_t item_def_id = 0;
    float x = 0, y = 0, z = 0;
    std::string label;
    float lifetime = 15.0f;  // 15초 후 소멸
};

class Room : public iouring_runtime::core::job::JobQueue {
public:
    Room(RoomId id, const std::string& name,
         iouring_runtime::core::job::GlobalQueue& gq,
         IoWorkerPool* workers);

    RoomId             Id()   const { return id_; }
    const std::string& Name() const { return name_; }
    std::uint32_t      PlayerCount() const { return static_cast<std::uint32_t>(players_.size()); }
    bool               IsEmpty() const { return players_.empty(); }
    bool               IsFull() const { return players_.size() >= kMaxPlayers; }
    TimePoint          EmptySince() const { return empty_since_; }
    void               MarkEmpty() { empty_since_ = std::chrono::steady_clock::now(); }
    void               ClearEmpty() { empty_since_ = TimePoint{}; }

    void AddPlayer(PlayerContext* ctx, float spawn_x = 0, float spawn_y = 0.5f, float spawn_z = 0);
    void RemovePlayer(PlayerId pid);

    // Server-Initiated Flow Control: called when the client signals that
    // its game scene is fully loaded and its packet handlers are ready.
    // This marks the player's scene_ready flag, sends the initial room
    // snapshot (all other entities) to the newly-ready player, and
    // broadcasts their spawn to the rest of the room.
    void HandleSceneReady(PlayerId pid);

    void HandlePacket(PlayerId pid, std::uint16_t msg_id,
                      const std::byte* data, std::uint32_t len);

    void OnTick();
    void ScheduleTick(iouring_runtime::core::job::JobTimer& timer);

    // 송신 헬퍼
    void SendTo(PlayerState& ps, MsgId msg_id,
                iouring_runtime::core::buffer::SendBufferRef buf);
    void BroadcastAll(MsgId msg_id, iouring_runtime::core::buffer::SendBufferRef buf);
    void BroadcastExcept(PlayerId exclude, MsgId msg_id,
                         iouring_runtime::core::buffer::SendBufferRef buf);

    template<iouring_runtime::core::ProtobufMessage T>
    void SendTo(PlayerState& ps, MsgId msg_id, const T& proto) {
        SendTo(ps, msg_id, PacketBuilder::Build(GetPool(), msg_id, proto));
    }
    template<iouring_runtime::core::ProtobufMessage T>
    void BroadcastAll(MsgId msg_id, const T& proto) {
        BroadcastAll(msg_id, PacketBuilder::Build(GetPool(), msg_id, proto));
    }
    template<iouring_runtime::core::ProtobufMessage T>
    void BroadcastExcept(PlayerId exclude, MsgId msg_id, const T& proto) {
        BroadcastExcept(exclude, msg_id, PacketBuilder::Build(GetPool(), msg_id, proto));
    }

    iouring_runtime::core::buffer::BufferPool& GetPool();

    std::unordered_map<PlayerId, PlayerState>& Players() { return players_; }

    const game::MapData& MapData() const { return map_data_; }
    void SetMapData(game::MapData&& md) { map_data_ = std::move(md); }
    const game::Vec3& SpawnPosition() const { return map_data_.spawn_position(); }

    DungeonGenerator& GetDungeon() { return dungeon_; }
    const DungeonGenerator& GetDungeon() const { return dungeon_; }

    void SpawnBots(int count) { bot_manager_.SpawnBots(*this, dungeon_, count); }
    ProjectileManager& Projectiles() { return projectile_manager_; }
    int Depth() const { return depth_; }
    void SetDepth(int d) { depth_ = d; }

    // Graph connections: portal_index → target RoomId
    std::unordered_map<uint32_t, RoomId>& Connections() { return connections_; }
    const std::unordered_map<uint32_t, RoomId>& Connections() const { return connections_; }
    bool IsReferencedBy(RoomId other_room_id) const;  // any portal points here?

    // Ground items (loot)
    void SpawnGroundItem(int32_t item_def_id, float x, float y, float z,
                         const std::string& label);
    bool TryPickup(PlayerId pid, uint64_t ground_id);
    std::unordered_map<uint64_t, GroundItem>& GroundItems() { return ground_items_; }

private:
    static constexpr std::uint32_t kMaxPlayers = 500;
    static constexpr auto kTickInterval = std::chrono::milliseconds(50);
    static constexpr int kScoreboardTicks = 100;  // 100 * 50ms = 5s
    int scoreboardCounter_ = 0;

    RoomId id_;
    std::string name_;
    std::unordered_map<PlayerId, PlayerState> players_;
    IoWorkerPool* workers_;
    iouring_runtime::core::job::JobTimer* timer_ = nullptr;
    game::MapData map_data_;
    BotManager bot_manager_;
    ProjectileManager projectile_manager_;
    DungeonGenerator dungeon_;
    std::unordered_map<uint64_t, GroundItem> ground_items_;
    uint64_t nextGroundId_ = 1;
    std::unordered_map<uint32_t, RoomId> connections_;
    TimePoint empty_since_{};
    int depth_ = 0;
};
