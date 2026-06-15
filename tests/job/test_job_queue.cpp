#include <iouring/event/GlobalQueue.h>
#include <iouring/event/JobQueue.h>
#include <iouring/event/JobTimer.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace iouring::event;

// ── Basic functionality ──────────────────────────────────────

TEST(JobQueueTest, PushAndExecute) {
    GlobalQueue gq;
    JobQueue jq(gq);

    int executed = 0;
    jq.Push([&] { ++executed; });

    // Push should register with GlobalQueue
    auto* popped = gq.TryPop();
    ASSERT_EQ(popped, &jq);

    popped->Execute();
    EXPECT_EQ(executed, 1);

    // GlobalQueue should be empty now
    EXPECT_EQ(gq.TryPop(), nullptr);
}

TEST(JobQueueTest, BatchDrain) {
    GlobalQueue gq;
    JobQueue jq(gq);

    int count = 0;
    for (int i = 0; i < 100; ++i)
        jq.Push([&] { ++count; });

    // Should be registered once (first Push triggers gq.Push)
    auto* popped = gq.TryPop();
    ASSERT_EQ(popped, &jq);
    EXPECT_EQ(gq.TryPop(), nullptr); // only one registration

    popped->Execute();
    EXPECT_EQ(count, 100); // all 100 processed in one Execute call
    EXPECT_EQ(gq.TryPop(), nullptr);
}

TEST(JobQueueTest, ExecuteWithDeadline) {
    GlobalQueue gq;
    JobQueue jq(gq);

    std::atomic<int> count{0};
    for (int i = 0; i < 1000; ++i)
        jq.Push([&] {
            ++count;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        });

    auto* popped = gq.TryPop();
    ASSERT_EQ(popped, &jq);

    // Execute with very short deadline — should not finish all 1000
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    popped->Execute(deadline);

    int first_batch = count.load();
    EXPECT_GT(first_batch, 0);
    EXPECT_LT(first_batch, 1000);

    // Should have re-registered with GlobalQueue
    auto* popped2 = gq.TryPop();
    ASSERT_NE(popped2, nullptr);

    // Execute remaining without deadline
    popped2->Execute();
    EXPECT_EQ(count.load(), 1000);
}

TEST(JobQueueTest, PushDuringExecute) {
    GlobalQueue gq;
    JobQueue jq(gq);

    int phase1 = 0;
    int phase2 = 0;

    // First job pushes more jobs during execution
    jq.Push([&] {
        ++phase1;
        jq.Push([&] { ++phase2; });
        jq.Push([&] { ++phase2; });
    });

    auto* popped = gq.TryPop();
    ASSERT_NE(popped, nullptr);
    popped->Execute();

    EXPECT_EQ(phase1, 1);
    EXPECT_EQ(phase2, 2); // Jobs pushed during Execute are processed in the same cycle
}

TEST(JobQueueTest, TimerIntegration) {
    GlobalQueue gq;
    JobTimer timer;
    auto jq = std::make_shared<JobQueue>(gq);

    int tick_count = 0;
    timer.Reserve(std::chrono::milliseconds(0), jq, [&] { ++tick_count; });

    timer.DistributeExpired();

    auto* popped = gq.TryPop();
    ASSERT_NE(popped, nullptr);
    popped->Execute();
    EXPECT_EQ(tick_count, 1);
}

// ── Multi-worker serialization guarantee ─────────────────────

// Multiple "worker" threads compete to TryPop and Execute the same JobQueue.
// Jobs increment a counter with a concurrent-access check.
// If serialization is broken, active_count would exceed 1.
TEST(JobQueueTest, MultiWorkerSerialization) {
    GlobalQueue gq;
    JobQueue jq(gq);

    constexpr int kJobs = 10000;
    constexpr int kWorkers = 4;

    std::atomic<int> total{0};
    std::atomic<int> active{0};
    std::atomic<bool> violation{false};

    // Push all jobs
    for (int i = 0; i < kJobs; ++i) {
        jq.Push([&] {
            int prev = active.fetch_add(1, std::memory_order_relaxed);
            if (prev != 0)
                violation.store(true, std::memory_order_relaxed);
            total.fetch_add(1, std::memory_order_relaxed);
            active.fetch_sub(1, std::memory_order_relaxed);
        });
    }

    // Simulate multiple workers competing
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                auto* popped = gq.TryPop();
                if (!popped) {
                    std::this_thread::yield();
                    continue;
                }
                popped->Execute();
            }
        });
    }

    // Wait for all jobs to complete
    while (total.load(std::memory_order_relaxed) < kJobs)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    stop.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();

    EXPECT_EQ(total.load(), kJobs);
    EXPECT_FALSE(violation.load()) << "Concurrent execution detected!";
}

