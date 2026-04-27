#include "chat_system.h"
#include "../game/room.h"
#include "Social.pb.h"
#include "Enum.pb.h"

void ChatSystem::ProcessChat(Room& room, PlayerState& sender, const game::C_Chat& msg) {
    game::S_Chat reply;
    reply.set_chat_type(msg.chat_type());
    reply.set_sender_id(sender.player_id);
    reply.set_sender_name(sender.name);
    reply.set_message(msg.message());
    reply.set_target_name(msg.target_name());

    switch (msg.chat_type()) {
        case game::CHAT_ALL:
        case game::CHAT_ZONE:
            room.BroadcastAll(MsgId::S_CHAT, reply);
            break;

        case game::CHAT_PARTY: {
            if (sender.party_id == 0) return;
            for (auto& [_, ps] : room.Players()) {
                if (ps.party_id == sender.party_id) {
                    room.SendTo(ps, MsgId::S_CHAT, reply);
                }
            }
            break;
        }

        case game::CHAT_WHISPER: {
            // 같은 룸 내에서만 귓속말
            for (auto& [_, ps] : room.Players()) {
                if (ps.name == msg.target_name()) {
                    room.SendTo(ps, MsgId::S_CHAT, reply);
                    room.SendTo(sender, MsgId::S_CHAT, reply);
                    return;
                }
            }
            break;
        }

        default:
            break;
    }
}
