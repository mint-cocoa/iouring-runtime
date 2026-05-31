#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/EventHandler.h>
#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>

using namespace iouring_runtime::core::ring;

class PollObject : public EventHandler {
public:
    bool poll_fired = false;
    int32_t last_result = 0;

protected:
    DispatchResult OnPoll(PollEvent& ev, int32_t result) override {
        poll_fired = true;
        last_result = result;
        return DispatchResult::kComplete;
    }
};

TEST(PollAddTest, EventFdTriggersPollin) {
    auto ring_result = IoRing::Create();
    ASSERT_TRUE(ring_result.has_value());
    auto& ring = *ring_result.value();
    IoRing::SetCurrent(&ring);

    int efd = eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(efd, 0);

    auto obj = std::make_shared<PollObject>();
    auto* poll_ev = new StrongPollEvent(obj);
    poll_ev->SetAutoDelete(true);

    ASSERT_TRUE(ring.PrepPollAdd(*poll_ev, efd, POLLIN));
    ring.Submit();

    uint64_t val = 1;
    write(efd, &val, sizeof(val));

    ring.Dispatch(std::chrono::milliseconds{100});
    EXPECT_TRUE(obj->poll_fired);
    EXPECT_GT(obj->last_result, 0);

    close(efd);
    IoRing::SetCurrent(nullptr);
}
