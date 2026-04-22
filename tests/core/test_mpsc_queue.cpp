#include <iouring_runtime/core/MpscQueue.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

using namespace iouring_runtime::core;

TEST(MpscQueue, EmptyOnConstruction) {
    MpscQueue<int> q;
    std::vector<int> results;
    q.Drain([&](int&& val) { results.push_back(val); });
    EXPECT_TRUE(results.empty());
}

TEST(MpscQueue, SinglePushPop) {
    MpscQueue<int> q;
    q.Push(42);

    std::vector<int> results;
    q.Drain([&](int&& val) { results.push_back(val); });
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0], 42);
}

TEST(MpscQueue, FifoOrder) {
    MpscQueue<int> q;
    for (int i = 0; i < 100; ++i)
        q.Push(i);

    std::vector<int> results;
    q.Drain([&](int&& val) { results.push_back(val); });
    ASSERT_EQ(results.size(), 100u);
    for (int i = 0; i < 100; ++i)
        EXPECT_EQ(results[i], i);
}

TEST(MpscQueue, DrainAll) {
    MpscQueue<int> q;
    for (int i = 0; i < 50; ++i)
        q.Push(i);

    std::vector<int> results;
    std::size_t count = q.Drain([&](int&& val) {
        results.push_back(val);
    });

    EXPECT_EQ(count, 50u);
    EXPECT_EQ(results.size(), 50u);
    for (int i = 0; i < 50; ++i)
        EXPECT_EQ(results[i], i);
}

TEST(MpscQueue, MoveOnlyType) {
    MpscQueue<std::unique_ptr<int>> q;
    q.Push(std::make_unique<int>(99));

    std::vector<std::unique_ptr<int>> results;
    q.Drain([&](std::unique_ptr<int>&& val) {
        results.push_back(std::move(val));
    });
    ASSERT_EQ(results.size(), 1u);
    ASSERT_NE(results[0], nullptr);
    EXPECT_EQ(*results[0], 99);
}

TEST(MpscQueue, MultiProducerSingleConsumer) {
    constexpr int kProducers = 8;
    constexpr int kItemsPerProducer = 10000;

    MpscQueue<int> q;
    std::atomic<int> ready{0};

    // Producers: each pushes [producer_id * kItemsPerProducer, ...)
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (ready.load(std::memory_order_relaxed) < kProducers) {}

            int base = p * kItemsPerProducer;
            for (int i = 0; i < kItemsPerProducer; ++i)
                q.Push(base + i);
        });
    }

    for (auto& t : producers)
        t.join();

    // Consumer: drain everything
    std::vector<int> results;
    results.reserve(kProducers * kItemsPerProducer);
    q.Drain([&](int&& val) { results.push_back(val); });

    EXPECT_EQ(results.size(), static_cast<size_t>(kProducers * kItemsPerProducer));

    // Each value should appear exactly once
    std::sort(results.begin(), results.end());
    std::vector<int> expected(kProducers * kItemsPerProducer);
    std::iota(expected.begin(), expected.end(), 0);
    EXPECT_EQ(results, expected);
}

TEST(MpscQueue, ConcurrentPushAndPop) {
    // Producers push while consumer drains concurrently
    constexpr int kProducers = 4;
    constexpr int kItemsPerProducer = 50000;

    MpscQueue<int> q;
    std::atomic<bool> done{false};
    std::atomic<int> producers_finished{0};
    std::vector<int> consumed;
    consumed.reserve(kProducers * kItemsPerProducer);

    // Consumer thread
    std::thread consumer([&] {
        while (!done.load(std::memory_order_relaxed)) {
            q.Drain([&](int&& val) { consumed.push_back(val); });
            std::this_thread::yield();
        }
        // Final drain
        q.Drain([&](int&& val) { consumed.push_back(val); });
    });

    // Producer threads
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            int base = p * kItemsPerProducer;
            for (int i = 0; i < kItemsPerProducer; ++i)
                q.Push(base + i);
            producers_finished.fetch_add(1, std::memory_order_relaxed);
        });
    }

    for (auto& t : producers)
        t.join();

    // Signal consumer that producers are done
    done.store(true, std::memory_order_relaxed);
    consumer.join();

    EXPECT_EQ(consumed.size(), static_cast<size_t>(kProducers * kItemsPerProducer));

    // Verify all values present
    std::sort(consumed.begin(), consumed.end());
    std::vector<int> expected(kProducers * kItemsPerProducer);
    std::iota(expected.begin(), expected.end(), 0);
    EXPECT_EQ(consumed, expected);
}

TEST(MpscQueue, PerProducerFifoOrder) {
    // Each producer's items should appear in order relative to each other
    constexpr int kProducers = 4;
    constexpr int kItemsPerProducer = 10000;

    MpscQueue<std::pair<int, int>> q; // {producer_id, sequence}

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kItemsPerProducer; ++i)
                q.Push({p, i});
        });
    }
    for (auto& t : producers)
        t.join();

    // Track last seen sequence per producer
    std::vector<int> last_seq(kProducers, -1);
    int total = 0;
    q.Drain([&](std::pair<int, int>&& item) {
        auto [pid, seq] = item;
        EXPECT_GT(seq, last_seq[pid]) << "Producer " << pid << " out of order";
        last_seq[pid] = seq;
        ++total;
    });

    EXPECT_EQ(total, kProducers * kItemsPerProducer);
}
