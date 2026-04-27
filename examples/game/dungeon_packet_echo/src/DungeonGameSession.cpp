#include "DungeonGameSession.h"

#include "Auth.pb.h"
#include "Common.pb.h"
#include "DungeonProtocol.h"
#include "DungeonRoom.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>

namespace {

std::atomic<iouring_runtime::game::PlayerId> g_next_player_id{1};

void FillPlayerInfo(game::PlayerInfo& info,
                    iouring_runtime::game::PlayerId player_id,
                    std::string_view name,
                    iouring_runtime::game::RoomId room_id) {
    info.set_player_id(player_id);
    info.set_name(std::string(name));
    info.set_hp(100);
    info.set_max_hp(100);
    info.set_level(1);
    info.set_zone_id(room_id);
    info.mutable_position()->set_y(0.5F);
}

} // namespace

DungeonGameSession::DungeonGameSession(
    int fd,
    iouring_runtime::core::ring::IoRing& ring,
    iouring_runtime::core::buffer::BufferPool& pool,
    std::shared_ptr<iouring_runtime::game::PlayerRegistry> player_registry,
    std::shared_ptr<iouring_runtime::game::RoomManager> room_manager)
    : PacketSession(fd, ring, pool)
    , player_registry_(std::move(player_registry))
    , room_manager_(std::move(room_manager)) {}

void DungeonGameSession::OnConnected() {
    std::cout << "dungeon packet client connected: " << RemoteAddr() << "\n";
}

void DungeonGameSession::OnDisconnected() {
    if (current_room_) {
        const auto player_id = player_id_;
        current_room_->Push([room = current_room_, player_id] {
            room->RemovePlayer(player_id);
        });
    }
    player_registry_->UnregisterBySession(GetSessionId());
    std::cout << "dungeon packet client disconnected\n";
}

void DungeonGameSession::OnPacket(iouring_runtime::game::PacketId packet_id,
                                  const std::byte* data, std::uint32_t len) {
    const auto payload = std::span<const std::byte>(data, len);

    if (packet_id == ToPacketId(DungeonMsgId::kCPortal)) {
        OnUnhandledPacket(packet_id, payload);
        return;
    }

    if (state_ == DungeonSessionState::kInRoom) {
        HandleInRoomPacket(packet_id, payload);
        return;
    }

    switch (packet_id) {
        case ToPacketId(DungeonMsgId::kCLogin):
            HandleLoginPacket(payload);
            return;
        case ToPacketId(DungeonMsgId::kCRoomList):
            HandleRoomListPacket(payload);
            return;
        case ToPacketId(DungeonMsgId::kCCreateRoom):
            HandleCreateRoomPacket(payload);
            return;
        case ToPacketId(DungeonMsgId::kCJoinRoom):
            HandleJoinRoomPacket(payload);
            return;
        default:
            OnUnhandledPacket(packet_id, payload);
            return;
    }
}

void DungeonGameSession::HandleLoginPacket(std::span<const std::byte> payload) {
    game::C_Login login;
    if (!login.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        Disconnect();
        return;
    }

    const auto player_id =
        g_next_player_id.fetch_add(1, std::memory_order_relaxed);
    if (!player_registry_->Register(GetSessionId(), player_id)) {
        Disconnect();
        return;
    }

    player_id_ = player_id;
    state_ = DungeonSessionState::kAuthenticated;
    player_name_ = login.username().empty() ? "player" : login.username();

    game::S_Login response;
    response.set_success(true);
    response.set_player_id(player_id);
    SendPacket(ToPacketId(DungeonMsgId::kSLogin), response);

    std::cout << "login accepted for user=" << player_name_
              << " player_id=" << player_id << "\n";
}

void DungeonGameSession::HandleRoomListPacket(std::span<const std::byte>) {
    game::S_RoomList response;
    for (const auto& room : room_manager_->GetRoomList()) {
        auto* out = response.add_rooms();
        out->set_zone_id(room.id);
        out->set_room_name(room.name);
        out->set_player_count(room.player_count);
        out->set_max_players(room.max_players);
    }
    SendPacket(ToPacketId(DungeonMsgId::kSRoomList), response);
}

void DungeonGameSession::HandleCreateRoomPacket(
    std::span<const std::byte> payload) {
    game::C_CreateRoom request;
    if (!request.ParseFromArray(payload.data(),
                                static_cast<int>(payload.size()))) {
        Disconnect();
        return;
    }

    auto room_name = request.room_name().empty()
                         ? std::string("Dungeon")
                         : request.room_name();
    auto room = room_manager_->CreateRoomAs<DungeonRoom>(std::move(room_name));
    JoinRoom(room);

    game::S_CreateRoom response;
    response.set_success(true);
    response.set_zone_id(room->Id());
    FillPlayerInfo(*response.mutable_player(), player_id_, player_name_,
                   room->Id());
    SendPacket(ToPacketId(DungeonMsgId::kSCreateRoom), response);
}

void DungeonGameSession::HandleJoinRoomPacket(
    std::span<const std::byte> payload) {
    game::C_JoinRoom request;
    if (!request.ParseFromArray(payload.data(),
                                static_cast<int>(payload.size()))) {
        Disconnect();
        return;
    }

    auto room = room_manager_->FindRoom(request.zone_id());
    game::S_JoinRoom response;
    if (!room) {
        response.set_success(false);
        response.set_zone_id(request.zone_id());
        response.set_error("room not found");
        SendPacket(ToPacketId(DungeonMsgId::kSJoinRoom), response);
        return;
    }
    if (room->IsFull()) {
        response.set_success(false);
        response.set_zone_id(room->Id());
        response.set_error("room is full");
        SendPacket(ToPacketId(DungeonMsgId::kSJoinRoom), response);
        return;
    }

    JoinRoom(room);
    response.set_success(true);
    response.set_zone_id(room->Id());
    FillPlayerInfo(*response.mutable_player(), player_id_, player_name_,
                   room->Id());
    SendPacket(ToPacketId(DungeonMsgId::kSJoinRoom), response);
}

void DungeonGameSession::OnUnhandledPacket(
    iouring_runtime::game::PacketId packet_id, std::span<const std::byte>) {
    std::cout << "unhandled dungeon packet id=" << packet_id << "\n";
}

void DungeonGameSession::HandleInRoomPacket(
    iouring_runtime::game::PacketId packet_id,
    std::span<const std::byte> payload) {
    if (!current_room_) {
        OnUnhandledPacket(packet_id, payload);
        return;
    }
    current_room_->HandlePacket(player_id_, packet_id, payload);
}

void DungeonGameSession::JoinRoom(
    std::shared_ptr<iouring_runtime::game::Room> room) {
    if (current_room_) {
        const auto player_id = player_id_;
        current_room_->Push([old_room = current_room_, player_id] {
            old_room->RemovePlayer(player_id);
        });
    }

    current_room_ = std::move(room);
    state_ = DungeonSessionState::kInRoom;
    player_registry_->BindRoomBySession(GetSessionId(), current_room_->Id(),
                                        current_room_.get());
    current_room_->Push([room = current_room_,
                         player = iouring_runtime::game::PlayerState{
                             .player_id = player_id_,
                         }] mutable {
        room->AddPlayer(std::move(player));
    });
}
