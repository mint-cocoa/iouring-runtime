#pragma once

#include "../types.h"
#include <iouring_runtime/core/Types.h>
#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/Session.h>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <memory>

class Room;

struct PlayerContext {
    iouring_runtime::core::SessionId session_id = 0;
    PlayerId player_id = 0;
    std::string username;
    CharId selected_char_id = 0;
    std::string char_name;
    int32_t level = 1;

    Room* room = nullptr;

    iouring_runtime::core::ring::IoRing* worker_ring = nullptr;
    iouring_runtime::core::ContextId worker_id = 0;
    std::weak_ptr<iouring_runtime::core::io::Session> session;
};

class PlayerManager {
public:
    PlayerContext* Register(iouring_runtime::core::SessionId sid, PlayerId pid,
                            iouring_runtime::core::ring::IoRing* ring,
                            iouring_runtime::core::ContextId worker_id,
                            std::weak_ptr<iouring_runtime::core::io::Session> sess);

    void Unregister(iouring_runtime::core::SessionId sid);

    PlayerContext* FindBySession(iouring_runtime::core::SessionId sid);
    PlayerContext* FindByPlayer(PlayerId pid);
    PlayerContext* FindByName(const std::string& name);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<iouring_runtime::core::SessionId, std::unique_ptr<PlayerContext>> by_session_;
    std::unordered_map<PlayerId, PlayerContext*> by_player_;
    std::unordered_map<std::string, PlayerContext*> by_name_;
};
