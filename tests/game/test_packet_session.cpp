#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/game/PacketSession.h>

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <span>
#include <string_view>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace iouring_runtime::core::buffer;
using namespace iouring_runtime::core::ring;
using iouring_runtime::game::PacketId;
using iouring_runtime::game::PacketBuilder;
using iouring_runtime::game::PacketSession;

namespace {

constexpr IoRingConfig kRingConfig{
    .queue_depth = 64,
    .buf_ring = {.buf_count = 512, .buf_size = 4096},
};

struct SocketPair {
    int local = -1;
    int remote = -1;
};

struct TcpPair {
    int server = -1;
    int client = -1;
};

SocketPair MakeSocketPair() {
    int sockets[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
    return {sockets[0], sockets[1]};
}

TcpPair MakeTcpPair() {
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(listener, 0);

    int one = 1;
    EXPECT_EQ(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one,
                           sizeof(one)), 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)), 0);
    EXPECT_EQ(::listen(listener, 1), 0);

    socklen_t address_len = sizeof(address);
    EXPECT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                            &address_len), 0);

    const int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(client, 0);
    EXPECT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&address),
                        sizeof(address)), 0);

    const int server = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    EXPECT_GE(server, 0);
    ::close(listener);
    return {server, client};
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

void PumpOnce(IoRing& ring) {
    ring.ProcessPostedTasks();
    ring.Dispatch(std::chrono::milliseconds{0});
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

std::uint16_t ReadLe16(const std::byte* data) {
    std::uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

class TestPacketSession final : public PacketSession {
public:
    using PacketSession::PacketSession;

    std::atomic<int> packet_count{0};
    std::atomic<bool> disconnected{false};
    PacketId last_msg_id{};
    std::vector<std::byte> last_payload;
    std::vector<PacketId> msg_ids;
    std::vector<std::vector<std::byte>> payloads;
    std::vector<const std::byte*> payload_pointers;

    void Feed(std::span<const std::byte> data) {
        OnRecv(data);
    }

protected:
    void OnPacket(PacketId msg_id, const std::byte* data,
                  std::uint32_t len) override {
        last_msg_id = msg_id;
        last_payload.assign(data, data + len);
        msg_ids.push_back(msg_id);
        payloads.emplace_back(data, data + len);
        payload_pointers.push_back(data);
        packet_count.fetch_add(1, std::memory_order_relaxed);
    }

    void OnDisconnected() override {
        disconnected.store(true, std::memory_order_relaxed);
    }
};

struct PacketProbeStats {
    std::uint64_t recv_views = 0;
    std::uint64_t bytes = 0;
    std::uint64_t packets = 0;
    std::uint64_t packets_split_across_views = 0;
    std::uint64_t continuation_views = 0;
    std::uint32_t max_view_size = 0;
};

class PacketBoundaryProbeSession final
    : public iouring_runtime::core::io::Session {
public:
    using Session::Session;

    PacketProbeStats stats;
    std::atomic<bool> disconnected{false};

protected:
    void OnRecv(std::span<const std::byte> data) override {
        ++stats.recv_views;
        stats.bytes += data.size();
        stats.max_view_size =
            std::max(stats.max_view_size, static_cast<std::uint32_t>(data.size()));

        if (HeaderBytesRead() != 0 || packet_bytes_read_ != 0) {
            ++stats.continuation_views;
            current_packet_split_ = true;
        }

        std::size_t offset = 0;
        while (offset < data.size()) {
            if (HeaderBytesRead() == 0 && packet_bytes_read_ == 0) {
                expected_packet_size_ = 0;
                current_packet_split_ = false;
            }

            while (HeaderBytesRead() < header_.size() && offset < data.size()) {
                header_[header_bytes_read_++] = data[offset++];
                ++packet_bytes_read_;
            }

            if (HeaderBytesRead() < header_.size()) {
                break;
            }

            if (expected_packet_size_ == 0) {
                expected_packet_size_ = ReadLe16(header_.data());
                if (expected_packet_size_ < PacketBuilder::kHeaderSize) {
                    Disconnect();
                    return;
                }
            }

            const auto remaining_packet_bytes =
                expected_packet_size_ - packet_bytes_read_;
            const auto available = data.size() - offset;
            const auto consumed =
                std::min<std::size_t>(remaining_packet_bytes, available);
            offset += consumed;
            packet_bytes_read_ += static_cast<std::uint32_t>(consumed);

            if (packet_bytes_read_ != expected_packet_size_) {
                break;
            }

            ++stats.packets;
            if (current_packet_split_) {
                ++stats.packets_split_across_views;
            }
            header_bytes_read_ = 0;
            packet_bytes_read_ = 0;
            expected_packet_size_ = 0;
            current_packet_split_ = false;
        }
    }

    void OnDisconnected() override {
        disconnected.store(true, std::memory_order_relaxed);
    }

private:
    static std::uint16_t ReadLe16(const std::byte* data) {
        std::uint16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }

    std::size_t HeaderBytesRead() const {
        return header_bytes_read_;
    }

    std::array<std::byte, PacketBuilder::kHeaderSize> header_{};
    std::uint32_t header_bytes_read_ = 0;
    std::uint32_t packet_bytes_read_ = 0;
    std::uint32_t expected_packet_size_ = 0;
    bool current_packet_split_ = false;
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

std::vector<std::byte> BuildPayload(std::size_t size) {
    std::vector<std::byte> payload(size);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i & 0xff);
    }
    return payload;
}

std::vector<std::vector<std::byte>> BuildPacketSet(std::size_t count,
                                                   std::size_t payload_size) {
    std::vector<std::vector<std::byte>> packets;
    packets.reserve(count);
    const auto payload = BuildPayload(payload_size);
    for (std::size_t i = 0; i < count; ++i) {
        packets.push_back(BuildPacket(static_cast<std::uint16_t>(100 + i),
                                      payload));
    }
    return packets;
}

std::vector<std::byte> FlattenPackets(
    const std::vector<std::vector<std::byte>>& packets) {
    const auto total_size = std::accumulate(
        packets.begin(), packets.end(), std::size_t{0},
        [](std::size_t total, const auto& packet) {
            return total + packet.size();
        });
    std::vector<std::byte> all;
    all.reserve(total_size);
    for (const auto& packet : packets) {
        all.insert(all.end(), packet.begin(), packet.end());
    }
    return all;
}

bool ContainsPointer(std::span<const std::byte> owner,
                     const std::byte* data,
                     std::size_t len) {
    const auto begin = reinterpret_cast<std::uintptr_t>(owner.data());
    const auto end = begin + owner.size();
    const auto current = reinterpret_cast<std::uintptr_t>(data);
    return current >= begin && current + len <= end;
}

std::vector<std::span<const std::byte>> BuildBulkViews(
    std::span<const std::byte> data, std::size_t view_size) {
    std::vector<std::span<const std::byte>> views;
    for (std::size_t offset = 0; offset < data.size(); offset += view_size) {
        const auto len = std::min(view_size, data.size() - offset);
        views.emplace_back(data.data() + offset, len);
    }
    return views;
}

std::vector<std::span<const std::byte>> BuildPacketViews(
    std::span<const std::byte> data, std::size_t packet_size) {
    std::vector<std::span<const std::byte>> views;
    for (std::size_t offset = 0; offset < data.size(); offset += packet_size) {
        views.emplace_back(data.data() + offset, packet_size);
    }
    return views;
}

std::vector<std::span<const std::byte>> BuildSplitHeaderBodyViews(
    std::span<const std::byte> data, std::size_t packet_size) {
    std::vector<std::span<const std::byte>> views;
    for (std::size_t offset = 0; offset < data.size(); offset += packet_size) {
        views.emplace_back(data.data() + offset, 2);
        views.emplace_back(data.data() + offset + 2, packet_size - 2);
    }
    return views;
}

void PrintProbeStats(std::string_view scenario, const PacketProbeStats& stats) {
    const double split_percent =
        stats.packets == 0
            ? 0.0
            : (100.0 * static_cast<double>(stats.packets_split_across_views)) /
                  static_cast<double>(stats.packets);

    std::cout << "[packet-boundary] " << scenario
              << " packets=" << stats.packets
              << " recv_views=" << stats.recv_views
              << " bytes=" << stats.bytes
              << " split_packets=" << stats.packets_split_across_views
              << " split_percent=" << split_percent
              << " continuation_views=" << stats.continuation_views
              << " max_view_size=" << stats.max_view_size << "\n";
}

struct ParserStats {
    std::uint64_t packets = 0;
    std::uint64_t copied_bytes = 0;
    std::uint64_t checksum = 0;
};

class CopyingPacketParser {
public:
    const ParserStats& Stats() const {
        return stats_;
    }

    void OnRecv(std::span<const std::byte> data) {
        auto append = recv_buffer_.Append(data);
        ASSERT_TRUE(append.has_value());
        stats_.copied_bytes += data.size();

        while (recv_buffer_.ReadableSize() >= PacketBuilder::kHeaderSize) {
            const auto region = recv_buffer_.ReadRegion();
            const auto* current = region.data();
            const auto packet_size = ReadLe16(current);
            ASSERT_GE(packet_size, PacketBuilder::kHeaderSize);
            ASSERT_LE(packet_size, PacketBuilder::kMaxPacketSize);

            if (recv_buffer_.ReadableSize() < packet_size) {
                break;
            }

            const auto packet_id = ReadLe16(current + sizeof(std::uint16_t));
            OnPacket(packet_id, packet_size - PacketBuilder::kHeaderSize);
            recv_buffer_.OnRead(packet_size);
        }
    }

private:
    void OnPacket(std::uint16_t packet_id, std::uint32_t payload_size) {
        ++stats_.packets;
        stats_.checksum += packet_id;
        stats_.checksum += payload_size;
    }

    RecvBuffer recv_buffer_;
    ParserStats stats_;
};

class ViewFirstPacketParser {
public:
    const ParserStats& Stats() const {
        return stats_;
    }

    void OnRecv(std::span<const std::byte> data) {
        std::size_t offset = 0;

        while (offset < data.size()) {
            if (!scratch_.IsEmpty()) {
                offset += CompleteScratch(data.subspan(offset));
                if (!scratch_.IsEmpty()) {
                    return;
                }
                continue;
            }

            const auto remaining = data.size() - offset;
            if (remaining < PacketBuilder::kHeaderSize) {
                AppendScratch(data.subspan(offset));
                return;
            }

            const auto* current = data.data() + offset;
            const auto packet_size = ReadLe16(current);
            ASSERT_GE(packet_size, PacketBuilder::kHeaderSize);
            ASSERT_LE(packet_size, PacketBuilder::kMaxPacketSize);

            if (remaining < packet_size) {
                AppendScratch(data.subspan(offset));
                return;
            }

            const auto packet_id = ReadLe16(current + sizeof(std::uint16_t));
            OnPacket(packet_id, packet_size - PacketBuilder::kHeaderSize);
            offset += packet_size;
        }
    }

private:
    void AppendScratch(std::span<const std::byte> data) {
        auto append = scratch_.Append(data);
        ASSERT_TRUE(append.has_value());
        stats_.copied_bytes += data.size();
    }

    std::size_t CompleteScratch(std::span<const std::byte> data) {
        std::size_t consumed = 0;

        if (scratch_.ReadableSize() < PacketBuilder::kHeaderSize) {
            const auto need = PacketBuilder::kHeaderSize - scratch_.ReadableSize();
            const auto copy = std::min<std::size_t>(need, data.size());
            AppendScratch(data.first(copy));
            consumed += copy;
            if (scratch_.ReadableSize() < PacketBuilder::kHeaderSize) {
                return consumed;
            }
        }

        const auto region = scratch_.ReadRegion();
        const auto packet_size = ReadLe16(region.data());
        EXPECT_GE(packet_size, PacketBuilder::kHeaderSize);
        EXPECT_LE(packet_size, PacketBuilder::kMaxPacketSize);

        const auto need = packet_size - scratch_.ReadableSize();
        const auto copy = std::min<std::size_t>(need, data.size() - consumed);
        AppendScratch(data.subspan(consumed, copy));
        consumed += copy;

        if (scratch_.ReadableSize() < packet_size) {
            return consumed;
        }

        const auto full_packet = scratch_.ReadRegion();
        const auto packet_id =
            ReadLe16(full_packet.data() + sizeof(std::uint16_t));
        OnPacket(packet_id, packet_size - PacketBuilder::kHeaderSize);
        scratch_.OnRead(packet_size);
        return consumed;
    }

    void OnPacket(std::uint16_t packet_id, std::uint32_t payload_size) {
        ++stats_.packets;
        stats_.checksum += packet_id;
        stats_.checksum += payload_size;
    }

    RecvBuffer scratch_;
    ParserStats stats_;
};

struct BenchmarkResult {
    ParserStats stats;
    std::chrono::nanoseconds elapsed{};
};

template<typename Parser>
BenchmarkResult RunParserBenchmark(
    const std::vector<std::span<const std::byte>>& views,
    std::size_t iterations) {
    Parser parser;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        for (const auto view : views) {
            parser.OnRecv(view);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return {.stats = parser.Stats(),
            .elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                elapsed)};
}

void PrintBenchmarkResult(std::string_view scenario, std::string_view parser,
                          const BenchmarkResult& result,
                          std::uint64_t logical_bytes) {
    const auto seconds =
        static_cast<double>(result.elapsed.count()) / 1'000'000'000.0;
    const auto ns_per_packet =
        static_cast<double>(result.elapsed.count()) /
        static_cast<double>(result.stats.packets);
    const auto mib_per_second =
        (static_cast<double>(logical_bytes) / (1024.0 * 1024.0)) / seconds;

    std::cout << "[packet-parse-bench] " << scenario
              << " parser=" << parser
              << " packets=" << result.stats.packets
              << " copied_bytes=" << result.stats.copied_bytes
              << " ns_per_packet=" << ns_per_packet
              << " mib_per_second=" << mib_per_second
              << " checksum=" << result.stats.checksum << "\n";
}

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

TEST_F(PacketSessionTest, ParsesCompletePacketsDirectlyFromRecvView) {
    auto sockets = MakeSocketPair();
    TestPacketSession session(sockets.local, *ring, pool);

    const auto first_payload = BuildPayload(3);
    const auto second_payload = BuildPayload(5);
    const auto first = BuildPacket(101, first_payload);
    const auto second = BuildPacket(102, second_payload);
    const auto view = FlattenPackets({first, second});

    session.Feed(view);

    EXPECT_EQ(session.packet_count.load(std::memory_order_relaxed), 2);
    ASSERT_EQ(session.msg_ids.size(), 2u);
    EXPECT_EQ(session.msg_ids[0], 101u);
    EXPECT_EQ(session.msg_ids[1], 102u);
    EXPECT_EQ(session.payloads[0], first_payload);
    EXPECT_EQ(session.payloads[1], second_payload);
    EXPECT_TRUE(ContainsPointer(view, session.payload_pointers[0],
                                first_payload.size()));
    EXPECT_TRUE(ContainsPointer(view, session.payload_pointers[1],
                                second_payload.size()));

    ::close(sockets.remote);
}

TEST_F(PacketSessionTest, CompletesBufferedPacketThenResumesViewFastPath) {
    auto sockets = MakeSocketPair();
    TestPacketSession session(sockets.local, *ring, pool);

    const auto first_payload = BuildPayload(3);
    const auto second_payload = BuildPayload(5);
    const auto first = BuildPacket(201, first_payload);
    const auto second = BuildPacket(202, second_payload);

    session.Feed(std::span<const std::byte>(first.data(), 2));

    std::vector<std::byte> second_view;
    second_view.insert(second_view.end(), first.begin() + 2, first.end());
    second_view.insert(second_view.end(), second.begin(), second.end());
    session.Feed(second_view);

    EXPECT_EQ(session.packet_count.load(std::memory_order_relaxed), 2);
    ASSERT_EQ(session.msg_ids.size(), 2u);
    EXPECT_EQ(session.msg_ids[0], 201u);
    EXPECT_EQ(session.msg_ids[1], 202u);
    EXPECT_EQ(session.payloads[0], first_payload);
    EXPECT_EQ(session.payloads[1], second_payload);
    EXPECT_FALSE(ContainsPointer(second_view, session.payload_pointers[0],
                                 first_payload.size()));
    EXPECT_TRUE(ContainsPointer(second_view, session.payload_pointers[1],
                                second_payload.size()));

    ::close(sockets.remote);
}

TEST_F(PacketSessionTest, MeasuresLoopbackPacketBoundaryBehavior) {
    constexpr std::size_t kPacketCount = 4096;
    constexpr std::size_t kPayloadSize = 31;

    auto run_scenario =
        [&](std::string_view name,
            const std::function<void(int, const std::vector<std::vector<std::byte>>&)>&
                write_packets) {
            auto sockets = MakeTcpPair();
            auto session = std::make_shared<PacketBoundaryProbeSession>(
                sockets.server, *ring, pool);
            session->Start();

            const auto packets = BuildPacketSet(kPacketCount, kPayloadSize);
            write_packets(sockets.client, packets);

            DispatchUntil(*ring, [&] {
                return session->stats.packets == kPacketCount;
            });
            EXPECT_EQ(session->stats.packets, kPacketCount);
            PrintProbeStats(name, session->stats);

            ::close(sockets.client);
            DispatchUntil(*ring, [&] {
                return session->disconnected.load(std::memory_order_relaxed);
            });
        };

    run_scenario("single bulk write", [](int fd, const auto& packets) {
        const auto all = FlattenPackets(packets);
        WriteAll(fd, all);
    });

    run_scenario("one write per packet", [](int fd, const auto& packets) {
        for (const auto& packet : packets) {
            WriteAll(fd, packet);
        }
    });

    run_scenario("split header/body writes", [&](int fd, const auto& packets) {
        for (const auto& packet : packets) {
            WriteAll(fd, std::span<const std::byte>(packet.data(), 2));
            PumpOnce(*ring);
            WriteAll(fd, std::span<const std::byte>(packet.data() + 2,
                                                    packet.size() - 2));
        }
    });
}

TEST(PacketSessionParserBenchmark, MeasuresCopyingVsViewFirstParser) {
    constexpr std::size_t kPacketCount = 4096;
    constexpr std::size_t kPayloadSize = 31;
    constexpr std::size_t kIterations = 1024;
    constexpr std::size_t kPacketSize =
        PacketBuilder::kHeaderSize + kPayloadSize;

    const auto packets = BuildPacketSet(kPacketCount, kPayloadSize);
    const auto data = FlattenPackets(packets);

    struct Scenario {
        std::string_view name;
        std::vector<std::span<const std::byte>> views;
    };

    std::vector<Scenario> scenarios;
    scenarios.push_back({
        .name = "bulk 4096-byte views",
        .views = BuildBulkViews(data, 4096),
    });
    scenarios.push_back({
        .name = "one view per packet",
        .views = BuildPacketViews(data, kPacketSize),
    });
    scenarios.push_back({
        .name = "split header/body views",
        .views = BuildSplitHeaderBodyViews(data, kPacketSize),
    });

    const auto expected_packets = kPacketCount * kIterations;
    const auto logical_bytes =
        static_cast<std::uint64_t>(data.size()) * kIterations;

    for (const auto& scenario : scenarios) {
        (void)RunParserBenchmark<CopyingPacketParser>(scenario.views, 1);
        (void)RunParserBenchmark<ViewFirstPacketParser>(scenario.views, 1);

        const auto copying =
            RunParserBenchmark<CopyingPacketParser>(scenario.views,
                                                    kIterations);
        const auto view_first =
            RunParserBenchmark<ViewFirstPacketParser>(scenario.views,
                                                      kIterations);

        EXPECT_EQ(copying.stats.packets, expected_packets);
        EXPECT_EQ(view_first.stats.packets, expected_packets);
        EXPECT_EQ(copying.stats.checksum, view_first.stats.checksum);

        PrintBenchmarkResult(scenario.name, "copying", copying, logical_bytes);
        PrintBenchmarkResult(scenario.name, "view-first", view_first,
                             logical_bytes);

        const auto speedup =
            static_cast<double>(copying.elapsed.count()) /
            static_cast<double>(view_first.elapsed.count());
        std::cout << "[packet-parse-bench] " << scenario.name
                  << " view_first_speedup=" << speedup << "\n";
    }
}
