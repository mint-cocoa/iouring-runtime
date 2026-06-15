#include <iouring/net/Session.h>
#include <iouring/event/IoRing.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace iouring::core;
using namespace iouring::net;
using namespace iouring::event;
using namespace iouring::core::buffer;

class TestSession : public Session {
public:
    using Session::Session;

    std::atomic<bool> connected{false};
    std::atomic<bool> disconnected{false};
    std::atomic<int> disconnect_count{0};
    std::atomic<int> recv_count{0};
    std::atomic<int> recv_bytes{0};

protected:
    void OnRecv(std::span<const std::byte> data) override {
        recv_count.fetch_add(1, std::memory_order_relaxed);
        recv_bytes.fetch_add(static_cast<int>(data.size()), std::memory_order_relaxed);
    }

    void OnConnected() override {
        connected.store(true, std::memory_order_relaxed);
    }

    void OnDisconnected() override {
        disconnected.store(true, std::memory_order_relaxed);
        disconnect_count.fetch_add(1, std::memory_order_relaxed);
    }
};

static constexpr IoRingConfig kTestRingConfig{
    .queue_depth = 64,
    .buf_ring = {.buf_count = 16, .buf_size = 4096},
};

static void DispatchUntil(IoRing& ring, std::function<bool()> pred,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        ring.ProcessPostedTasks();
        ring.Dispatch(std::chrono::milliseconds{10});
    }
}

struct SocketPair {
    int local;
    int remote;
};

static SocketPair MakeSocketPair() {
    int sv[2];
    int ret = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv);
    EXPECT_EQ(ret, 0);
    return {sv[0], sv[1]};
}

