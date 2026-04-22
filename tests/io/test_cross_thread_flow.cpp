#include <iouring_runtime/core/SendQueue.h>
#include <iouring_runtime/core/MpscQueue.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

using namespace iouring_runtime::core;
using namespace iouring_runtime::core::buffer;

// ── Lightweight test message types (no GameServer dependency) ─

struct TestZoneMsg {
    std::uint8_t type;
    std::uint64_t player_id;
    std::uint16_t source_reactor;
    std::uint16_t seq;  // per-shard sequence number
};

struct TestIOJob {
    std::uint8_t type;
    std::uint64_t player_id;
    std::uint32_t payload_size;
};

// ── Shard→Zone: MPSC queue tests ─────────────────────────────

// 4 Shard threads → 1 Zone queue, each sending 10,000 messages.
// Verifies total count + per-shard FIFO ordering.
TEST(CrossThreadFlow, MultiShardToSingleZone) {
    constexpr int kShards = 4;
    constexpr int kMsgsPerShard = 10000;

    MpscQueue<TestZoneMsg> zone_queue;
    std::atomic<int> ready{0};

    std::vector<std::thread> shard_threads;
    for (int s = 0; s < kShards; ++s) {
        shard_threads.emplace_back([&, s] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (ready.load(std::memory_order_acquire) < kShards) {}

            for (int i = 0; i < kMsgsPerShard; ++i) {
                zone_queue.Push(TestZoneMsg{
                    .type = 1,
                    .player_id = static_cast<std::uint64_t>(s * kMsgsPerShard + i),
                    .source_reactor = static_cast<std::uint16_t>(s),
                    .seq = static_cast<std::uint16_t>(i),
                });
            }
        });
    }

    for (auto& t : shard_threads) t.join();

    // Drain and verify
    std::vector<int> last_seq(kShards, -1);
    int total = 0;
    zone_queue.Drain([&](TestZoneMsg&& msg) {
        int s = msg.source_reactor;
        ASSERT_GE(s, 0);
        ASSERT_LT(s, kShards);
        EXPECT_GT(static_cast<int>(msg.seq), last_seq[s])
            << "Shard " << s << " FIFO violation";
        last_seq[s] = msg.seq;
        ++total;
    });

    EXPECT_EQ(total, kShards * kMsgsPerShard);
}

// Push and Drain execute concurrently. No messages may be lost.
TEST(CrossThreadFlow, ConcurrentDrainWithPush) {
    constexpr int kProducers = 4;
    constexpr int kItemsPerProducer = 50000;

    MpscQueue<TestZoneMsg> queue;
    std::atomic<bool> done{false};
    std::vector<TestZoneMsg> consumed;
    consumed.reserve(kProducers * kItemsPerProducer);

    // Consumer thread (single)
    std::thread consumer([&] {
        while (!done.load(std::memory_order_acquire)) {
            queue.Drain([&](TestZoneMsg&& msg) {
                consumed.push_back(std::move(msg));
            });
            std::this_thread::yield();
        }
        // Final drain
        queue.Drain([&](TestZoneMsg&& msg) {
            consumed.push_back(std::move(msg));
        });
    });

    // Producer threads
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kItemsPerProducer; ++i) {
                queue.Push(TestZoneMsg{
                    .type = 0,
                    .player_id = static_cast<std::uint64_t>(p * kItemsPerProducer + i),
                    .source_reactor = static_cast<std::uint16_t>(p),
                    .seq = static_cast<std::uint16_t>(i % 65536),
                });
            }
        });
    }

    for (auto& t : producers) t.join();
    done.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(static_cast<int>(consumed.size()), kProducers * kItemsPerProducer);

    // Verify all unique player_ids present
    std::vector<std::uint64_t> ids;
    ids.reserve(consumed.size());
    for (auto& m : consumed) ids.push_back(m.player_id);
    std::sort(ids.begin(), ids.end());
    for (int i = 0; i < kProducers * kItemsPerProducer; ++i) {
        EXPECT_EQ(ids[i], static_cast<std::uint64_t>(i));
    }
}

