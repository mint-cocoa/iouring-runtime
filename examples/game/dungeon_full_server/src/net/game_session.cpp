#include "game_session.h"
#include "io_worker.h"
#include "../game/player_context.h"
#include "../game/room.h"
#include "../handler/auth_handler.h"
#include "../handler/char_handler.h"
#include "../handler/room_handler.h"
#include <spdlog/spdlog.h>

GameSession::GameSession(int fd,
                         iouring_runtime::core::ring::IoRing& ring,
                         iouring_runtime::core::buffer::BufferPool& pool,
                         IoWorker* worker)
    : PacketSession(fd, ring, pool)
    , worker_(worker)
{
}

void GameSession::SetServices(PlayerManager* pm, RoomManager* rm, DbService* db) {
    player_mgr_ = pm;
    room_mgr_   = rm;
    db_service_  = db;
}

void GameSession::OnConnected() {
    spdlog::info("GameSession: connected, sid={}", GetSessionId());
}

void GameSession::OnDisconnected() {
    spdlog::info("GameSession: disconnected, sid={}", GetSessionId());

    if (player_ctx_) {
        if (player_ctx_->room) {
            auto* room = player_ctx_->room;
            auto pid = player_ctx_->player_id;
            room->Push([room, pid] {
                room->RemovePlayer(pid);
            });
        }
        player_mgr_->Unregister(GetSessionId());
        player_ctx_ = nullptr;
    }

    worker_->RemoveSession(GetSessionId());
}

void GameSession::OnPacket(std::uint16_t msg_id,
                           const std::byte* data,
                           std::uint32_t len) {
    // Session-level packets: handled regardless of state
    switch (static_cast<MsgId>(msg_id)) {
        case MsgId::C_PORTAL: handler::HandlePortal(*this, data, len); return;
        default: break;
    }

    if (state_ == SessionState::InRoom) {
        HandleInRoomPacket(msg_id, data, len);
    } else {
        HandlePreRoomPacket(msg_id, data, len);
    }
}

void GameSession::HandlePreRoomPacket(std::uint16_t msg_id,
                                      const std::byte* data,
                                      std::uint32_t len) {
    switch (static_cast<MsgId>(msg_id)) {
        case MsgId::C_LOGIN:       handler::HandleLogin(*this, data, len); break;
        case MsgId::C_REGISTER:    handler::HandleRegister(*this, data, len); break;
        case MsgId::C_CHAR_LIST:   handler::HandleCharList(*this, data, len); break;
        case MsgId::C_CREATE_CHAR: handler::HandleCreateChar(*this, data, len); break;
        case MsgId::C_SELECT_CHAR: handler::HandleSelectChar(*this, data, len); break;
        case MsgId::C_ROOM_LIST:   handler::HandleRoomList(*this, data, len); break;
        case MsgId::C_CREATE_ROOM: handler::HandleCreateRoom(*this, data, len); break;
        case MsgId::C_JOIN_ROOM:   handler::HandleJoinRoom(*this, data, len); break;
        default:
            spdlog::warn("GameSession: unhandled pre-room packet, msg_id={}", msg_id);
            break;
    }
}

void GameSession::HandleInRoomPacket(std::uint16_t msg_id,
                                     const std::byte* data,
                                     std::uint32_t len) {
    if (!player_ctx_ || !player_ctx_->room) return;

    auto* room = player_ctx_->room;
    auto pid = player_ctx_->player_id;
    auto buf = std::vector<std::byte>(data, data + len);

    room->Push([room, pid, msg_id, buf = std::move(buf)] {
        room->HandlePacket(pid, msg_id, buf.data(),
                           static_cast<std::uint32_t>(buf.size()));
    });
}
