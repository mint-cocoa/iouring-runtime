#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/game/PacketSession.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace iouring_runtime::core::buffer;
using namespace iouring_runtime::core::ring;
using iouring_runtime::game::PacketId;
using iouring_runtime::game::PacketSession;

namespace {

constexpr IoRingConfig kRingConfig{
    .queue_depth = 64,
    .buf_ring = {.buf_count = 16, .buf_size = 4096},
};

struct SocketPair {
    int local = -1;
    int remote = -1;
};

SocketPair MakeSocketPair() {
    int sockets[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
    return {sockets[0], sockets[1]};
}

void WriteAll(int fd, std::span<const std::byte> data) {
    const auto* current = reinterpret_cast<const char*>(data.data());
    std::size_t remaining = data.size();
    while (remaining != 0) {
        const auto sent = ::write(fd, current, remaining);
        ASSERT_GT(sent, 0);
        current += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
}

void DispatchUntil(IoRing& ring, std::function<bool()> predicate,
                   std::chrono::milliseconds timeout =
                       std::chrono::milliseconds{2000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        ring.ProcessPostedTasks();
        ring.Dispatch(std::chrono::milliseconds{10});
    }
}

std::vector<std::byte> BuildPacket(std::uint16_t msg_id,
                                   std::span<const std::byte> payload) {
    const auto packet_size = static_cast<std::uint16_t>(4 + payload.size());
    std::vector<std::byte> packet(packet_size);
    std::memcpy(packet.data(), &packet_size, sizeof(packet_size));
    std::memcpy(packet.data() + 2, &msg_id, sizeof(msg_id));
    std::memcpy(packet.data() + 4, payload.data(), payload.size());
    return packet;
}

class TestPacketSession final : public PacketSession {
public:
    using PacketSession::PacketSession;

    std::atomic<int> packet_count{0};
    std::atomic<bool> disconnected{false};
    PacketId last_msg_id{};
    std::vector<std::byte> last_payload;

protected:
    void OnPacket(PacketId msg_id, const std::byte* data,
                  std::uint32_t len) override {
        last_msg_id = msg_id;
        last_payload.assign(data, data + len);
        packet_count.fetch_add(1, std::memory_order_relaxed);
    }

    void OnDisconnected() override {
        disconnected.store(true, std::memory_order_relaxed);
    }
};

class PacketSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto result = IoRing::Create(kRingConfig);
        ASSERT_TRUE(result.has_value());
        ring = std::move(*result);
        IoRing::SetCurrent(ring.get());
    }

    void TearDown() override {
        IoRing::SetCurrent(nullptr);
    }

    BufferPool pool;
    std::unique_ptr<IoRing> ring;
};

} // namespace

TEST_F(PacketSessionTest, ReassemblesFragmentedPacket) {
    auto sockets = MakeSocketPair();
    auto session = std::make_shared<TestPacketSession>(
        sockets.local, *ring, pool);
    session->Start();

    const std::vector<std::byte> payload{
        std::byte{0x10},
        std::byte{0x20},
        std::byte{0x30},
    };
    const auto packet = BuildPacket(101, payload);

    WriteAll(sockets.remote, std::span<const std::byte>(packet.data(), 2));
    DispatchUntil(*ring, [&] {
        return session->packet_count.load(std::memory_order_relaxed) != 0;
    }, std::chrono::milliseconds{100});
    EXPECT_EQ(session->packet_count.load(std::memory_order_relaxed), 0);

    WriteAll(sockets.remote,
             std::span<const std::byte>(packet.data() + 2, packet.size() - 2));
    DispatchUntil(*ring, [&] {
        return session->packet_count.load(std::memory_order_relaxed) == 1;
    });

    EXPECT_EQ(session->packet_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(session->last_msg_id, 101u);
    EXPECT_EQ(session->last_payload, payload);

    ::close(sockets.remote);
    DispatchUntil(*ring, [&] {
        return session->disconnected.load(std::memory_order_relaxed);
    });
}

TEST_F(PacketSessionTest, DisconnectsOnInvalidPacketSize) {
    auto sockets = MakeSocketPair();
    auto session = std::make_shared<TestPacketSession>(
        sockets.local, *ring, pool);
    session->Start();

    std::uint16_t invalid_size = 3;
    std::uint16_t msg_id = 101;
    std::byte packet[4];
    std::memcpy(packet, &invalid_size, sizeof(invalid_size));
    std::memcpy(packet + 2, &msg_id, sizeof(msg_id));

    WriteAll(sockets.remote, packet);
    DispatchUntil(*ring, [&] {
        return session->disconnected.load(std::memory_order_relaxed);
    });

    EXPECT_TRUE(session->disconnected.load(std::memory_order_relaxed));
    ::close(sockets.remote);
}