// 2 Zone threads → 4 Shard queues, concurrent Push.
// All messages received.
TEST(CrossThreadFlow, ZoneBroadcastToMultipleShards) {
    constexpr int kZones = 2;
    constexpr int kShards = 4;
    constexpr int kMsgsPerZone = 10000;

    std::array<MpscQueue<TestIOJob>, kShards> shard_queues;
    std::atomic<int> ready{0};

    std::vector<std::thread> zone_threads;
    for (int z = 0; z < kZones; ++z) {
        zone_threads.emplace_back([&, z] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (ready.load(std::memory_order_acquire) < kZones) {}

            for (int i = 0; i < kMsgsPerZone; ++i) {
                // Broadcast: push to all shard queues
                for (int s = 0; s < kShards; ++s) {
                    shard_queues[s].Push(TestIOJob{
                        .type = static_cast<std::uint8_t>(z),
                        .player_id = static_cast<std::uint64_t>(z * kMsgsPerZone + i),
                        .payload_size = 64,
                    });
                }
            }
        });
    }

    for (auto& t : zone_threads) t.join();

    // Each shard should receive kZones * kMsgsPerZone messages
    for (int s = 0; s < kShards; ++s) {
        int count = 0;
        shard_queues[s].Drain([&](TestIOJob&&) { ++count; });
        EXPECT_EQ(count, kZones * kMsgsPerZone)
            << "Shard " << s << " message count mismatch";
    }
}

// 4 Zones × 4 Shards × 1,000 broadcasts = 16,000 msgs per shard.
// Verifies integrity under heavy contention.
TEST(CrossThreadFlow, BroadcastUnderContention) {
    constexpr int kZones = 4;
    constexpr int kShards = 4;
    constexpr int kBroadcastsPerZone = 1000;

    std::array<MpscQueue<TestIOJob>, kShards> shard_queues;
    std::atomic<int> ready{0};

    std::vector<std::thread> zone_threads;
    for (int z = 0; z < kZones; ++z) {
        zone_threads.emplace_back([&, z] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (ready.load(std::memory_order_acquire) < kZones) {}

            for (int i = 0; i < kBroadcastsPerZone; ++i) {
                for (int s = 0; s < kShards; ++s) {
                    shard_queues[s].Push(TestIOJob{
                        .type = static_cast<std::uint8_t>(z),
                        .player_id = static_cast<std::uint64_t>(z * kBroadcastsPerZone + i),
                        .payload_size = 32,
                    });
                }
            }
        });
    }

    for (auto& t : zone_threads) t.join();

    const int expected_per_shard = kZones * kBroadcastsPerZone;
    for (int s = 0; s < kShards; ++s) {
        std::vector<std::uint64_t> ids;
        shard_queues[s].Drain([&](TestIOJob&& job) {
            ids.push_back(job.player_id);
        });
        EXPECT_EQ(static_cast<int>(ids.size()), expected_per_shard)
            << "Shard " << s << " total mismatch";

        // Verify per-zone ordering: within each zone's messages,
        // player_ids should be monotonically increasing
        std::vector<std::uint64_t> last_id(kZones, 0);
        std::vector<bool> first_seen(kZones, true);

        shard_queues[s].Drain([](TestIOJob&&) {}); // already drained above

        // Re-verify on the ids vector: group by zone (type field encodes zone)
        // Since we already drained, verify on collected ids using source zone info
        // Actually the ids encode zone info: zone z has ids [z*1000, z*1000+999]
        for (auto id : ids) {
            int zone = static_cast<int>(id / kBroadcastsPerZone);
            ASSERT_GE(zone, 0);
            ASSERT_LT(zone, kZones);
            if (!first_seen[zone]) {
                EXPECT_GT(id, last_id[zone])
                    << "Zone " << zone << " FIFO violation on shard " << s;
            }
            first_seen[zone] = false;
            last_id[zone] = id;
        }
    }
}

