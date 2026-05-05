#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/EventHandler.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>

using namespace iouring_runtime::core::ring;
using namespace std::chrono_literals;

namespace {

// Minimal EventHandler that captures whatever OnTimeout receives.
class TimeoutObserver : public EventHandler {
public:
    bool fired = false;
    std::int32_t last_result = 0;

protected:
    void OnTimeout(TimeoutEvent& /*ev*/, std::int32_t result) override {
        fired = true;
        last_result = result;
    }
};

} // namespace

TEST(IoRingTimeout, FiresEtimeAtRequestedDuration) {
    auto ring_result = IoRing::Create();
    ASSERT_TRUE(ring_result.has_value());
    auto& ring = *ring_result.value();
    IoRing::SetCurrent(&ring);

    auto obs = std::make_shared<TimeoutObserver>();
    auto* ev = new TimeoutEvent();
    ev->SetStrongOwner(obs);
    ev->SetAutoDelete(true);

    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(ring.PrepTimeout(*ev, 50ms));
    ring.Submit();

    // Wait up to 500ms for the CQE — should really come back in ~50ms.
    ring.Dispatch(500ms);

    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(obs->fired);
    EXPECT_EQ(obs->last_result, -ETIME)
        << "timeout CQE should report -ETIME, got " << obs->last_result;
    EXPECT_GE(elapsed, 40ms);
    EXPECT_LT(elapsed, 400ms);

    IoRing::SetCurrent(nullptr);
}

TEST(IoRingTimeout, CancellationReportsEcanceled) {
    auto ring_result = IoRing::Create();
    ASSERT_TRUE(ring_result.has_value());
    auto& ring = *ring_result.value();
    IoRing::SetCurrent(&ring);

    auto obs = std::make_shared<TimeoutObserver>();
    auto* ev = new TimeoutEvent();
    ev->SetStrongOwner(obs);
    ev->SetAutoDelete(true);

    // Arm a long timeout, then immediately cancel it. The CQE should arrive
    // with -ECANCELED rather than -ETIME.
    ASSERT_TRUE(ring.PrepTimeout(*ev, 10s));
    ring.Submit();
    ASSERT_TRUE(ring.PrepCancel(*ev));
    ring.Submit();

    ring.Dispatch(500ms);

    EXPECT_TRUE(obs->fired);
    EXPECT_EQ(obs->last_result, -ECANCELED)
        << "cancelled timeout should report -ECANCELED, got " << obs->last_result;

    IoRing::SetCurrent(nullptr);
}

TEST(IoRingTimeout, DispatchFlushesDeferredSubmissions) {
    IoRingConfig cfg;
    cfg.submit_batch_size = 2;

    auto ring_result = IoRing::Create(cfg);
    ASSERT_TRUE(ring_result.has_value());
    auto& ring = *ring_result.value();
    IoRing::SetCurrent(&ring);

    auto obs = std::make_shared<TimeoutObserver>();
    auto* ev = new TimeoutEvent();
    ev->SetStrongOwner(obs);
    ev->SetAutoDelete(true);

    ASSERT_TRUE(ring.PrepTimeout(*ev, 10ms));
    EXPECT_EQ(ring.Submit(), 0) << "first submit should stay deferred";

    ring.Dispatch(200ms);

    EXPECT_TRUE(obs->fired);
    EXPECT_EQ(obs->last_result, -ETIME);

    IoRing::SetCurrent(nullptr);
}

TEST(IoRingTimeout, DispatchHonorsCqeBatchBudgetAcrossPolls) {
    IoRingConfig cfg;
    cfg.cqe_batch_budget = 1;

    auto ring_result = IoRing::Create(cfg);
    ASSERT_TRUE(ring_result.has_value());
    auto& ring = *ring_result.value();
    IoRing::SetCurrent(&ring);

    auto first = std::make_shared<TimeoutObserver>();
    auto second = std::make_shared<TimeoutObserver>();
    auto* first_ev = new TimeoutEvent();
    auto* second_ev = new TimeoutEvent();
    first_ev->SetStrongOwner(first);
    second_ev->SetStrongOwner(second);
    first_ev->SetAutoDelete(true);
    second_ev->SetAutoDelete(true);

    ASSERT_TRUE(ring.PrepTimeout(*first_ev, 1ms));
    ASSERT_TRUE(ring.PrepTimeout(*second_ev, 1ms));
    EXPECT_GE(ring.Submit(), 0);

    ring.Dispatch(200ms);
    EXPECT_TRUE(first->fired || second->fired);
    EXPECT_FALSE(first->fired && second->fired)
        << "budget=1 should leave one ready CQE for the next dispatch";

    ring.Dispatch(200ms);
    EXPECT_TRUE(first->fired);
    EXPECT_TRUE(second->fired);
    EXPECT_EQ(first->last_result, -ETIME);
    EXPECT_EQ(second->last_result, -ETIME);

    IoRing::SetCurrent(nullptr);
}
