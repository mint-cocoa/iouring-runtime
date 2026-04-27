#include "combat_system.h"
#include "../game/room.h"
#include "Game.pb.h"
#include "Common.pb.h"
#include "Currency.pb.h"
#include "Inventory.pb.h"
#include <cmath>
#include <random>
#include <spdlog/spdlog.h>

static void GrantKillReward(Room& room, PlayerState& attacker) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    // Gold reward 10~50
    int gold = 10 + (rng() % 41);
    attacker.gold += gold;

    if (attacker.session_id != 0) {
        game::S_CurrencyUpdate cu;
        cu.set_gold(attacker.gold);
        room.SendTo(attacker, MsgId::S_CURRENCY_UPDATE, cu);
    }

    // 30% chance random item drop
    if (rng() % 100 < 30 && attacker.session_id != 0) {
        int item_def_id = 1 + (rng() % 15);
        // Find free slot (simple: use attacker's kill count as pseudo-slot)
        int slot = static_cast<int>(rng() % 100);

        game::S_ItemAdd item_msg;
        auto* item = item_msg.mutable_item();
        item->set_instance_id(static_cast<int64_t>(rng()));
        item->set_item_def_id(item_def_id);
        item->set_slot(slot);
        item->set_quantity(1);
        item->set_durability(100);
        room.SendTo(attacker, MsgId::S_ITEM_ADD, item_msg);
    }
}

game::S_SkillData CombatSystem::BuildSkillDataPacket() {
    game::S_SkillData pkt;
    for (auto& sk : kSkills) {
        auto* si = pkt.add_skills();
        si->set_skill_id(sk.skill_id);
        si->set_name(sk.name);
        si->set_damage(sk.damage);
        si->set_range(sk.range);
        si->set_cooldown_ms(sk.cooldown_ms);
        si->set_is_self_target(sk.is_self_target);
    }
    return pkt;
}

bool CombatSystem::ProcessAttack(Room& room, PlayerId attacker_id, PlayerId target_id,
                                  int32_t skill_id, float fire_x, float fire_y, float fire_z,
                                  float dir_x, float dir_y, float dir_z) {
    spdlog::info("[Attack] attacker={} target={} skill={} fire=({:.1f},{:.1f},{:.1f}) dir=({:.2f},{:.2f},{:.2f})",
        attacker_id, target_id, skill_id, fire_x, fire_y, fire_z, dir_x, dir_y, dir_z);

    if (skill_id < 0 || skill_id >= static_cast<int32_t>(kSkills.size())) {
        spdlog::warn("[Attack] invalid skill_id={}", skill_id);
        return false;
    }

    auto& players = room.Players();
    auto att_it = players.find(attacker_id);
    if (att_it == players.end()) {
        spdlog::warn("[Attack] attacker {} not found", attacker_id);
        return false;
    }
    auto& attacker = att_it->second;

    if (attacker.is_dead) {
        spdlog::warn("[Attack] attacker {} is dead", attacker_id);
        return false;
    }

    auto& skill = kSkills[skill_id];

    // 쿨다운 체크
    auto now = std::chrono::steady_clock::now();
    auto cd_it = attacker.skill_cooldowns.find(skill_id);
    if (cd_it != attacker.skill_cooldowns.end()) {
        if (now < cd_it->second) return false;
    }
    attacker.skill_cooldowns[skill_id] =
        now + std::chrono::milliseconds(skill.cooldown_ms);

    // 타겟 해석
    PlayerState* target_ps = nullptr;
    if (skill.is_self_target) {
        target_ps = &attacker;
    } else {
        // target_id == 0 → fire_dir 레이캐스트로 충돌하는 가장 가까운 적 탐색
        PlayerId resolved_target = target_id;
        if (resolved_target == 0) {
            float dir_len = std::sqrt(dir_x*dir_x + dir_z*dir_z);
            float ndx = (dir_len > 0.001f) ? dir_x / dir_len : 0.0f;
            float ndz = (dir_len > 0.001f) ? dir_z / dir_len : 1.0f;

            static constexpr float kRayHitRadius = 1.5f;
            float best_proj = skill.range;

            for (auto& [pid, ps] : players) {
                if (pid == attacker_id || ps.is_dead) continue;
                float vx = ps.pos_x - fire_x;
                float vz = ps.pos_z - fire_z;
                float proj = vx * ndx + vz * ndz;
                if (proj < 0 || proj > skill.range) continue;
                float perp_sq = (vx*vx + vz*vz) - proj*proj;
                if (perp_sq > kRayHitRadius * kRayHitRadius) continue;
                if (proj < best_proj) {
                    best_proj = proj;
                    resolved_target = pid;
                }
            }
            if (resolved_target == 0) {
                spdlog::info("[Attack] ray-cast: no hit along dir=({:.2f},{:.2f}) range={:.1f}",
                             ndx, ndz, skill.range);
                return false;
            }
            spdlog::info("[Attack] ray-cast hit target={} dist={:.2f}", resolved_target, best_proj);
        }

        auto tgt_it = players.find(resolved_target);
        if (tgt_it == players.end()) {
            spdlog::warn("[Attack] target {} not found in players", resolved_target);
            return false;
        }
        target_ps = &tgt_it->second;
        if (target_ps->is_dead) return false;

        // 거리 체크
        float dx = target_ps->pos_x - attacker.pos_x;
        float dy = target_ps->pos_y - attacker.pos_y;
        float dz = target_ps->pos_z - attacker.pos_z;
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        spdlog::info("[Attack] dist={:.2f} range={:.2f}", dist, skill.range);
        if (dist > skill.range) {
            spdlog::warn("[Attack] out of range dist={:.2f} > range={:.2f}", dist, skill.range);
            return false;
        }
    }

    // S_ATTACK 브로드캐스트 (resolved target 사용)
    game::S_Attack attack_msg;
    attack_msg.set_attacker_id(attacker_id);
    attack_msg.set_target_id(target_ps->player_id);
    attack_msg.set_skill_id(skill_id);
    auto* fp = attack_msg.mutable_fire_pos();
    fp->set_x(fire_x); fp->set_y(fire_y); fp->set_z(fire_z);
    auto* fd = attack_msg.mutable_fire_dir();
    fd->set_x(dir_x); fd->set_y(dir_y); fd->set_z(dir_z);
    room.BroadcastAll(MsgId::S_ATTACK, attack_msg);

    // 데미지 적용
    target_ps->hp -= skill.damage;  // 음수 데미지 = 힐
    if (target_ps->hp > target_ps->max_hp) target_ps->hp = target_ps->max_hp;
    if (target_ps->hp < 0) target_ps->hp = 0;

    bool is_dead = (target_ps->hp <= 0);

    // S_DAMAGE
    game::S_Damage dmg_msg;
    dmg_msg.set_attacker_id(attacker_id);
    dmg_msg.set_target_id(target_ps->player_id);
    dmg_msg.set_damage(skill.damage);
    dmg_msg.set_remaining_hp(target_ps->hp);
    dmg_msg.set_skill_id(skill_id);
    dmg_msg.set_is_dead(is_dead);
    room.BroadcastAll(MsgId::S_DAMAGE, dmg_msg);

    if (is_dead) {
        target_ps->is_dead = true;
        target_ps->death_time = now;
        target_ps->deaths++;
        attacker.kills++;
        GrantKillReward(room, attacker);

        // Drop dog tag only when a real player gets the kill
        if (attacker.session_id != 0) {
            std::string label = target_ps->name + "'s Dog Tag";
            room.SpawnGroundItem(100, target_ps->pos_x, 0.5f, target_ps->pos_z, label);
        }
    }

    return true;
}

