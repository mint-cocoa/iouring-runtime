#include "party_system.h"
#include "../game/room.h"
#include "Social.pb.h"
#include "Common.pb.h"

void PartySystem::CreateParty(Room& room, PlayerId leader) {
    auto& players = room.Players();
    auto it = players.find(leader);
    if (it == players.end()) return;
    auto& ps = it->second;

    if (ps.party_id != 0) return;  // 이미 파티 소속

    auto pid = next_id_++;
    parties_[pid] = {pid, leader, {leader}};
    ps.party_id = pid;

    game::S_CreateParty reply;
    reply.set_success(true);
    reply.set_party_id(pid);
    room.SendTo(ps, MsgId::S_CREATE_PARTY, reply);

    BroadcastPartyUpdate(room, pid);
}

void PartySystem::JoinParty(Room& room, PlayerId player, PartyId party_id) {
    auto& players = room.Players();
    auto it = players.find(player);
    if (it == players.end()) return;
    auto& ps = it->second;

    if (ps.party_id != 0) return;

    auto pit = parties_.find(party_id);
    if (pit == parties_.end()) {
        game::S_JoinParty reply;
        reply.set_success(false);
        room.SendTo(ps, MsgId::S_JOIN_PARTY, reply);
        return;
    }

    pit->second.members.push_back(player);
    ps.party_id = party_id;

    game::S_JoinParty reply;
    reply.set_success(true);
    reply.set_party_id(party_id);
    room.SendTo(ps, MsgId::S_JOIN_PARTY, reply);

    BroadcastPartyUpdate(room, party_id);
}

void PartySystem::LeaveParty(Room& room, PlayerId player) {
    auto& players = room.Players();
    auto it = players.find(player);
    if (it == players.end()) return;
    auto& ps = it->second;

    if (ps.party_id == 0) return;

    auto party_id = ps.party_id;
    ps.party_id = 0;

    game::S_LeaveParty reply;
    reply.set_success(true);
    room.SendTo(ps, MsgId::S_LEAVE_PARTY, reply);

    auto pit = parties_.find(party_id);
    if (pit == parties_.end()) return;

    auto& members = pit->second.members;
    members.erase(std::remove(members.begin(), members.end(), player), members.end());

    if (members.empty()) {
        parties_.erase(pit);
        return;
    }

    // 리더가 떠나면 다음 멤버가 리더
    if (pit->second.leader_id == player) {
        pit->second.leader_id = members[0];
    }

    BroadcastPartyUpdate(room, party_id);
}

void PartySystem::OnPlayerLeave(Room& room, PlayerId player) {
    auto& players = room.Players();
    auto it = players.find(player);
    if (it == players.end()) return;
    if (it->second.party_id != 0) {
        LeaveParty(room, player);
    }
}

void PartySystem::BroadcastPartyUpdate(Room& room, PartyId party_id) {
    auto pit = parties_.find(party_id);
    if (pit == parties_.end()) return;

    game::S_PartyUpdate update;
    update.set_party_id(party_id);
    update.set_leader_id(pit->second.leader_id);

    auto& players = room.Players();
    for (auto mid : pit->second.members) {
        auto mit = players.find(mid);
        if (mit == players.end()) continue;
        auto* mi = update.add_members();
        mi->set_player_id(mit->second.player_id);
        mi->set_player_name(mit->second.name);
        mi->set_hp(mit->second.hp);
        mi->set_max_hp(mit->second.max_hp);
        mi->set_level(mit->second.level);
        mi->set_is_leader(mid == pit->second.leader_id);
    }

    for (auto mid : pit->second.members) {
        auto mit = players.find(mid);
        if (mit != players.end()) {
            room.SendTo(mit->second, MsgId::S_PARTY_UPDATE, update);
        }
    }
}
