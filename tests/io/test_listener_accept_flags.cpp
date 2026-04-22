#include <iouring_runtime/core/Listener.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/IoRing.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>

using namespace std::chrono_literals;

using iouring_runtime::core::Address;
using iouring_runtime::core::buffer::BufferPool;
using iouring_runtime::core::io::Listener;
using iouring_runtime::core::ring::IoRing;
using iouring_runtime::core::ring::IoRingConfig;

namespace {

constexpr IoRingConfig kTestRingConfig{
    .queue_depth = 64,
    .buf_ring = {.buf_count = 16, .buf_size = 4096},
};

void DispatchUntil(IoRing& ring, auto&& pred,
                   std::chrono::milliseconds timeout = 2000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        ring.ProcessPostedTasks();
        ring.Dispatch(10ms);
    }
}

} // namespace

TEST(ListenerAcceptFlags, AcceptedSocketsAreNonBlocking) {
    auto ring_result = IoRing::Create(kTestRingConfig);
    ASSERT_TRUE(ring_result.has_value());
    auto ring = std::move(*ring_result);
    IoRing::SetCurrent(ring.get());

    BufferPool pool;
    std::atomic<bool> factory_called{false};
    std::atomic<int> accepted_flags{-1};

    auto listener = std::make_shared<Listener>(
        *ring, pool, Address{.host = "127.0.0.1", .port = 19882},
        [&](int fd, IoRing&, BufferPool&, iouring_runtime::core::ContextId) -> iouring_runtime::core::io::SessionRef {
            accepted_flags.store(::fcntl(fd, F_GETFL, 0), std::memory_order_relaxed);
            factory_called.store(true, std::memory_order_relaxed);
            return nullptr;
        },
        0);

    ASSERT_TRUE(listener->Start().has_value());

    int client_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19882);
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
    ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    DispatchUntil(*ring, [&] {
        return factory_called.load(std::memory_order_relaxed);
    });

    EXPECT_TRUE(factory_called.load(std::memory_order_relaxed));
    ASSERT_GE(accepted_flags.load(std::memory_order_relaxed), 0);
    EXPECT_NE(accepted_flags.load(std::memory_order_relaxed) & O_NONBLOCK, 0);

    listener->Stop();
    ::close(client_fd);
    IoRing::SetCurrent(nullptr);
}
