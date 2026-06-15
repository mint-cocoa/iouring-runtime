#pragma once

#include <iouring/core/Types.h>
#include <iouring/game/Types.h>

#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace iouring::game {

class RoomLike;

struct PlayerRecord {
    core::SessionId session_id = 0;
    PlayerId player_id = 0;
    RoomId room_id = 0;
    RoomLike* room = nullptr;
};

class PlayerRegistry {
public:
    bool Register(core::SessionId session_id, PlayerId player_id);
    std::optional<PlayerRecord> UnregisterBySession(
        core::SessionId session_id);
    std::optional<PlayerRecord> UnregisterByPlayer(PlayerId player_id);

    bool BindRoomBySession(core::SessionId session_id, RoomId room_id,
                           RoomLike* room);
    bool BindRoomByPlayer(PlayerId player_id, RoomId room_id, RoomLike* room);
    bool ClearRoomBySession(core::SessionId session_id);
    bool ClearRoomByPlayer(PlayerId player_id);

    std::optional<PlayerRecord> FindBySession(
        core::SessionId session_id) const;
    std::optional<PlayerRecord> FindByPlayer(PlayerId player_id) const;
    std::vector<PlayerRecord> Records() const;
    std::uint32_t PlayerCount() const;

private:
    bool BindRoomLocked(PlayerRecord& record, RoomId room_id, RoomLike* room);
    bool ClearRoomLocked(PlayerRecord& record);

    mutable std::shared_mutex mutex_;
    std::unordered_map<core::SessionId, PlayerRecord> by_session_;
    std::unordered_map<PlayerId, core::SessionId> by_player_;
};

} // namespace iouring::game
