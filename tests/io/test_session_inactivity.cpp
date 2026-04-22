#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/IoRing.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

using namespace iouring_runtime::core;
using namespace iouring_runtime::core::io;
using namespace iouring_runtime::core::ring;
using namespace iouring_runtime::core::buffer;
using namespace std::chrono_literals;

namespace {

class TestSession : public Session {
public:
    using Session::Session;

    std::atomic<bool> disconnected{false};

protected:
    void OnRecv(std::span<const std::byte> /*data*/) override {}
    void OnDisconnected() override {
        disconnected.store(true, std::memory_order_relaxed);
    }
};

constexpr IoRingConfig kTestRingConfig{
    .queue_depth = 64,
    .buf_ring = {.buf_count = 16, .buf_size = 4096},
};

struct SocketPair {
    int local;
    int remote;
};

SocketPair MakeSocketPair() {
    int sv[2];
    int ret = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv);
    EXPECT_EQ(ret, 0);
    return {sv[0], sv[1]};
}

void DispatchUntil(IoRing& ring, std::function<bool()> pred,
                   std::chrono::milliseconds timeout = 2000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        ring.ProcessPostedTasks();
        ring.Dispatch(10ms);
    }
}

class InactivityTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto result = IoRing::Create(kTestRingConfig);
        ASSERT_TRUE(result.has_value());
        ring_ = std::move(*result);
        IoRing::SetCurrent(ring_.get());
    }

    void TearDown() override {
        IoRing::SetCurrent(nullptr);
    }

    BufferPool pool_;
    std::unique_ptr<IoRing> ring_;
};

} // namespace

TEST_F(InactivityTest, IdleSessionDisconnectsAfterThreshold) {
    auto [local_fd, remote_fd] = MakeSocketPair();

    auto sess = std::make_shared<TestSession>(local_fd, *ring_, pool_);
    sess->SetInactivityTimeout(200ms);  // check_interval clamps to 100ms
    sess->Start();

    // No data ever arrives on local_fd. The watchdog should disconnect
    // somewhere between 200ms and ~350ms (200ms threshold + up to one
    // 100ms tick of slack).
    auto start = std::chrono::steady_clock::now();
    DispatchUntil(*ring_, [&] { return sess->disconnected.load(); }, 1000ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(sess->disconnected.load());
    EXPECT_GE(elapsed, 150ms);
    EXPECT_LT(elapsed, 700ms);

    ::close(remote_fd);
}

TEST_F(InactivityTest, ContinuousTrafficKeepsSessionAlive) {
    auto [local_fd, remote_fd] = MakeSocketPair();

    auto sess = std::make_shared<TestSession>(local_fd, *ring_, pool_);
    sess->SetInactivityTimeout(200ms);
    sess->Start();

    // Drip a byte every 50ms for ~500ms — well under the 200ms idle
    // threshold per gap. The watchdog must not fire.
    auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < deadline) {
        char b = 'x';
        ::write(remote_fd, &b, 1);
        ring_->ProcessPostedTasks();
        ring_->Dispatch(50ms);
    }

    EXPECT_FALSE(sess->disconnected.load());

    ::close(remote_fd);
    DispatchUntil(*ring_, [&] { return sess->disconnected.load(); });
    EXPECT_TRUE(sess->disconnected.load()) << "peer close should still disconnect";
}
