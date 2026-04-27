#include <iouring_runtime/game/Room.h>

#include <iouring_runtime/observability/Logging.h>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kSession;
}

namespace iouring_runtime::game {

Room::Room(RoomId id, std::string name, core::job::GlobalQueue& global_queue)
    : JobQueue(global_queue)
    , id_(id)
    , name_(std::move(name)) {}

bool Room::HasPlayer(PlayerId player_id) const {
    return players_.contains(player_id);
}

void Room::AddPlayer(PlayerState player) {
    players_[player.player_id] = std::move(player);
}

void Room::RemovePlayer(PlayerId player_id) {
    players_.erase(player_id);
}

void Room::HandlePacket(PlayerId player_id, PacketId msg_id,
                        std::span<const std::byte> payload) {
    auto copy = std::vector<std::byte>(payload.begin(), payload.end());
    Push([this, player_id, msg_id, copy = std::move(copy)] {
        DispatchPacket(player_id, msg_id, copy);
    });
}

void Room::DispatchPacket(PlayerId player_id, PacketId msg_id,
                          std::span<const std::byte> payload) {
    auto it = players_.find(player_id);
    if (it == players_.end()) {
        return;
    }

    OnPacket(it->second, msg_id, payload);
}

void Room::OnPacket(PlayerState& player, PacketId msg_id,
                    std::span<const std::byte>) {
    obs::LogWarn(kLogCategory,
                 "Room[{}]: unhandled packet id={} from player={}",
                 id_, static_cast<std::uint16_t>(msg_id), player.player_id);
}

} // namespace iouring_runtime::game
