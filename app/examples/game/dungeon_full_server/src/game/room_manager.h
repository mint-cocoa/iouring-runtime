#pragma once

#include "../types.h"
#include "room.h"
#include <iouring_runtime/game/RoomManager.h>
#include <vector>

class IoWorkerPool;

class RoomManager {
public:
    RoomManager(iouring_runtime::core::job::GlobalQueue& gq, IoWorkerPool* workers,
                iouring_runtime::core::job::JobTimer& timer);

    Room* CreateRoom(const std::string& name);
    Room* FindRoom(RoomId id);
    void  RemoveRoom(RoomId id);
    void  CleanupEmptyRooms();
    RoomId NextId() const { return rooms_.NextRoomId(); }

    struct RoomInfo {
        RoomId id;
        std::string name;
        std::uint32_t player_count;
    };
    std::vector<RoomInfo> GetRoomList();

private:
    iouring_runtime::game::RoomManager rooms_;
    iouring_runtime::core::job::JobTimer& timer_;
    IoWorkerPool* workers_;
};
