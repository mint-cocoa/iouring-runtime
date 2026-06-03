#include <iouring_runtime/core/IoRing.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <memory>
#include <thread>

using namespace iouring_runtime::core::ring;
using namespace std::chrono_literals;

namespace {

// Minimal observer that captures whatever the timeout completion receives.
class TimeoutObserver : public std::enable_shared_from_this<TimeoutObserver> {
public:
    bool fired = false;
    std::int32_t last_result = 0;

    DispatchResult OnTimeout(TimeoutEvent&, std::int32_t result) {
        fired = true;
        last_result = result;
        return DispatchResult::kComplete;
    }
};

struct OwnerLifetimeState {
    int timeout_callbacks = 0;
    int cancel_callbacks = 0;
    int destroyed = 0;
    bool in_callback = false;
    bool destroyed_during_callback = false;
};

class OwnerLifetimeObserver : public std::enable_shared_from_this<OwnerLifetimeObserver> {
public:
    OwnerLifetimeObserver(std::shared_ptr<OwnerLifetimeState> state,
                          bool clear_first_timeout_owner)
        : state_(std::move(state))
        , clear_first_timeout_owner_(clear_first_timeout_owner) {}

    ~OwnerLifetimeObserver() {
        if (state_->in_callback) {
            state_->destroyed_during_callback = true;
        }
        ++state_->destroyed;
    }

    DispatchResult OnTimeout(TimeoutEvent& ev, std::int32_t result) {
        state_->in_callback = true;
        ++state_->timeout_callbacks;
        if (clear_first_timeout_owner_ && state_->timeout_callbacks == 1) {
            ev.ClearKeepAlive();
            EXPECT_EQ(state_->destroyed, 0)
                << "dispatch-local owner should keep handler alive";
        }
        EXPECT_TRUE(result == -ETIME || result == -ECANCELED);
        state_->in_callback = false;
        return DispatchResult::kComplete;
    }

    DispatchResult OnCancel(CancelEvent&, std::int32_t) {
        state_->in_callback = true;
        ++state_->cancel_callbacks;
        state_->in_callback = false;
        return DispatchResult::kComplete;
    }

private:
    std::shared_ptr<OwnerLifetimeState> state_;
    bool clear_first_timeout_owner_;
};

template <class ObserverT>
TimeoutEvent* MakeTimeoutEvent(const std::shared_ptr<ObserverT>& observer) {
    auto* ev = new TimeoutEvent();
    BindCompletion(*ev, observer, &ObserverT::OnTimeout);
    ev->SetAutoDelete(true);
    return ev;
}

template <class ObserverT>
CancelEvent* MakeCancelEvent(const std::shared_ptr<ObserverT>& observer,
                             IoEvent* target) {
    auto* ev = new CancelEvent(target);
    BindCompletion(*ev, observer, &ObserverT::OnCancel);
    ev->SetAutoDelete(true);
    return ev;
}

} // namespace

TEST(IoRingTimeout, FiresEtimeAtRequestedDuration) {
    auto ring_result = IoRing::Create();
    ASSERT_TRUE(ring_result.has_value());
    auto& ring = *ring_result.value();
    IoRing::SetCurrent(&ring);

    auto obs = std::make_shared<TimeoutObserver>();
    auto* ev = MakeTimeoutEvent(obs);

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
    auto* ev = MakeTimeoutEvent(obs);

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
    auto* ev = MakeTimeoutEvent(obs);

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
    auto* first_ev = MakeTimeoutEvent(first);
    auto* second_ev = MakeTimeoutEvent(second);

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

TEST(IoRingOwnerLifetime, DispatchLocalOwnerSurvivesEventOwnerReset) {
    auto ring_result = IoRing::Create();
    ASSERT_TRUE(ring_result.has_value());
    auto& ring = *ring_result.value();
    IoRing::SetCurrent(&ring);

    auto state = std::make_shared<OwnerLifetimeState>();
    auto observer = std::make_shared<OwnerLifetimeObserver>(state, true);
    auto* ev = MakeTimeoutEvent(observer);
    observer.reset();

    ASSERT_TRUE(ring.PrepTimeout(*ev, 1ms));
    ASSERT_GE(ring.Submit(), 0);
    ring.Dispatch(200ms);

    EXPECT_EQ(state->timeout_callbacks, 1);
    EXPECT_EQ(state->cancel_callbacks, 0);
    EXPECT_FALSE(state->destroyed_during_callback);
    EXPECT_EQ(state->destroyed, 1);

    IoRing::SetCurrent(nullptr);
}

TEST(IoRingOwnerLifetime, ReadyCqesUseIndependentEventOwners) {
    auto ring_result = IoRing::Create();
    ASSERT_TRUE(ring_result.has_value());
    auto& ring = *ring_result.value();
    IoRing::SetCurrent(&ring);

    auto state = std::make_shared<OwnerLifetimeState>();
    auto observer = std::make_shared<OwnerLifetimeObserver>(state, true);
    auto* first_ev = MakeTimeoutEvent(observer);
    auto* second_ev = MakeTimeoutEvent(observer);
    observer.reset();

    ASSERT_TRUE(ring.PrepTimeout(*first_ev, 1ms));
    ASSERT_TRUE(ring.PrepTimeout(*second_ev, 1ms));
    ASSERT_GE(ring.Submit(), 0);

    std::this_thread::sleep_for(20ms);
    ring.Dispatch(200ms);

    EXPECT_EQ(state->timeout_callbacks, 2);
    EXPECT_EQ(state->cancel_callbacks, 0);
    EXPECT_FALSE(state->destroyed_during_callback);
    EXPECT_EQ(state->destroyed, 1);

    IoRing::SetCurrent(nullptr);
}

TEST(IoRingOwnerLifetime, CancelRequestAndTargetHaveIndependentOwners) {
    auto ring_result = IoRing::Create();
    ASSERT_TRUE(ring_result.has_value());
    auto& ring = *ring_result.value();
    IoRing::SetCurrent(&ring);

    auto state = std::make_shared<OwnerLifetimeState>();
    auto observer = std::make_shared<OwnerLifetimeObserver>(state, false);
    auto* timeout_ev = MakeTimeoutEvent(observer);
    auto* cancel_ev = MakeCancelEvent(observer, timeout_ev);
    observer.reset();

    ASSERT_TRUE(ring.PrepTimeout(*timeout_ev, 10s));
    ASSERT_GE(ring.Submit(), 0);
    ASSERT_TRUE(ring.PrepCancel(*timeout_ev, cancel_ev));
    ASSERT_GE(ring.Submit(), 0);

    auto deadline = std::chrono::steady_clock::now() + 1s;
    while ((state->timeout_callbacks != 1 || state->cancel_callbacks != 1) &&
           std::chrono::steady_clock::now() < deadline) {
        ring.Dispatch(100ms);
    }

    EXPECT_EQ(state->timeout_callbacks, 1);
    EXPECT_EQ(state->cancel_callbacks, 1);
    EXPECT_FALSE(state->destroyed_during_callback);
    EXPECT_EQ(state->destroyed, 1);

    IoRing::SetCurrent(nullptr);
}
