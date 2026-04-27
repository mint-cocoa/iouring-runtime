#include "auth_handler.h"
#include "../net/game_session.h"
#include "../net/io_worker.h"
#include "../game/player_context.h"
#include "../db/db_service.h"
#include "Auth.pb.h"
#include <spdlog/spdlog.h>

namespace handler {

void HandleLogin(GameSession& session, const std::byte* data, std::uint32_t len) {
    if (session.GetState() != SessionState::Connected) return;

    game::C_Login pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    auto* db = session.GetDbService();
    // shared_from_this()는 shared_ptr<EventHandler> 반환 — static_pointer_cast 필요
    auto sess_shared = std::static_pointer_cast<iouring_runtime::core::io::Session>(session.shared_from_this());
    std::weak_ptr<iouring_runtime::core::io::Session> sess_weak = sess_shared;
    auto* ring = &session.GetRing();
    auto* pm = session.GetPlayerManager();
    auto sid = session.GetSessionId();
    auto wid = session.GetWorker()->Id();

    db->Login(pkt.username(), pkt.password(),
        [sess_weak, ring, pm, sid, wid, username = pkt.username()](bool success, PlayerId pid) {
            ring->RunOnRing([sess_weak, success, pid, pm, sid, wid, username] {
                auto sess = sess_weak.lock();
                if (!sess) return;
                auto* gs = static_cast<GameSession*>(sess.get());

                if (success) {
                    auto* ctx = pm->Register(
                        sid, pid, &gs->GetRing(), wid, sess_weak);
                    ctx->username = username;
                    gs->SetPlayerCtx(ctx);
                    gs->SetState(SessionState::Authenticated);
                }

                game::S_Login reply;
                reply.set_success(success);
                reply.set_player_id(success ? pid : 0);
                gs->SendPacket(MsgId::S_LOGIN, reply);
            });
        });
}

void HandleRegister(GameSession& session, const std::byte* data, std::uint32_t len) {
    if (session.GetState() != SessionState::Connected) return;

    game::C_Register pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    auto* db = session.GetDbService();
    auto sess_shared = std::static_pointer_cast<iouring_runtime::core::io::Session>(session.shared_from_this());
    std::weak_ptr<iouring_runtime::core::io::Session> sess_weak = sess_shared;
    auto* ring = &session.GetRing();

    db->Register(pkt.username(), pkt.password(),
        [sess_weak, ring](bool success, std::string error) {
            ring->RunOnRing([sess_weak, success, error = std::move(error)] {
                auto sess = sess_weak.lock();
                if (!sess) return;
                auto* gs = static_cast<GameSession*>(sess.get());

                game::S_Register reply;
                reply.set_success(success);
                reply.set_error(error);
                gs->SendPacket(MsgId::S_REGISTER, reply);
            });
        });
}

}  // namespace handler
