#include <iouring_runtime/game/PlayerRegistry.h>
#include <iouring_runtime/game/Room.h>

#include <gtest/gtest.h>

using iouring_runtime::game::PacketId;
using iouring_runtime::game::PlayerRegistry;
using iouring_runtime::game::RoomLike;

namespace {

class TestRoom final : public RoomLike {
public:
    void HandlePacket(iouring_runtime::game::PlayerId, PacketId,
                      std::span<const std::byte>) override {}
};

} // namespace

TEST(PlayerRegistry, RegistersAndFindsBySessionAndPlayer) {
    PlayerRegistry registry;

    EXPECT_TRUE(registry.Register(10, 42));
    EXPECT_FALSE(registry.Register(10, 43));
    EXPECT_FALSE(registry.Register(11, 42));
    EXPECT_FALSE(registry.Register(0, 44));
    EXPECT_FALSE(registry.Register(12, 0));

    auto by_session = registry.FindBySession(10);
    ASSERT_TRUE(by_session.has_value());
    EXPECT_EQ(by_session->session_id, 10u);
    EXPECT_EQ(by_session->player_id, 42u);

    auto by_player = registry.FindByPlayer(42);
    ASSERT_TRUE(by_player.has_value());
    EXPECT_EQ(by_player->session_id, 10u);
    EXPECT_EQ(registry.PlayerCount(), 1u);
}

TEST(PlayerRegistry, BindsAndClearsRoom) {
    PlayerRegistry registry;
    TestRoom room;

    ASSERT_TRUE(registry.Register(10, 42));
    EXPECT_TRUE(registry.BindRoomBySession(10, 7, &room));
    EXPECT_FALSE(registry.BindRoomBySession(99, 7, &room));
    EXPECT_FALSE(registry.BindRoomByPlayer(42, 0, &room));
    EXPECT_FALSE(registry.BindRoomByPlayer(42, 7, nullptr));

    auto record = registry.FindByPlayer(42);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->room_id, 7u);
    EXPECT_EQ(record->room, &room);

    EXPECT_TRUE(registry.ClearRoomByPlayer(42));
    record = registry.FindBySession(10);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->room_id, 0u);
    EXPECT_EQ(record->room, nullptr);
}

TEST(PlayerRegistry, UnregistersBothIndexesAndReturnsLastRecord) {
    PlayerRegistry registry;
    TestRoom room;

    ASSERT_TRUE(registry.Register(10, 42));
    ASSERT_TRUE(registry.BindRoomByPlayer(42, 7, &room));

    auto removed = registry.UnregisterBySession(10);
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed->player_id, 42u);
    EXPECT_EQ(removed->room_id, 7u);
    EXPECT_EQ(removed->room, &room);
    EXPECT_FALSE(registry.FindBySession(10).has_value());
    EXPECT_FALSE(registry.FindByPlayer(42).has_value());
    EXPECT_EQ(registry.PlayerCount(), 0u);

    ASSERT_TRUE(registry.Register(11, 43));
    removed = registry.UnregisterByPlayer(43);
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed->session_id, 11u);
    EXPECT_FALSE(registry.UnregisterByPlayer(43).has_value());
}

TEST(PlayerRegistry, ReturnsSnapshotRecords) {
    PlayerRegistry registry;

    ASSERT_TRUE(registry.Register(10, 42));
    ASSERT_TRUE(registry.Register(11, 43));

    auto records = registry.Records();
    EXPECT_EQ(records.size(), 2u);
}