// Push from multiple threads while workers Execute. No jobs lost.
TEST(JobQueueTest, ConcurrentPushAndExecute) {
    GlobalQueue gq;
    JobQueue jq(gq);

    constexpr int kPushers = 4;
    constexpr int kJobsPerPusher = 5000;
    constexpr int kWorkers = 4;
    constexpr int kTotalJobs = kPushers * kJobsPerPusher;

    std::atomic<int> total{0};
    std::atomic<int> active{0};
    std::atomic<bool> violation{false};
    std::atomic<bool> stop{false};

    // Worker threads
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                auto* popped = gq.TryPop();
                if (!popped) {
                    std::this_thread::yield();
                    continue;
                }
                popped->Execute();
            }
            // Drain remaining
            while (auto* popped = gq.TryPop())
                popped->Execute();
        });
    }

    // Pusher threads (start after workers)
    std::vector<std::thread> pushers;
    for (int p = 0; p < kPushers; ++p) {
        pushers.emplace_back([&] {
            for (int i = 0; i < kJobsPerPusher; ++i) {
                jq.Push([&] {
                    int prev = active.fetch_add(1, std::memory_order_relaxed);
                    if (prev != 0)
                        violation.store(true, std::memory_order_relaxed);
                    total.fetch_add(1, std::memory_order_relaxed);
                    active.fetch_sub(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (auto& p : pushers) p.join();

    // Wait for all jobs
    while (total.load(std::memory_order_relaxed) < kTotalJobs)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    stop.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();

    EXPECT_EQ(total.load(), kTotalJobs);
    EXPECT_FALSE(violation.load()) << "Concurrent execution detected!";
}

// Multiple independent JobQueues processed by multiple workers.
// Each JobQueue maintains its own serialization.
TEST(JobQueueTest, MultipleQueuesMultipleWorkers) {
    GlobalQueue gq;

    constexpr int kQueues = 8;
    constexpr int kJobsPerQueue = 2000;
    constexpr int kWorkers = 4;

    std::vector<std::unique_ptr<JobQueue>> queues;
    std::vector<std::atomic<int>> totals(kQueues);
    std::vector<std::atomic<int>> actives(kQueues);
    std::atomic<bool> violation{false};

    for (int q = 0; q < kQueues; ++q) {
        queues.push_back(std::make_unique<JobQueue>(gq));
        totals[q].store(0);
        actives[q].store(0);
    }

    // Push jobs to all queues
    for (int q = 0; q < kQueues; ++q) {
        for (int i = 0; i < kJobsPerQueue; ++i) {
            queues[q]->Push([&, q] {
                int prev = actives[q].fetch_add(1, std::memory_order_relaxed);
                if (prev != 0)
                    violation.store(true, std::memory_order_relaxed);
                totals[q].fetch_add(1, std::memory_order_relaxed);
                actives[q].fetch_sub(1, std::memory_order_relaxed);
            });
        }
    }

    // Workers
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                auto* popped = gq.TryPop();
                if (!popped) {
                    std::this_thread::yield();
                    continue;
                }
                popped->Execute();
            }
            while (auto* popped = gq.TryPop())
                popped->Execute();
        });
    }

    // Wait
    auto all_done = [&] {
        for (int q = 0; q < kQueues; ++q)
            if (totals[q].load(std::memory_order_relaxed) < kJobsPerQueue)
                return false;
        return true;
    };

    while (!all_done())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    stop.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();

    for (int q = 0; q < kQueues; ++q)
        EXPECT_EQ(totals[q].load(), kJobsPerQueue) << "Queue " << q;

    EXPECT_FALSE(violation.load()) << "Concurrent execution detected!";
}
