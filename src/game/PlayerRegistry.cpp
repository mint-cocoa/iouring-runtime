#include <iouring/game/PlayerRegistry.h>

#include <mutex>

namespace iouring::game {

bool PlayerRegistry::Register(core::SessionId session_id, PlayerId player_id) {
    if (session_id == 0 || player_id == 0) {
        return false;
    }

    std::unique_lock lock(mutex_);
    if (by_session_.contains(session_id) || by_player_.contains(player_id)) {
        return false;
    }

    by_session_.emplace(session_id, PlayerRecord{
                                        .session_id = session_id,
                                        .player_id = player_id,
                                    });
    by_player_.emplace(player_id, session_id);
    return true;
}

std::optional<PlayerRecord> PlayerRegistry::UnregisterBySession(
    core::SessionId session_id) {
    std::unique_lock lock(mutex_);

    auto it = by_session_.find(session_id);
    if (it == by_session_.end()) {
        return std::nullopt;
    }

    auto record = it->second;
    by_player_.erase(record.player_id);
    by_session_.erase(it);
    return record;
}

std::optional<PlayerRecord> PlayerRegistry::UnregisterByPlayer(
    PlayerId player_id) {
    std::unique_lock lock(mutex_);

    auto player_it = by_player_.find(player_id);
    if (player_it == by_player_.end()) {
        return std::nullopt;
    }

    auto session_it = by_session_.find(player_it->second);
    if (session_it == by_session_.end()) {
        by_player_.erase(player_it);
        return std::nullopt;
    }

    auto record = session_it->second;
    by_session_.erase(session_it);
    by_player_.erase(player_it);
    return record;
}

bool PlayerRegistry::BindRoomBySession(core::SessionId session_id,
                                       RoomId room_id, RoomLike* room) {
    std::unique_lock lock(mutex_);

    auto it = by_session_.find(session_id);
    if (it == by_session_.end()) {
        return false;
    }
    return BindRoomLocked(it->second, room_id, room);
}

bool PlayerRegistry::BindRoomByPlayer(PlayerId player_id, RoomId room_id,
                                      RoomLike* room) {
    std::unique_lock lock(mutex_);

    auto player_it = by_player_.find(player_id);
    if (player_it == by_player_.end()) {
        return false;
    }
    auto session_it = by_session_.find(player_it->second);
    if (session_it == by_session_.end()) {
        return false;
    }
    return BindRoomLocked(session_it->second, room_id, room);
}

bool PlayerRegistry::ClearRoomBySession(core::SessionId session_id) {
    std::unique_lock lock(mutex_);

    auto it = by_session_.find(session_id);
    if (it == by_session_.end()) {
        return false;
    }
    return ClearRoomLocked(it->second);
}

bool PlayerRegistry::ClearRoomByPlayer(PlayerId player_id) {
    std::unique_lock lock(mutex_);

    auto player_it = by_player_.find(player_id);
    if (player_it == by_player_.end()) {
        return false;
    }
    auto session_it = by_session_.find(player_it->second);
    if (session_it == by_session_.end()) {
        return false;
    }
    return ClearRoomLocked(session_it->second);
}

std::optional<PlayerRecord> PlayerRegistry::FindBySession(
    core::SessionId session_id) const {
    std::shared_lock lock(mutex_);

    auto it = by_session_.find(session_id);
    if (it == by_session_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<PlayerRecord> PlayerRegistry::FindByPlayer(
    PlayerId player_id) const {
    std::shared_lock lock(mutex_);

    auto player_it = by_player_.find(player_id);
    if (player_it == by_player_.end()) {
        return std::nullopt;
    }
    auto session_it = by_session_.find(player_it->second);
    if (session_it == by_session_.end()) {
        return std::nullopt;
    }
    return session_it->second;
}

std::vector<PlayerRecord> PlayerRegistry::Records() const {
    std::shared_lock lock(mutex_);

    std::vector<PlayerRecord> records;
    records.reserve(by_session_.size());
    for (const auto& [_, record] : by_session_) {
        records.push_back(record);
    }
    return records;
}

std::uint32_t PlayerRegistry::PlayerCount() const {
    std::shared_lock lock(mutex_);
    return static_cast<std::uint32_t>(by_session_.size());
}

bool PlayerRegistry::BindRoomLocked(PlayerRecord& record, RoomId room_id,
                                    RoomLike* room) {
    if (room_id == 0 || room == nullptr) {
        return false;
    }
    record.room_id = room_id;
    record.room = room;
    return true;
}

bool PlayerRegistry::ClearRoomLocked(PlayerRecord& record) {
    record.room_id = 0;
    record.room = nullptr;
    return true;
}

} // namespace iouring::game
