#include <iouring_runtime/game/RoomManager.h>

#include <utility>

namespace iouring_runtime::game {

RoomManager::RoomManager(core::job::GlobalQueue& global_queue)
    : global_queue_(global_queue) {}

std::shared_ptr<Room> RoomManager::CreateRoom(std::string name) {
    return CreateRoomAs<Room>(std::move(name));
}

bool RoomManager::AddRoom(std::shared_ptr<Room> room) {
    if (!room) {
        return false;
    }

    std::unique_lock lock(mutex_);
    const auto [_, inserted] = rooms_.emplace(room->Id(), std::move(room));
    return inserted;
}

std::shared_ptr<Room> RoomManager::FindRoom(RoomId room_id) const {
    std::shared_lock lock(mutex_);
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        return nullptr;
    }
    return it->second;
}

bool RoomManager::RemoveRoom(RoomId room_id) {
    std::unique_lock lock(mutex_);
    return rooms_.erase(room_id) != 0;
}

std::vector<std::shared_ptr<Room>> RoomManager::Rooms() const {
    std::shared_lock lock(mutex_);

    std::vector<std::shared_ptr<Room>> rooms;
    rooms.reserve(rooms_.size());
    for (const auto& [_, room] : rooms_) {
        rooms.push_back(room);
    }
    return rooms;
}

std::vector<RoomManager::RoomInfo> RoomManager::GetRoomList() const {
    std::shared_lock lock(mutex_);

    std::vector<RoomInfo> room_list;
    room_list.reserve(rooms_.size());
    for (const auto& [id, room] : rooms_) {
        room_list.push_back(RoomInfo{
            .id = id,
            .name = room->Name(),
            .player_count = room->PlayerCount(),
            .max_players = Room::kMaxPlayers,
        });
    }
    return room_list;
}

std::uint32_t RoomManager::RoomCount() const {
    std::shared_lock lock(mutex_);
    return static_cast<std::uint32_t>(rooms_.size());
}

RoomId RoomManager::NextRoomId() const {
    std::shared_lock lock(mutex_);
    return next_room_id_;
}

RoomId RoomManager::NextRoomIdLocked() {
    while (next_room_id_ == 0 || rooms_.contains(next_room_id_)) {
        ++next_room_id_;
    }
    return next_room_id_++;
}

} // namespace iouring_runtime::game
