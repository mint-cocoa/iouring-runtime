#include <iouring/event/GlobalQueue.h>
#include <iouring/game/Room.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

using iouring::event::GlobalQueue;
using iouring::game::PacketId;
using iouring::game::PlayerId;
using iouring::game::PlayerState;
using iouring::game::Room;

namespace {

class TestRoom final : public Room {
public:
    using Room::Room;

    std::atomic<int> move_count{0};
    PlayerId last_player_id = 0;
    PacketId last_msg_id{};
    std::vector<std::byte> last_payload;

protected:
    void OnPacket(PlayerState& player, PacketId msg_id,
                  std::span<const std::byte> payload) override {
        last_player_id = player.player_id;
        last_msg_id = msg_id;
        last_payload.assign(payload.begin(), payload.end());
        move_count.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace

TEST(Room, QueuesAndDispatchesInRoomPackets) {
    GlobalQueue global_queue;
    TestRoom room(7, "alpha", global_queue);
    room.AddPlayer(PlayerState{.player_id = 42});

    EXPECT_FALSE(room.IsEmpty());
    EXPECT_FALSE(room.IsFull());

    const std::vector<std::byte> payload{std::byte{0x10}, std::byte{0x20}};
    room.HandlePacket(42, 201, payload);

    auto* queued = global_queue.TryPop();
    ASSERT_EQ(queued, &room);
    queued->Execute();

    EXPECT_EQ(room.move_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(room.last_player_id, 42u);
    EXPECT_EQ(room.last_msg_id, 201u);
    EXPECT_EQ(room.last_payload, payload);

    room.RemovePlayer(42);
    EXPECT_TRUE(room.IsEmpty());
}

TEST(Room, IgnoresPacketsFromUnknownPlayers) {
    GlobalQueue global_queue;
    TestRoom room(7, "alpha", global_queue);

    room.HandlePacket(42, 201, {});

    auto* queued = global_queue.TryPop();
    ASSERT_EQ(queued, &room);
    queued->Execute();

    EXPECT_EQ(room.move_count.load(std::memory_order_relaxed), 0);
}

TEST(Room, DispatchesAnyPacketIdToGenericHook) {
    GlobalQueue global_queue;
    TestRoom room(7, "alpha", global_queue);
    room.AddPlayer(PlayerState{.player_id = 42});

    room.HandlePacket(42, 119, {});
    ASSERT_EQ(global_queue.TryPop(), &room);
    room.Execute();
    EXPECT_EQ(room.move_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(room.last_msg_id, 119u);

    room.HandlePacket(42, 102, {});
    ASSERT_EQ(global_queue.TryPop(), &room);
    room.Execute();
    EXPECT_EQ(room.move_count.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(room.last_msg_id, 102u);
}
