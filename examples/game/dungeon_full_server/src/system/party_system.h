#pragma once
#include "../types.h"
#include <unordered_map>
#include <vector>

class Room;

class PartySystem {
public:
    struct Party {
        PartyId id;
        PlayerId leader_id;
        std::vector<PlayerId> members;
    };

    void CreateParty(Room& room, PlayerId leader);
    void JoinParty(Room& room, PlayerId player, PartyId party_id);
    void LeaveParty(Room& room, PlayerId player);
    void OnPlayerLeave(Room& room, PlayerId player);

private:
    void BroadcastPartyUpdate(Room& room, PartyId party_id);

    std::unordered_map<PartyId, Party> parties_;
    PartyId next_id_ = 1;
};
