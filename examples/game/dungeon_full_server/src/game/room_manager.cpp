#include "room_manager.h"
#include <spdlog/spdlog.h>
#include <unordered_set>

RoomManager::RoomManager(iouring_runtime::core::job::GlobalQueue& gq, IoWorkerPool* workers,
                         iouring_runtime::core::job::JobTimer& timer)
    : global_queue_(gq), timer_(timer), workers_(workers) {}

Room* RoomManager::CreateRoom(const std::string& name) {
    std::unique_lock lock(mutex_);
    auto id = next_id_++;
    auto room = std::make_shared<Room>(id, name, global_queue_, workers_);
    room->ScheduleTick(timer_);
    auto* ptr = room.get();
    rooms_[id] = std::move(room);
    spdlog::info("RoomManager: created room id={} name={}", id, name);
    return ptr;
}

Room* RoomManager::FindRoom(RoomId id) {
    std::shared_lock lock(mutex_);
    auto it = rooms_.find(id);
    return it != rooms_.end() ? it->second.get() : nullptr;
}

void RoomManager::RemoveRoom(RoomId id) {
    std::unique_lock lock(mutex_);
    rooms_.erase(id);
    spdlog::info("RoomManager: removed room id={}", id);
}

void RoomManager::CleanupEmptyRooms() {
    std::unique_lock lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    constexpr auto kGracePeriod = std::chrono::seconds(30);

    // Build set of rooms referenced by other rooms' connections
    std::unordered_set<RoomId> referenced;
    for (auto& [id, room] : rooms_) {
        for (auto& [portal, target] : room->Connections()) {
            referenced.insert(target);
        }
    }

    // Collect rooms that are: empty 30s+, not root (id>1), not the only reference holder
    std::vector<RoomId> to_remove;
    for (auto& [id, room] : rooms_) {
        if (id <= 1) continue;               // keep lobby-created rooms
        if (!room->IsEmpty()) continue;       // has players

        auto empty_since = room->EmptySince();
        if (empty_since == TimePoint{}) continue;
        if (now - empty_since < kGracePeriod) continue;

        to_remove.push_back(id);
    }

    for (auto rid : to_remove) {
        // Remove all connections pointing to this room from other rooms
        for (auto& [id, room] : rooms_) {
            auto& conns = room->Connections();
            for (auto cit = conns.begin(); cit != conns.end(); ) {
                if (cit->second == rid) cit = conns.erase(cit);
                else ++cit;
            }
        }
        spdlog::info("RoomManager: cleaning up empty room id={} (empty 30s+)", rid);
        rooms_.erase(rid);
    }
}

std::vector<RoomManager::RoomInfo> RoomManager::GetRoomList() {
    std::shared_lock lock(mutex_);
    std::vector<RoomInfo> list;
    list.reserve(rooms_.size());
    for (auto& [id, room] : rooms_) {
        if (id > 1 && room->Name().find("Zone_") == 0) continue;  // hide portal-created zones
        list.push_back({id, room->Name(), room->PlayerCount()});
    }
    return list;
}
