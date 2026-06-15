#include <iouring/event/GlobalQueue.h>
#include <iouring/game/RoomManager.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using iouring::event::GlobalQueue;
using iouring::game::Room;
using iouring::game::RoomManager;

namespace {

class CustomRoom final : public Room {
public:
    using Room::Room;
};

} // namespace

TEST(RoomManager, CreatesRoomsWithUniqueIds) {
    GlobalQueue global_queue;
    RoomManager manager(global_queue);

    auto first = manager.CreateRoom("alpha");
    auto second = manager.CreateRoom("beta");

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first->Id(), second->Id());
    EXPECT_EQ(first->Name(), "alpha");
    EXPECT_EQ(second->Name(), "beta");
    EXPECT_EQ(manager.RoomCount(), 2u);
    EXPECT_EQ(manager.NextRoomId(), 3u);
    EXPECT_EQ(manager.FindRoom(first->Id()), first);
    EXPECT_EQ(manager.FindRoom(second->Id()), second);
}

TEST(RoomManager, CreatesDerivedRoomsWithManagedIds) {
    GlobalQueue global_queue;
    RoomManager manager(global_queue);

    auto room = manager.CreateRoomAs<CustomRoom>("custom");
    static_assert(std::is_same_v<decltype(room), std::shared_ptr<CustomRoom>>);

    ASSERT_NE(room, nullptr);
    EXPECT_EQ(room->Id(), 1u);
    EXPECT_EQ(room->Name(), "custom");
    EXPECT_EQ(manager.FindRoom(room->Id()), room);
    EXPECT_EQ(manager.NextRoomId(), 2u);
}

TEST(RoomManager, AddsAndRejectsDuplicateRooms) {
    GlobalQueue global_queue;
    RoomManager manager(global_queue);
    auto room = std::make_shared<Room>(77, "custom", global_queue);

    EXPECT_TRUE(manager.AddRoom(room));
    EXPECT_FALSE(manager.AddRoom(std::make_shared<Room>(77, "duplicate",
                                                        global_queue)));
    EXPECT_FALSE(manager.AddRoom(nullptr));
    EXPECT_EQ(manager.RoomCount(), 1u);
    EXPECT_EQ(manager.FindRoom(77), room);
}

TEST(RoomManager, ReturnsOriginalStyleRoomList) {
    GlobalQueue global_queue;
    RoomManager manager(global_queue);

    auto room = manager.CreateRoom("alpha");
    room->AddPlayer({.player_id = 42});

    auto room_list = manager.GetRoomList();

    ASSERT_EQ(room_list.size(), 1u);
    EXPECT_EQ(room_list.front().id, room->Id());
    EXPECT_EQ(room_list.front().name, "alpha");
    EXPECT_EQ(room_list.front().player_count, 1u);
    EXPECT_EQ(room_list.front().max_players, 500u);
}

TEST(RoomManager, RemovesRoomsAndReturnsSnapshots) {
    GlobalQueue global_queue;
    RoomManager manager(global_queue);

    auto first = manager.CreateRoom("alpha");
    auto second = manager.CreateRoom("beta");
    auto rooms = manager.Rooms();

    EXPECT_EQ(rooms.size(), 2u);
    EXPECT_TRUE(manager.RemoveRoom(first->Id()));
    EXPECT_FALSE(manager.RemoveRoom(first->Id()));
    EXPECT_EQ(manager.FindRoom(first->Id()), nullptr);
    EXPECT_EQ(manager.FindRoom(second->Id()), second);
    EXPECT_EQ(manager.RoomCount(), 1u);
}
