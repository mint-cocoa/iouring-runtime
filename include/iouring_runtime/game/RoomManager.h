#pragma once

#include <iouring_runtime/core/GlobalQueue.h>
#include <iouring_runtime/game/Room.h>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iouring_runtime::game {

class RoomManager {
public:
    struct RoomInfo {
        RoomId id = 0;
        std::string name;
        std::uint32_t player_count = 0;
        std::uint32_t max_players = 0;
    };

    explicit RoomManager(core::job::GlobalQueue& global_queue);

    std::shared_ptr<Room> CreateRoom(std::string name);

    template<typename RoomT, typename... Args>
    std::shared_ptr<RoomT> CreateRoomAs(std::string name, Args&&... args) {
        static_assert(std::is_base_of_v<Room, RoomT>,
                      "RoomT must derive from iouring_runtime::game::Room");

        std::unique_lock lock(mutex_);
        const auto room_id = NextRoomIdLocked();
        auto room = std::make_shared<RoomT>(room_id, std::move(name),
                                            global_queue_,
                                            std::forward<Args>(args)...);
        rooms_.emplace(room_id, room);
        return room;
    }

    bool AddRoom(std::shared_ptr<Room> room);
    std::shared_ptr<Room> FindRoom(RoomId room_id) const;
    bool RemoveRoom(RoomId room_id);
    std::vector<std::shared_ptr<Room>> Rooms() const;
    std::vector<RoomInfo> GetRoomList() const;
    std::uint32_t RoomCount() const;
    RoomId NextRoomId() const;

private:
    RoomId NextRoomIdLocked();

    core::job::GlobalQueue& global_queue_;
    mutable std::shared_mutex mutex_;
    RoomId next_room_id_ = 1;
    std::unordered_map<RoomId, std::shared_ptr<Room>> rooms_;
};

} // namespace iouring_runtime::game
