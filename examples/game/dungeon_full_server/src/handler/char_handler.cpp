#include "char_handler.h"
#include "../net/game_session.h"
#include "../net/io_worker.h"
#include "../game/player_context.h"
#include "../db/db_service.h"
#include "Auth.pb.h"
#include "Inventory.pb.h"
#include "Currency.pb.h"
#include "Common.pb.h"
#include <spdlog/spdlog.h>

namespace handler {

void HandleCharList(GameSession& session, const std::byte* data, std::uint32_t len) {
    if (session.GetState() != SessionState::Authenticated &&
        session.GetState() != SessionState::CharSelect &&
        session.GetState() != SessionState::InLobby) return;

    auto* ctx = session.GetPlayerCtx();
    if (!ctx) return;

    auto* db = session.GetDbService();
    auto sess_shared = std::static_pointer_cast<iouring_runtime::core::io::Session>(session.shared_from_this());
    std::weak_ptr<iouring_runtime::core::io::Session> sess_weak = sess_shared;
    auto* ring = &session.GetRing();

    db->GetCharList(ctx->player_id,
        [sess_weak, ring](std::vector<CharInfo> chars) {
            ring->RunOnRing([sess_weak, chars = std::move(chars)] {
                auto sess = sess_weak.lock();
                if (!sess) return;
                auto* gs = static_cast<GameSession*>(sess.get());

                game::S_CharList reply;
                for (auto& c : chars) {
                    auto* ci = reply.add_characters();
                    ci->set_char_id(c.id);
                    ci->set_name(c.name);
                    ci->set_level(c.level);
                }
                gs->SendPacket(MsgId::S_CHAR_LIST, reply);
                gs->SetState(SessionState::CharSelect);
            });
        });
}

void HandleCreateChar(GameSession& session, const std::byte* data, std::uint32_t len) {
    if (session.GetState() != SessionState::Authenticated &&
        session.GetState() != SessionState::CharSelect &&
        session.GetState() != SessionState::InLobby) return;

    game::C_CreateChar pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    auto* ctx = session.GetPlayerCtx();
    if (!ctx) return;

    auto* db = session.GetDbService();
    auto sess_shared2 = std::static_pointer_cast<iouring_runtime::core::io::Session>(session.shared_from_this());
    std::weak_ptr<iouring_runtime::core::io::Session> sess_weak = sess_shared2;
    auto* ring = &session.GetRing();

    db->CreateChar(ctx->player_id, pkt.name(),
        [sess_weak, ring](bool success, std::string error, CharInfo ch) {
            ring->RunOnRing([sess_weak, success, error = std::move(error), ch] {
                auto sess = sess_weak.lock();
                if (!sess) return;
                auto* gs = static_cast<GameSession*>(sess.get());

                game::S_CreateChar reply;
                reply.set_success(success);
                reply.set_error(error);
                if (success) {
                    auto* ci = reply.mutable_character();
                    ci->set_char_id(ch.id);
                    ci->set_name(ch.name);
                    ci->set_level(ch.level);
                }
                gs->SendPacket(MsgId::S_CREATE_CHAR, reply);
            });
        });
}

void HandleSelectChar(GameSession& session, const std::byte* data, std::uint32_t len) {
    if (session.GetState() != SessionState::Authenticated &&
        session.GetState() != SessionState::CharSelect &&
        session.GetState() != SessionState::InLobby) return;

    game::C_SelectChar pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    auto* ctx = session.GetPlayerCtx();
    if (!ctx) return;

    auto* db = session.GetDbService();
    auto sess_shared3 = std::static_pointer_cast<iouring_runtime::core::io::Session>(session.shared_from_this());
    std::weak_ptr<iouring_runtime::core::io::Session> sess_weak = sess_shared3;
    auto* ring = &session.GetRing();
    auto pid = ctx->player_id;
    auto char_id = pkt.char_id();

    // 캐릭터 정보 로드 → 인벤토리 로드 → 화폐 로드 → 3개 응답 전송
    db->GetCharList(pid, [sess_weak, ring, db, char_id](std::vector<CharInfo> chars) {
        // 캐릭터 찾기
        CharInfo found{};
        bool ok = false;
        for (auto& c : chars) {
            if (c.id == char_id) { found = c; ok = true; break; }
        }

        if (!ok) {
            ring->RunOnRing([sess_weak] {
                auto sess = sess_weak.lock();
                if (!sess) return;
                auto* gs = static_cast<GameSession*>(sess.get());
                game::S_SelectChar reply;
                reply.set_success(false);
                gs->SendPacket(MsgId::S_SELECT_CHAR, reply);
            });
            return;
        }

        // S_SELECT_CHAR 전송
        ring->RunOnRing([sess_weak, found, db, ring, char_id] {
            auto sess = sess_weak.lock();
            if (!sess) return;
            auto* gs = static_cast<GameSession*>(sess.get());
            auto* ctx = gs->GetPlayerCtx();
            if (!ctx) return;

            ctx->selected_char_id = char_id;
            ctx->char_name = found.name;
            ctx->level = found.level;

            // by_name 등록
            // (PlayerManager에서 직접 접근은 스레드 안전하지 않으므로
            //  여기서는 ctx만 업데이트)

            game::S_SelectChar reply;
            reply.set_success(true);
            reply.set_player_id(ctx->player_id);
            reply.set_name(found.name);
            reply.set_level(found.level);
            gs->SendPacket(MsgId::S_SELECT_CHAR, reply);

            // Sequential chain: LoadInventory → (inside its callback) LoadCurrency.
            //
            // The two queries are issued back-to-back in the async DB backend
            // (PostgreSQL worker pool), which may dispatch them to different
            // worker threads and reorder completion. Serializing them here
            // guarantees S_INVENTORY_INIT precedes S_CURRENCY_INIT, and in
            // particular that SetState(InLobby) — which gates subsequent room
            // handlers — only runs after both replies have been sent.
            db->LoadInventory(char_id, [sess_weak, ring, db, char_id](std::vector<ItemData> items) {
                ring->RunOnRing([sess_weak, ring, db, char_id, items = std::move(items)]() mutable {
                    auto sess = sess_weak.lock();
                    if (!sess) return;
                    auto* gs = static_cast<GameSession*>(sess.get());

                    game::S_InventoryInit inv_reply;
                    for (auto& item : items) {
                        auto* ii = inv_reply.add_items();
                        ii->set_instance_id(item.instance_id);
                        ii->set_item_def_id(item.item_def_id);
                        ii->set_slot(item.slot);
                        ii->set_quantity(item.quantity);
                        ii->set_durability(item.durability);
                    }
                    gs->SendPacket(MsgId::S_INVENTORY_INIT, inv_reply);

                    // Chain LoadCurrency after inventory has been sent.
                    db->LoadCurrency(char_id, [sess_weak, ring](CurrencyData cur) {
                        ring->RunOnRing([sess_weak, cur] {
                            auto sess = sess_weak.lock();
                            if (!sess) return;
                            auto* gs = static_cast<GameSession*>(sess.get());

                            game::S_CurrencyInit cur_reply;
                            cur_reply.set_gold(cur.gold);
                            gs->SendPacket(MsgId::S_CURRENCY_INIT, cur_reply);

                            gs->SetState(SessionState::InLobby);
                        });
                    });
                });
            });
        });
    });
}

}  // namespace handler
