#pragma once

#include <iouring_runtime/core/JobQueue.h>
#include <iouring_runtime/game/Types.h>

#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace iouring_runtime::game {

class RoomLike {
public:
    virtual ~RoomLike() = default;
    virtual void HandlePacket(PlayerId player_id, PacketId msg_id,
                              std::span<const std::byte> payload) = 0;
};

struct PlayerState {
    PlayerId player_id = 0;
};

class Room : public RoomLike, public core::job::JobQueue {
public:
    static constexpr std::uint32_t kMaxPlayers = 500;

    Room(RoomId id, std::string name, core::job::GlobalQueue& global_queue);

    RoomId Id() const noexcept {
        return id_;
    }

    const std::string& Name() const noexcept {
        return name_;
    }

    std::uint32_t PlayerCount() const noexcept {
        return static_cast<std::uint32_t>(players_.size());
    }

    bool IsEmpty() const noexcept {
        return players_.empty();
    }

    bool IsFull() const noexcept {
        return players_.size() >= kMaxPlayers;
    }

    bool HasPlayer(PlayerId player_id) const;
    void AddPlayer(PlayerState player);
    void RemovePlayer(PlayerId player_id);

    void HandlePacket(PlayerId player_id, PacketId msg_id,
                      std::span<const std::byte> payload) override;

protected:
    virtual void OnPacket(PlayerState& player, PacketId msg_id,
                          std::span<const std::byte> payload);

private:
    void DispatchPacket(PlayerId player_id, PacketId msg_id,
                        std::span<const std::byte> payload);

    RoomId id_;
    std::string name_;
    std::unordered_map<PlayerId, PlayerState> players_;
};

} // namespace iouring_runtime::game
