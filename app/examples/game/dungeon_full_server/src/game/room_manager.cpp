#include "room_manager.h"
#include <spdlog/spdlog.h>
#include <unordered_set>

RoomManager::RoomManager(iouring_runtime::core::job::GlobalQueue& gq, IoWorkerPool* workers,
                         iouring_runtime::core::job::JobTimer& timer)
    : rooms_(gq), timer_(timer), workers_(workers) {}

Room* RoomManager::CreateRoom(const std::string& name) {
    auto room = rooms_.CreateRoomAs<Room>(name, workers_);
    room->ScheduleTick(timer_);
    auto* ptr = room.get();
    spdlog::info("RoomManager: created room id={} name={}", ptr->Id(), name);
    return ptr;
}

Room* RoomManager::FindRoom(RoomId id) {
    auto room = std::dynamic_pointer_cast<Room>(rooms_.FindRoom(id));
    return room ? room.get() : nullptr;
}

void RoomManager::RemoveRoom(RoomId id) {
    rooms_.RemoveRoom(id);
    spdlog::info("RoomManager: removed room id={}", id);
}

void RoomManager::CleanupEmptyRooms() {
    auto rooms = rooms_.Rooms();
    auto now = std::chrono::steady_clock::now();
    constexpr auto kGracePeriod = std::chrono::seconds(30);

    // Build set of rooms referenced by other rooms' connections
    std::unordered_set<RoomId> referenced;
    for (auto& base_room : rooms) {
        auto room = std::dynamic_pointer_cast<Room>(base_room);
        if (!room) continue;
        for (auto& [portal, target] : room->Connections()) {
            referenced.insert(target);
        }
    }

    // Collect rooms that are: empty 30s+, not root (id>1), not the only reference holder
    std::vector<RoomId> to_remove;
    for (auto& base_room : rooms) {
        auto room = std::dynamic_pointer_cast<Room>(base_room);
        if (!room) continue;
        auto id = room->Id();
        if (id <= 1) continue;               // keep lobby-created rooms
        if (!room->IsEmpty()) continue;       // has players

        auto empty_since = room->EmptySince();
        if (empty_since == TimePoint{}) continue;
        if (now - empty_since < kGracePeriod) continue;

        to_remove.push_back(id);
    }

    for (auto rid : to_remove) {
        // Remove all connections pointing to this room from other rooms
        for (auto& base_room : rooms) {
            auto room = std::dynamic_pointer_cast<Room>(base_room);
            if (!room) continue;
            auto& conns = room->Connections();
            for (auto cit = conns.begin(); cit != conns.end(); ) {
                if (cit->second == rid) cit = conns.erase(cit);
                else ++cit;
            }
        }
        spdlog::info("RoomManager: cleaning up empty room id={} (empty 30s+)", rid);
        rooms_.RemoveRoom(rid);
    }
}

std::vector<RoomManager::RoomInfo> RoomManager::GetRoomList() {
    auto rooms = rooms_.Rooms();
    std::vector<RoomInfo> list;
    list.reserve(rooms.size());
    for (auto& base_room : rooms) {
        auto room = std::dynamic_pointer_cast<Room>(base_room);
        if (!room) continue;
        auto id = room->Id();
        if (id > 1 && room->Name().find("Zone_") == 0) continue;  // hide portal-created zones
        list.push_back({id, room->Name(), room->PlayerCount()});
    }
    return list;
}
