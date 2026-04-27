#include "game_handler.h"
#include "../game/room.h"
#include "../game/dungeon_generator.h"
#include "../system/combat_system.h"
#include "Game.pb.h"
#include "Common.pb.h"
#include "Inventory.pb.h"
#include "Currency.pb.h"
#include <cmath>
#include <spdlog/spdlog.h>

namespace handler {

void HandleMove(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len) {
    game::C_Move pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    // Wall collision validation
    float nx = pkt.position().x();
    float nz = pkt.position().z();
    auto& dg = room.GetDungeon();
    int gx = static_cast<int>(std::round(nx / DungeonGenerator::CELL_SIZE + DungeonGenerator::GRID_WIDTH / 2.0f));
    int gz = static_cast<int>(std::round(nz / DungeonGenerator::CELL_SIZE + DungeonGenerator::GRID_HEIGHT / 2.0f));
    if (dg.GetTile(gx, gz) != 0) return;  // reject move into wall

    ps.pos_x = nx;
    ps.pos_y = pkt.position().y();
    ps.pos_z = nz;
    ps.rotation_y = pkt.rotation_y();
    if (pkt.has_velocity()) {
        ps.vel_x = pkt.velocity().x();
        ps.vel_y = pkt.velocity().y();
        ps.vel_z = pkt.velocity().z();
    }
    ps.move_state = pkt.state();

    game::S_Move move_msg;
    move_msg.set_player_id(ps.player_id);
    auto* pos = move_msg.mutable_position();
    pos->set_x(ps.pos_x); pos->set_y(ps.pos_y); pos->set_z(ps.pos_z);
    move_msg.set_rotation_y(ps.rotation_y);
    auto* vel = move_msg.mutable_velocity();
    vel->set_x(ps.vel_x); vel->set_y(ps.vel_y); vel->set_z(ps.vel_z);
    move_msg.set_state(ps.move_state);

    room.BroadcastExcept(ps.player_id, MsgId::S_MOVE, move_msg);
}

void HandleAttack(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len) {
    game::C_Attack pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    static CombatSystem combat;
    float fx = 0, fy = 0, fz = 0, dx = 0, dy = 0, dz = 0;
    if (pkt.has_fire_pos()) {
        fx = pkt.fire_pos().x(); fy = pkt.fire_pos().y(); fz = pkt.fire_pos().z();
    }
    if (pkt.has_fire_dir()) {
        dx = pkt.fire_dir().x(); dy = pkt.fire_dir().y(); dz = pkt.fire_dir().z();
    }

    combat.ProcessAttack(room, ps.player_id, pkt.target_id(), pkt.skill_id(),
                         fx, fy, fz, dx, dy, dz);
}

void HandleFire(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len) {
    game::C_Fire pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    // Cooldown check for fire (skill 0, 500ms)
    auto now = std::chrono::steady_clock::now();
    auto cd_it = ps.skill_cooldowns.find(0);
    if (cd_it != ps.skill_cooldowns.end() && now < cd_it->second) return;
    ps.skill_cooldowns[0] = now + std::chrono::milliseconds(500);

    game::S_Fire fire_msg;
    fire_msg.set_player_id(ps.player_id);
    auto* fp = fire_msg.mutable_fire_pos();
    fp->set_x(pkt.fire_pos().x()); fp->set_y(pkt.fire_pos().y()); fp->set_z(pkt.fire_pos().z());
    auto* fd = fire_msg.mutable_fire_dir();
    fd->set_x(pkt.fire_dir().x()); fd->set_y(pkt.fire_dir().y()); fd->set_z(pkt.fire_dir().z());

    room.BroadcastExcept(ps.player_id, MsgId::S_FIRE, fire_msg);

    // Spawn server-side projectile
    room.Projectiles().SpawnProjectile(
        ps.player_id,
        pkt.fire_pos().x(), pkt.fire_pos().y(), pkt.fire_pos().z(),
        pkt.fire_dir().x(), pkt.fire_dir().y(), pkt.fire_dir().z(),
        0);
}

void HandlePickup(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len) {
    game::C_Pickup pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    game::S_Pickup reply;
    reply.set_ground_id(pkt.ground_id());
    if (room.TryPickup(ps.player_id, pkt.ground_id())) {
        reply.set_success(true);
    } else {
        reply.set_success(false);
        reply.set_error("Cannot pick up");
    }
    room.SendTo(ps, MsgId::S_PICKUP, reply);
}

void HandleUseItem(Room& room, PlayerState& ps, const std::byte* data, std::uint32_t len) {
    game::C_UseItem pkt;
    if (!pkt.ParseFromArray(data, static_cast<int>(len))) return;

    game::S_UseItem reply;
    reply.set_instance_id(pkt.instance_id());
    reply.set_success(true);

    // Dog Tag (item_def_id=100): +50 gold bonus
    // We don't track actual inventory server-side per-item yet,
    // so grant reward and remove the item
    ps.gold += 50;
    reply.set_effect("Dog Tag redeemed: +50 Gold");

    room.SendTo(ps, MsgId::S_USE_ITEM, reply);

    // Update currency
    game::S_CurrencyUpdate cu;
    cu.set_gold(ps.gold);
    room.SendTo(ps, MsgId::S_CURRENCY_UPDATE, cu);

    // Remove item from inventory
    game::S_ItemRemove rm;
    rm.set_instance_id(pkt.instance_id());
    room.SendTo(ps, MsgId::S_ITEM_REMOVE, rm);

    spdlog::info("Player {} used item instance={}", ps.player_id, pkt.instance_id());
}

}  // namespace handler
