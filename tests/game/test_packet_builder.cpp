#include <iouring_runtime/game/PacketBuilder.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using iouring_runtime::core::buffer::BufferPool;
using iouring_runtime::game::PacketId;
using iouring_runtime::game::PacketBuilder;

namespace {

class FakeProto {
public:
    explicit FakeProto(std::vector<std::byte> payload)
        : payload_(std::move(payload)) {}

    bool ParseFromArray(const void*, int) {
        return true;
    }

    bool SerializeToArray(void* out, int size) const {
        if (size != static_cast<int>(payload_.size())) {
            return false;
        }
        std::memcpy(out, payload_.data(), payload_.size());
        return true;
    }

    std::size_t ByteSizeLong() const {
        return payload_.size();
    }

private:
    std::vector<std::byte> payload_;
};

std::uint16_t ReadLe16(const std::byte* data) {
    std::uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

} // namespace

TEST(PacketBuilder, BuildsDungeonPacketHeaderAndPayload) {
    BufferPool pool;
    FakeProto proto({
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33},
    });

    auto packet = PacketBuilder::Build(pool, 101, proto);

    ASSERT_NE(packet, nullptr);
    const auto data = packet->Data();
    ASSERT_EQ(data.size(), 7u);
    EXPECT_EQ(ReadLe16(data.data()), 7u);
    EXPECT_EQ(ReadLe16(data.data() + 2), 101u);
    EXPECT_EQ(data[4], std::byte{0x11});
    EXPECT_EQ(data[5], std::byte{0x22});
    EXPECT_EQ(data[6], std::byte{0x33});
}

TEST(PacketBuilder, AllowsPacketAtMaximumSize) {
    BufferPool pool(PacketBuilder::kMaxPacketSize, 1);
    FakeProto proto(std::vector<std::byte>(
        PacketBuilder::kMaxPacketSize - PacketBuilder::kHeaderSize,
        std::byte{0x7f}));

    auto packet = PacketBuilder::Build(pool, 102, proto);

    ASSERT_NE(packet, nullptr);
    EXPECT_EQ(packet->Data().size(), PacketBuilder::kMaxPacketSize);
    EXPECT_EQ(ReadLe16(packet->Data().data()), PacketBuilder::kMaxPacketSize);
    EXPECT_EQ(ReadLe16(packet->Data().data() + 2), 102u);
}

TEST(PacketBuilder, RejectsOversizedPacket) {
    BufferPool pool;
    FakeProto proto(std::vector<std::byte>(
        PacketBuilder::kMaxPacketSize - PacketBuilder::kHeaderSize + 1,
        std::byte{0x01}));

    auto packet = PacketBuilder::Build(pool, 101, proto);

    EXPECT_EQ(packet, nullptr);
}
