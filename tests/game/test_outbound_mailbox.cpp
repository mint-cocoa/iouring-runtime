#include "../../app/examples/game/dungeon_full_server/src/net/outbound_mailbox.h"

#include <iouring_runtime/core/SendBuffer.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

iouring_runtime::core::buffer::SendBufferRef MakeBuffer(
    iouring_runtime::core::buffer::BufferPool& pool, std::byte value) {
    auto buffer = pool.Allocate(1);
    EXPECT_TRUE(buffer.has_value());
    if (!buffer) {
        return nullptr;
    }
    (*buffer)->Writable()[0] = value;
    (*buffer)->Commit(1);
    return *buffer;
}

OutboundMessage MakeMessage(iouring_runtime::core::buffer::BufferPool& pool,
                            std::uint64_t room_seq, std::byte value,
                            iouring_runtime::core::SessionId session_id = 42,
                            RoomId room_id = 7) {
    return OutboundMessage{
        .session_id = session_id,
        .room_id = room_id,
        .room_seq = room_seq,
        .buffer = MakeBuffer(pool, value),
    };
}

} // namespace

TEST(WorkerOutbox, PreservesRoomOrderWhenRemoteThenOwnerWorkerEnqueue) {
    iouring_runtime::core::buffer::BufferPool pool;
    WorkerOutbox outbox;

    std::atomic<bool> remote_enqueued{false};
    std::thread remote_room_worker([&] {
        outbox.Enqueue(MakeMessage(pool, 1, std::byte{0xA1}));
        remote_enqueued.store(true, std::memory_order_release);
    });

    while (!remote_enqueued.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // This models the old problematic case: the next room output is produced
    // on the target owner worker before that worker has drained the remote
    // output. With outbox enqueue, both messages still share one FIFO.
    outbox.Enqueue(MakeMessage(pool, 2, std::byte{0xB2}));
    remote_room_worker.join();

    std::vector<std::uint64_t> drained_seq;
    std::vector<std::byte> drained_payload;
    outbox.DrainBudgeted([&](OutboundMessage msg) {
        drained_seq.push_back(msg.room_seq);
        drained_payload.push_back(msg.buffer->Data()[0]);
    });

    EXPECT_EQ(drained_seq, (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(drained_payload, (std::vector<std::byte>{std::byte{0xA1},
                                                       std::byte{0xB2}}));
    EXPECT_EQ(outbox.Pending(), 0u);
}

TEST(WorkerOutbox, BudgetedDrainKeepsSessionFifoAcrossPhases) {
    iouring_runtime::core::buffer::BufferPool pool;
    WorkerOutbox outbox;

    outbox.Enqueue(MakeMessage(pool, 1, std::byte{0x01}));
    outbox.Enqueue(MakeMessage(pool, 2, std::byte{0x02}));
    outbox.Enqueue(MakeMessage(pool, 3, std::byte{0x03}));

    std::vector<std::uint64_t> drained_seq;
    outbox.DrainBudgeted([&](OutboundMessage msg) {
        drained_seq.push_back(msg.room_seq);
    }, 2);

    EXPECT_EQ(drained_seq, (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(outbox.Pending(), 1u);

    outbox.DrainBudgeted([&](OutboundMessage msg) {
        drained_seq.push_back(msg.room_seq);
    }, 2);

    EXPECT_EQ(drained_seq, (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(outbox.Pending(), 0u);
}

TEST(WorkerOutbox, OrdersSessionTasksAndPacketsTogether) {
    iouring_runtime::core::buffer::BufferPool pool;
    WorkerOutbox outbox;

    std::vector<int> observed;
    outbox.Enqueue(OutboundMessage{
        .session_id = 42,
        .room_id = 7,
        .room_seq = 1,
        .task = [&observed] { observed.push_back(1); },
    });
    outbox.Enqueue(MakeMessage(pool, 2, std::byte{0x02}));
    outbox.Enqueue(OutboundMessage{
        .session_id = 42,
        .room_id = 7,
        .room_seq = 3,
        .task = [&observed] { observed.push_back(3); },
    });

    outbox.DrainBudgeted([&](OutboundMessage msg) {
        if (msg.task) {
            msg.task();
        }
        if (msg.buffer) {
            observed.push_back(static_cast<int>(msg.room_seq));
        }
    });

    EXPECT_EQ(observed, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(outbox.Pending(), 0u);
}

TEST(WorkerOutbox, MaintainsIndependentFifoForFanoutSessions) {
    iouring_runtime::core::buffer::BufferPool pool;
    WorkerOutbox outbox;

    // Models a room broadcasting event #1 and #2 to two sessions. The worker
    // outbox may interleave sessions while draining, but each session must see
    // its own room stream in order.
    outbox.Enqueue(MakeMessage(pool, 1, std::byte{0x11}, 101));
    outbox.Enqueue(MakeMessage(pool, 1, std::byte{0x21}, 202));
    outbox.Enqueue(MakeMessage(pool, 2, std::byte{0x12}, 101));
    outbox.Enqueue(MakeMessage(pool, 2, std::byte{0x22}, 202));

    std::vector<std::pair<iouring_runtime::core::SessionId, std::uint64_t>>
        drained;
    outbox.DrainBudgeted([&](OutboundMessage msg) {
        drained.emplace_back(msg.session_id, msg.room_seq);
    });

    EXPECT_EQ(drained,
              (std::vector<std::pair<iouring_runtime::core::SessionId,
                                     std::uint64_t>>{
                  {101, 1},
                  {202, 1},
                  {101, 2},
                  {202, 2},
              }));
    EXPECT_EQ(outbox.Pending(), 0u);
}

TEST(WorkerOutbox, BudgetedDrainPreservesMultipleSessionOrder) {
    iouring_runtime::core::buffer::BufferPool pool;
    WorkerOutbox outbox;

    outbox.Enqueue(MakeMessage(pool, 1, std::byte{0x11}, 101));
    outbox.Enqueue(MakeMessage(pool, 1, std::byte{0x21}, 202));
    outbox.Enqueue(MakeMessage(pool, 2, std::byte{0x12}, 101));
    outbox.Enqueue(MakeMessage(pool, 2, std::byte{0x22}, 202));
    outbox.Enqueue(MakeMessage(pool, 3, std::byte{0x13}, 101));
    outbox.Enqueue(MakeMessage(pool, 3, std::byte{0x23}, 202));

    std::vector<std::uint64_t> sid101;
    std::vector<std::uint64_t> sid202;

    auto drain = [&](std::size_t budget) {
        outbox.DrainBudgeted([&](OutboundMessage msg) {
            if (msg.session_id == 101) {
                sid101.push_back(msg.room_seq);
            } else if (msg.session_id == 202) {
                sid202.push_back(msg.room_seq);
            }
        }, budget);
    };

    drain(1);
    drain(2);
    drain(3);

    EXPECT_EQ(sid101, (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(sid202, (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(outbox.Pending(), 0u);
}

TEST(WorkerOutbox, ConcurrentDifferentSessionsDrainWithoutLoss) {
    iouring_runtime::core::buffer::BufferPool pool;
    WorkerOutbox outbox;

    constexpr int kSessions = 4;
    constexpr int kMessagesPerSession = 250;

    std::vector<std::thread> producers;
    for (int session = 0; session < kSessions; ++session) {
        producers.emplace_back([&, session] {
            const auto sid =
                static_cast<iouring_runtime::core::SessionId>(1000 + session);
            const auto payload =
                static_cast<std::byte>(static_cast<unsigned char>(session));
            for (int i = 1; i <= kMessagesPerSession; ++i) {
                outbox.Enqueue(MakeMessage(
                    pool, static_cast<std::uint64_t>(i), payload, sid));
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    std::unordered_map<iouring_runtime::core::SessionId,
                       std::vector<std::uint64_t>>
        drained_by_session;
    outbox.DrainBudgeted([&](OutboundMessage msg) {
        drained_by_session[msg.session_id].push_back(msg.room_seq);
    }, kSessions * kMessagesPerSession);

    ASSERT_EQ(drained_by_session.size(), static_cast<std::size_t>(kSessions));
    for (int session = 0; session < kSessions; ++session) {
        const auto sid =
            static_cast<iouring_runtime::core::SessionId>(1000 + session);
        auto it = drained_by_session.find(sid);
        ASSERT_NE(it, drained_by_session.end());
        ASSERT_EQ(it->second.size(), static_cast<std::size_t>(kMessagesPerSession));
        for (int i = 1; i <= kMessagesPerSession; ++i) {
            EXPECT_EQ(it->second[static_cast<std::size_t>(i - 1)],
                      static_cast<std::uint64_t>(i));
        }
    }
    EXPECT_EQ(outbox.Pending(), 0u);
}