class SessionLifecycleTest : public ::testing::Test {
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

TEST_F(SessionLifecycleTest, NormalDisconnect) {
    auto [local_fd, remote_fd] = MakeSocketPair();

    auto sess = std::make_shared<TestSession>(local_fd, *ring_, pool_);
    sess->Start();

    EXPECT_TRUE(sess->connected.load());
    EXPECT_FALSE(sess->disconnected.load());

    ::close(remote_fd);

    DispatchUntil(*ring_, [&] { return sess->disconnected.load(); });

    EXPECT_TRUE(sess->disconnected.load());
    EXPECT_EQ(sess->disconnect_count.load(), 1);
    EXPECT_EQ(sess.use_count(), 1);
}

TEST_F(SessionLifecycleTest, SendThenPeerClose) {
    auto [local_fd, remote_fd] = MakeSocketPair();

    auto sess = std::make_shared<TestSession>(local_fd, *ring_, pool_);
    sess->Start();

    auto buf_result = pool_.Allocate(128);
    ASSERT_TRUE(buf_result.has_value());
    auto buf = std::move(*buf_result);
    std::memset(buf->Writable().data(), 0xAB, 128);
    buf->Commit(128);
    ASSERT_TRUE(sess->Send(std::move(buf)).has_value());

    ring_->Dispatch(std::chrono::milliseconds{50});

    std::byte tmp[256];
    ::recv(remote_fd, tmp, sizeof(tmp), 0);

    ::close(remote_fd);

    DispatchUntil(*ring_, [&] { return sess->disconnected.load(); });

    EXPECT_TRUE(sess->disconnected.load());
    EXPECT_EQ(sess->disconnect_count.load(), 1);
    EXPECT_EQ(sess.use_count(), 1);
}

TEST_F(SessionLifecycleTest, SendFromExternalThreadMarshalsToOwnerRing) {
    auto [local_fd, remote_fd] = MakeSocketPair();

    auto sess = std::make_shared<TestSession>(local_fd, *ring_, pool_);
    sess->Start();

    auto buf_result = pool_.Allocate(32);
    ASSERT_TRUE(buf_result.has_value());
    auto buf = std::move(*buf_result);
    std::thread sender([&, buf = std::move(buf)]() mutable {
        std::memcpy(buf->Writable().data(), "owner-marshal", 13);
        buf->Commit(13);
        EXPECT_TRUE(sess->Send(std::move(buf)).has_value());
    });
    sender.join();

    std::array<char, 32> received{};
    std::size_t total = 0;
    DispatchUntil(*ring_, [&] {
        auto n = ::recv(remote_fd, received.data() + total,
                        received.size() - total, MSG_DONTWAIT);
        if (n > 0) {
            total += static_cast<std::size_t>(n);
        }
        return total == 13;
    });

    EXPECT_EQ(total, 13u);
    EXPECT_EQ(std::string_view(received.data(), 13), "owner-marshal");

    ::close(remote_fd);
    DispatchUntil(*ring_, [&] { return sess->disconnected.load(); });
}

TEST_F(SessionLifecycleTest, DisconnectFromExternalThreadMarshalsToOwnerRing) {
    auto [local_fd, remote_fd] = MakeSocketPair();

    auto sess = std::make_shared<TestSession>(local_fd, *ring_, pool_);
    sess->Start();

    std::thread closer([&] {
        sess->Disconnect();
    });
    closer.join();

    DispatchUntil(*ring_, [&] { return sess->disconnected.load(); });

    EXPECT_TRUE(sess->Disconnecting());
    EXPECT_TRUE(sess->disconnected.load());
    EXPECT_EQ(sess->disconnect_count.load(), 1);

    ::close(remote_fd);
}

TEST_F(SessionLifecycleTest, InterleavedDisconnectSourcesNotifyOnce) {
    auto [local_fd, remote_fd] = MakeSocketPair();

    auto sess = std::make_shared<TestSession>(local_fd, *ring_, pool_);
    sess->Start();
    EXPECT_EQ(ring_->Sessions().Count(), 1u);

    auto buf_result = pool_.Allocate(512 * 1024);
    ASSERT_TRUE(buf_result.has_value());
    auto buf = std::move(*buf_result);
    std::memset(buf->Writable().data(), 0xA5, 512 * 1024);
    buf->Commit(512 * 1024);
    ASSERT_TRUE(sess->Send(std::move(buf)).has_value());

    std::thread closer([sess] {
        sess->Disconnect();
    });
    ::close(remote_fd);
    closer.join();

    DispatchUntil(*ring_, [&] {
        return sess->disconnected.load() && ring_->Sessions().Count() == 0;
    });

    EXPECT_TRUE(sess->Disconnecting());
    EXPECT_TRUE(sess->disconnected.load());
    EXPECT_EQ(sess->disconnect_count.load(), 1);
    EXPECT_EQ(ring_->Sessions().Count(), 0u);
}

TEST_F(SessionLifecycleTest, MixedShutdownDrainsWithoutExternalOwner) {
    auto [local_fd, remote_fd] = MakeSocketPair();

    std::weak_ptr<TestSession> weak;
    {
        auto sess = std::make_shared<TestSession>(local_fd, *ring_, pool_);
        weak = sess;
        sess->Start();
        EXPECT_EQ(ring_->Sessions().Count(), 1u);

        auto buf_result = pool_.Allocate(128 * 1024);
        ASSERT_TRUE(buf_result.has_value());
        auto buf = std::move(*buf_result);
        std::memset(buf->Writable().data(), 0x5A, 128 * 1024);
        buf->Commit(128 * 1024);
        ASSERT_TRUE(sess->Send(std::move(buf)).has_value());

        sess->Disconnect();
    }

    ::close(remote_fd);

    DispatchUntil(*ring_, [&] {
        return weak.expired() && ring_->Sessions().Count() == 0;
    });

    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(ring_->Sessions().Count(), 0u);
}

TEST(SessionLifecycleDrainGateTest, FirstDisconnectCqeDoesNotReleaseSession) {
    IoRingConfig config{
        .queue_depth = 64,
        .buf_ring = {.buf_count = 16, .buf_size = 4096},
        .cqe_batch_budget = 1,
    };
    auto ring_result = IoRing::Create(config);
    ASSERT_TRUE(ring_result.has_value());
    auto ring = std::move(*ring_result);
    IoRing::SetCurrent(ring.get());

    BufferPool pool;
    auto [local_fd, remote_fd] = MakeSocketPair();
    auto sess = std::make_shared<TestSession>(local_fd, *ring, pool);
    sess->Start();
    ASSERT_EQ(ring->Sessions().Count(), 1u);

    sess->Disconnect();

    ring->Dispatch(std::chrono::milliseconds{500});

    EXPECT_TRUE(sess->Disconnecting());
    EXPECT_FALSE(sess->disconnected.load());
    EXPECT_EQ(sess->disconnect_count.load(), 0);
    EXPECT_EQ(ring->Sessions().Count(), 1u);

    DispatchUntil(*ring, [&] {
        return sess->disconnected.load() && ring->Sessions().Count() == 0;
    });

    EXPECT_TRUE(sess->disconnected.load());
    EXPECT_EQ(sess->disconnect_count.load(), 1);
    EXPECT_EQ(ring->Sessions().Count(), 0u);

    ::close(remote_fd);
    IoRing::SetCurrent(nullptr);
}

TEST_F(SessionLifecycleTest, SendQueueOverflowDisconnects) {
    auto [local_fd, remote_fd] = MakeSocketPair();

    auto sess = std::make_shared<TestSession>(local_fd, *ring_, pool_, 1);
    sess->Start();

    bool saw_overflow = false;
    for (int i = 0; i < 16; ++i) {
        auto buf_result = pool_.Allocate(64);
        ASSERT_TRUE(buf_result.has_value());
        auto buf = std::move(*buf_result);
        std::memset(buf->Writable().data(), 0, 64);
        buf->Commit(64);
        auto result = sess->Send(std::move(buf));
        if (!result.has_value()) {
            saw_overflow = true;
            break;
        }
    }

    EXPECT_TRUE(saw_overflow);

    DispatchUntil(*ring_, [&] { return sess->disconnected.load(); });

    EXPECT_TRUE(sess->Disconnecting());
    EXPECT_TRUE(sess->disconnected.load());

    ::close(remote_fd);
}

TEST_F(SessionLifecycleTest, ManagerRelease) {
    auto [local_fd, remote_fd] = MakeSocketPair();

    std::weak_ptr<TestSession> weak;
    {
        auto sess = std::make_shared<TestSession>(local_fd, *ring_, pool_);
        weak = sess;
        sess->Start();
        EXPECT_TRUE(sess->connected.load());
    }

    ASSERT_FALSE(weak.expired());

    ::close(remote_fd);

    DispatchUntil(*ring_, [&] { return weak.expired(); });

    EXPECT_TRUE(weak.expired());
}