bool CombatSystem::ProcessProjectileHit(Room& room, PlayerId attacker_id, PlayerId target_id,
                                         int32_t skill_id) {
    if (skill_id < 0 || skill_id >= static_cast<int32_t>(kSkills.size())) return false;

    auto& players = room.Players();
    auto att_it = players.find(attacker_id);
    if (att_it == players.end()) return false;
    auto& attacker = att_it->second;
    if (attacker.is_dead) return false;

    auto tgt_it = players.find(target_id);
    if (tgt_it == players.end()) return false;
    auto& target = tgt_it->second;
    if (target.is_dead) return false;

    auto& skill = kSkills[skill_id];

    // No range check — projectile already reached the target
    // No cooldown check — cooldown was checked at fire time

    game::S_Attack attack_msg;
    attack_msg.set_attacker_id(attacker_id);
    attack_msg.set_target_id(target_id);
    attack_msg.set_skill_id(skill_id);
    auto* fp = attack_msg.mutable_fire_pos();
    fp->set_x(attacker.pos_x); fp->set_y(attacker.pos_y); fp->set_z(attacker.pos_z);
    room.BroadcastAll(MsgId::S_ATTACK, attack_msg);

    target.hp -= skill.damage;
    if (target.hp > target.max_hp) target.hp = target.max_hp;
    if (target.hp < 0) target.hp = 0;

    bool is_dead = (target.hp <= 0);

    game::S_Damage dmg_msg;
    dmg_msg.set_attacker_id(attacker_id);
    dmg_msg.set_target_id(target_id);
    dmg_msg.set_damage(skill.damage);
    dmg_msg.set_remaining_hp(target.hp);
    dmg_msg.set_skill_id(skill_id);
    dmg_msg.set_is_dead(is_dead);
    room.BroadcastAll(MsgId::S_DAMAGE, dmg_msg);

    if (is_dead) {
        target.is_dead = true;
        target.death_time = std::chrono::steady_clock::now();
        target.deaths++;
        attacker.kills++;
        GrantKillReward(room, attacker);

        if (attacker.session_id != 0) {
            std::string label = target.name + "'s Dog Tag";
            room.SpawnGroundItem(100, target.pos_x, 0.5f, target.pos_z, label);
        }
    }

    spdlog::info("[ProjectileHit] {} -> {} skill={} dmg={} hp={} dead={}",
                 attacker_id, target_id, skill_id, skill.damage, target.hp, is_dead);
    return true;
}

void CombatSystem::CheckRespawns(Room& room, TimePoint now) {
    static constexpr auto kRespawnDelay = std::chrono::seconds(5);

    for (auto& [pid, ps] : room.Players()) {
        if (!ps.is_dead) continue;
        if (now - ps.death_time < kRespawnDelay) continue;

        ps.is_dead = false;
        ps.hp = ps.max_hp;
        // 랜덤 위치로 리스폰
        static thread_local std::mt19937 rng{std::random_device{}()};
        float rx = 0, rz = 0;
        if (!room.GetDungeon().GetRandomFloorPosition(rng, rx, rz)) {
            auto& spawn = room.SpawnPosition();
            rx = spawn.x(); rz = spawn.z();
        }
        ps.pos_x = rx;
        ps.pos_y = 0.5f;
        ps.pos_z = rz;

        game::S_Respawn respawn_msg;
        auto* pi = respawn_msg.mutable_player();
        pi->set_player_id(ps.player_id);
        pi->set_name(ps.name);
        pi->set_hp(ps.hp);
        pi->set_max_hp(ps.max_hp);
        pi->set_level(ps.level);
        auto* pos = pi->mutable_position();
        pos->set_x(ps.pos_x);
        pos->set_y(ps.pos_y);
        pos->set_z(ps.pos_z);

        room.BroadcastAll(MsgId::S_RESPAWN, respawn_msg);
    }
}
