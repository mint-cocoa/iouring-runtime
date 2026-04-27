#pragma once

#include <iouring_runtime/game/PacketSession.h>
#include <iouring_runtime/game/PlayerRegistry.h>
#include <iouring_runtime/game/RoomManager.h>

#include <memory>
#include <string>

enum class DungeonSessionState {
    kConnected,
    kAuthenticated,
    kInRoom,
};

class DungeonGameSession final : public iouring_runtime::game::PacketSession {
public:
    DungeonGameSession(
        int fd,
        iouring_runtime::core::ring::IoRing& ring,
        iouring_runtime::core::buffer::BufferPool& pool,
        std::shared_ptr<iouring_runtime::game::PlayerRegistry> player_registry,
        std::shared_ptr<iouring_runtime::game::RoomManager> room_manager);

protected:
    void OnConnected() override;
    void OnDisconnected() override;
    void OnPacket(iouring_runtime::game::PacketId packet_id,
                  const std::byte* data, std::uint32_t len) override;

private:
    void HandleLoginPacket(std::span<const std::byte> payload);
    void HandleRoomListPacket(std::span<const std::byte> payload);
    void HandleCreateRoomPacket(std::span<const std::byte> payload);
    void HandleJoinRoomPacket(std::span<const std::byte> payload);
    void HandleInRoomPacket(iouring_runtime::game::PacketId packet_id,
                            std::span<const std::byte> payload);
    void OnUnhandledPacket(iouring_runtime::game::PacketId packet_id,
                           std::span<const std::byte> payload);
    void JoinRoom(std::shared_ptr<iouring_runtime::game::Room> room);

    DungeonSessionState state_ = DungeonSessionState::kConnected;
    iouring_runtime::game::PlayerId player_id_ = 0;
    std::shared_ptr<iouring_runtime::game::PlayerRegistry> player_registry_;
    std::shared_ptr<iouring_runtime::game::RoomManager> room_manager_;
    std::shared_ptr<iouring_runtime::game::Room> current_room_;
    std::string player_name_ = "player";
};