// ── SendQueue concurrency tests ──────────────────────────────

// N threads Push + 1 thread Drain/MarkSent.
// needs_register accuracy and overflow correctness.
TEST(CrossThreadFlow, SendQueueConcurrency) {
    constexpr int kPushers = 4;
    constexpr int kItemsPerPusher = 10000;
    constexpr std::uint32_t kMaxPending = 256;

    BufferPool pool;
    SendQueue queue(kMaxPending);

    std::atomic<int> ready{0};
    std::atomic<int> total_pushed{0};
    std::atomic<int> total_overflow{0};
    std::atomic<int> total_needs_register{0};

    // Push threads (simulate game threads calling Connection::Send)
    std::vector<std::thread> pushers;
    for (int p = 0; p < kPushers; ++p) {
        pushers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (ready.load(std::memory_order_acquire) < kPushers + 1) {}

            for (int i = 0; i < kItemsPerPusher; ++i) {
                auto buf_result = pool.Allocate(64);
                if (!buf_result) continue;
                auto buf = std::move(*buf_result);
                std::memset(buf->Writable().data(), 0, 64);
                buf->Commit(64);

                auto result = queue.Push(std::move(buf));
                if (result.overflowed) {
                    total_overflow.fetch_add(1, std::memory_order_relaxed);
                } else {
                    total_pushed.fetch_add(1, std::memory_order_relaxed);
                    if (result.needs_register)
                        total_needs_register.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Drain thread (simulate IO thread)
    std::atomic<bool> done{false};
    int total_drained = 0;
    std::thread drainer([&] {
        ready.fetch_add(1, std::memory_order_relaxed);
        while (ready.load(std::memory_order_acquire) < kPushers + 1) {}

        while (!done.load(std::memory_order_acquire)) {
            auto bufs = queue.Drain();
            total_drained += static_cast<int>(bufs.size());
            if (!bufs.empty()) {
                // Simulate send completion
                queue.MarkSent();
            }
            // Small yield to allow pushers to fill queue
            std::this_thread::yield();
        }

        // Final drain
        auto bufs = queue.Drain();
        total_drained += static_cast<int>(bufs.size());
    });

    for (auto& t : pushers) t.join();
    done.store(true, std::memory_order_release);
    drainer.join();

    // Every pushed (non-overflow) item must be drained
    EXPECT_EQ(total_drained, total_pushed.load());

    // needs_register must fire at least once (first push)
    EXPECT_GE(total_needs_register.load(), 1);

    // Total = pushed + overflow should equal total attempts
    EXPECT_EQ(total_pushed.load() + total_overflow.load(),
              kPushers * kItemsPerPusher);
}

// Verify needs_register fires exactly once per Drain/MarkSent cycle
// in a single-threaded scenario.
TEST(CrossThreadFlow, SendQueueNeedsRegisterAccuracy) {
    BufferPool pool;
    SendQueue queue(256);

    // First push → needs_register
    auto buf1 = *pool.Allocate(64);
    buf1->Commit(64);
    auto r1 = queue.Push(std::move(buf1));
    EXPECT_TRUE(r1.needs_register);
    EXPECT_FALSE(r1.overflowed);

    // Second push (before drain) → no needs_register
    auto buf2 = *pool.Allocate(64);
    buf2->Commit(64);
    auto r2 = queue.Push(std::move(buf2));
    EXPECT_FALSE(r2.needs_register);
    EXPECT_FALSE(r2.overflowed);

    // Drain
    auto drained = queue.Drain();
    EXPECT_EQ(drained.size(), 2u);

    // MarkSent resets the cycle
    queue.MarkSent();

    // Next push → needs_register again
    auto buf3 = *pool.Allocate(64);
    buf3->Commit(64);
    auto r3 = queue.Push(std::move(buf3));
    EXPECT_TRUE(r3.needs_register);
    EXPECT_FALSE(r3.overflowed);

    // Drain + push-before-MarkSent
    auto drained2 = queue.Drain();
    EXPECT_EQ(drained2.size(), 1u);

    // Push while drain is done but before MarkSent
    auto buf4 = *pool.Allocate(64);
    buf4->Commit(64);
    auto r4 = queue.Push(std::move(buf4));
    EXPECT_FALSE(r4.needs_register); // still registered

    // MarkSent sees pending not empty → stays registered
    queue.MarkSent();

    // Another push → still no needs_register (pending was not empty at MarkSent)
    auto buf5 = *pool.Allocate(64);
    buf5->Commit(64);
    auto r5 = queue.Push(std::move(buf5));
    EXPECT_FALSE(r5.needs_register);

    // Drain all remaining
    auto drained3 = queue.Drain();
    EXPECT_EQ(drained3.size(), 2u);
    queue.MarkSent();
}

// Verify overflow is reported when queue is at capacity.
TEST(CrossThreadFlow, SendQueueOverflowDetection) {
    constexpr std::uint32_t kMax = 8;
    BufferPool pool;
    SendQueue queue(kMax);

    // Fill to capacity
    for (std::uint32_t i = 0; i < kMax; ++i) {
        auto buf = *pool.Allocate(64);
        buf->Commit(64);
        auto r = queue.Push(std::move(buf));
        EXPECT_FALSE(r.overflowed) << "Overflow at index " << i;
    }

    // Next push should overflow
    auto buf = *pool.Allocate(64);
    buf->Commit(64);
    auto r = queue.Push(std::move(buf));
    EXPECT_TRUE(r.overflowed);

    // Drain brings it back
    auto drained = queue.Drain();
    EXPECT_EQ(drained.size(), kMax);

    queue.MarkSent();

    // Can push again
    auto buf2 = *pool.Allocate(64);
    buf2->Commit(64);
    auto r2 = queue.Push(std::move(buf2));
    EXPECT_TRUE(r2.needs_register);
    EXPECT_FALSE(r2.overflowed);
}

TEST(CrossThreadFlow, SendQueueStatsTrackCurrentPeakAndOverflow) {
    constexpr std::uint32_t kMax = 2;
    SendQueue queue(kMax);
    BufferPool pool;

    auto make_buf = [&]() {
        auto result = pool.Allocate(16);
        EXPECT_TRUE(result.has_value());
        auto buf = std::move(*result);
        std::memset(buf->Writable().data(), 0, 16);
        buf->Commit(16);
        return buf;
    };

    auto r1 = queue.Push(make_buf());
    EXPECT_FALSE(r1.overflowed);
    auto s1 = queue.Snapshot();
    EXPECT_EQ(s1.current_depth, 1u);
    EXPECT_EQ(s1.peak_depth, 1u);
    EXPECT_EQ(s1.overflow_count, 0u);

    auto r2 = queue.Push(make_buf());
    EXPECT_FALSE(r2.overflowed);
    auto s2 = queue.Snapshot();
    EXPECT_EQ(s2.current_depth, 2u);
    EXPECT_EQ(s2.peak_depth, 2u);
    EXPECT_EQ(s2.overflow_count, 0u);

    auto r3 = queue.Push(make_buf());
    EXPECT_TRUE(r3.overflowed);
    auto s3 = queue.Snapshot();
    EXPECT_EQ(s3.current_depth, 2u);
    EXPECT_EQ(s3.peak_depth, 2u);
    EXPECT_EQ(s3.overflow_count, 1u);

    auto drained = queue.Drain();
    EXPECT_EQ(drained.size(), 2u);
    queue.MarkSent();

    auto s4 = queue.Snapshot();
    EXPECT_EQ(s4.current_depth, 0u);
    EXPECT_EQ(s4.peak_depth, 2u);
    EXPECT_EQ(s4.overflow_count, 1u);
}
