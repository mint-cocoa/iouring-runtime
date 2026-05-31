#pragma once

#include "../types.h"
#include <iouring_runtime/game/PacketSession.h>
#include <iouring_runtime/core/Types.h>

class IoWorker;
class PlayerContext;
class PlayerManager;
class RoomManager;
class DbService;

enum class SessionState {
    Connected,
    Authenticated,
    CharSelect,
    InLobby,
    InRoom,
};

class GameSession : public iouring_runtime::game::PacketSession {
public:
    GameSession(int fd,
                iouring_runtime::core::ring::IoRing& ring,
                iouring_runtime::core::buffer::BufferPool& pool,
                IoWorker* worker);

    IoWorker*       GetWorker()      const { return worker_; }
    PlayerContext*  GetPlayerCtx()   const { return player_ctx_; }
    ::SessionState  GetState()       const { return state_; }
    DbService*      GetDbService()   const { return db_service_; }
    PlayerManager*  GetPlayerManager() const { return player_mgr_; }
    RoomManager*    GetRoomManager() const { return room_mgr_; }

    // protected 접근자를 public으로 노출
    iouring_runtime::core::ring::IoRing& GetRing() { return Ring(); }
    iouring_runtime::core::buffer::BufferPool& GetPool() { return Pool(); }

    void SetPlayerCtx(PlayerContext* ctx) { player_ctx_ = ctx; }
    void SetState(::SessionState s) { state_ = s; }

    void SetServices(PlayerManager* pm, RoomManager* rm, DbService* db);

    template<iouring_runtime::core::ProtobufMessage T>
    void SendPacket(MsgId msg_id, const T& proto) {
        iouring_runtime::game::PacketSession::SendPacket(
            static_cast<iouring_runtime::game::PacketId>(msg_id), proto);
    }

protected:
    void OnPacket(std::uint16_t msg_id,
                  const std::byte* data,
                  std::uint32_t len) override;

    void OnConnected() override;
    void OnDisconnected() override;

private:
    void HandlePreRoomPacket(std::uint16_t msg_id,
                             const std::byte* data,
                             std::uint32_t len);
    void HandleInRoomPacket(std::uint16_t msg_id,
                            const std::byte* data,
                            std::uint32_t len);

    IoWorker* worker_;
    PlayerContext* player_ctx_ = nullptr;
    ::SessionState state_ = ::SessionState::Connected;

    PlayerManager* player_mgr_ = nullptr;
    RoomManager*   room_mgr_   = nullptr;
    DbService*     db_service_  = nullptr;
};
