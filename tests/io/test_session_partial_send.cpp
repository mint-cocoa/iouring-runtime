#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/SessionDetail.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/SendQueue.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <sys/uio.h>
#include <vector>

using namespace iouring_runtime::core;
using namespace iouring_runtime::core::io;
using namespace iouring_runtime::core::buffer;

namespace {

// Builds a SendBufferRef of the given size with each byte set to `fill`, so
// the partial-advance test can inspect the post-advance iov_base and confirm
// it points at the right offset into the underlying storage.
SendBufferRef MakeBuffer(BufferPool& pool, std::uint32_t size, std::byte fill) {
    auto result = pool.Allocate(size);
    EXPECT_TRUE(result.has_value());
    auto buf = std::move(*result);
    std::memset(buf->Writable().data(), static_cast<int>(fill), size);
    buf->Commit(size);
    return buf;
}

// Snapshot the iovec-list as (base, length) so assertions are readable even
// after iov_base values have been advanced past the buffer origin.
struct IovView {
    const std::byte* base;
    std::size_t len;
};
std::vector<IovView> View(const std::vector<struct iovec>& iovs) {
    std::vector<IovView> out;
    out.reserve(iovs.size());
    for (const auto& iov : iovs) {
        out.push_back({static_cast<const std::byte*>(iov.iov_base), iov.iov_len});
    }
    return out;
}

struct PartialSendFixture : public ::testing::Test {
    BufferPool pool;
    std::vector<struct iovec> iovs;
    std::vector<SendBufferRef> bufs;

    void PushBuffer(std::uint32_t size, std::byte fill) {
        auto ref = MakeBuffer(pool, size, fill);
        iovs.push_back({
            .iov_base = const_cast<std::byte*>(ref->Data().data()),
            .iov_len = ref->Data().size(),
        });
        bufs.push_back(std::move(ref));
    }
};

} // namespace

TEST_F(PartialSendFixture, AdvanceZeroLeavesStateUnchanged) {
    PushBuffer(10, std::byte{0xAA});
    PushBuffer(20, std::byte{0xBB});
    auto before = View(iovs);

    detail::AdvanceSendState(iovs, bufs, 0);

    EXPECT_EQ(iovs.size(), 2u);
    EXPECT_EQ(bufs.size(), 2u);
    auto after = View(iovs);
    EXPECT_EQ(before[0].base, after[0].base);
    EXPECT_EQ(before[0].len, after[0].len);
    EXPECT_EQ(before[1].base, after[1].base);
    EXPECT_EQ(before[1].len, after[1].len);
}

TEST_F(PartialSendFixture, AdvanceWithinFirstIovec) {
    PushBuffer(10, std::byte{0xAA});
    PushBuffer(20, std::byte{0xBB});
    const std::byte* original_first = static_cast<const std::byte*>(iovs[0].iov_base);
    const std::byte* original_second = static_cast<const std::byte*>(iovs[1].iov_base);

    detail::AdvanceSendState(iovs, bufs, 3);

    ASSERT_EQ(iovs.size(), 2u);
    ASSERT_EQ(bufs.size(), 2u);
    EXPECT_EQ(iovs[0].iov_base, original_first + 3);
    EXPECT_EQ(iovs[0].iov_len, 7u);
    EXPECT_EQ(iovs[1].iov_base, original_second);
    EXPECT_EQ(iovs[1].iov_len, 20u);
}

TEST_F(PartialSendFixture, AdvanceExactlyFirstIovecDropsIt) {
    PushBuffer(10, std::byte{0xAA});
    PushBuffer(20, std::byte{0xBB});
    const std::byte* second_origin = static_cast<const std::byte*>(iovs[1].iov_base);
    std::weak_ptr<SendBuffer> first_weak = bufs[0];

    detail::AdvanceSendState(iovs, bufs, 10);

    ASSERT_EQ(iovs.size(), 1u);
    ASSERT_EQ(bufs.size(), 1u);
    EXPECT_EQ(iovs[0].iov_base, second_origin);
    EXPECT_EQ(iovs[0].iov_len, 20u);
    EXPECT_TRUE(first_weak.expired()) << "consumed buffer ref must be released";
}

TEST_F(PartialSendFixture, AdvanceAcrossMultipleIovecs) {
    PushBuffer(10, std::byte{0xAA});
    PushBuffer(20, std::byte{0xBB});
    PushBuffer(30, std::byte{0xCC});
    const std::byte* third_origin = static_cast<const std::byte*>(iovs[2].iov_base);
    std::weak_ptr<SendBuffer> first_weak = bufs[0];
    std::weak_ptr<SendBuffer> second_weak = bufs[1];

    // Consume all of iov[0] (10), all of iov[1] (20), and 5 bytes of iov[2].
    detail::AdvanceSendState(iovs, bufs, 35);

    ASSERT_EQ(iovs.size(), 1u);
    ASSERT_EQ(bufs.size(), 1u);
    EXPECT_EQ(iovs[0].iov_base, third_origin + 5);
    EXPECT_EQ(iovs[0].iov_len, 25u);
    EXPECT_TRUE(first_weak.expired());
    EXPECT_TRUE(second_weak.expired());
}

TEST_F(PartialSendFixture, AdvanceBySumEmptiesBothVectors) {
    PushBuffer(10, std::byte{0xAA});
    PushBuffer(20, std::byte{0xBB});
    std::weak_ptr<SendBuffer> first_weak = bufs[0];
    std::weak_ptr<SendBuffer> second_weak = bufs[1];

    detail::AdvanceSendState(iovs, bufs, 30);

    EXPECT_TRUE(iovs.empty());
    EXPECT_TRUE(bufs.empty());
    EXPECT_TRUE(first_weak.expired());
    EXPECT_TRUE(second_weak.expired());
}

TEST(SessionDisconnectClassificationTest, TreatsPeerCloseAsExpectedDisconnect) {
    EXPECT_TRUE(detail::IsExpectedDisconnectResult(0));
    EXPECT_EQ(detail::DisconnectReasonForResult(0), std::string_view("PEER_CLOSE"));
}

TEST(SessionDisconnectClassificationTest, TreatsCommonClientShutdownErrorsAsExpected) {
    EXPECT_TRUE(detail::IsExpectedDisconnectResult(-ECONNRESET));
    EXPECT_EQ(detail::DisconnectReasonForResult(-ECONNRESET),
              std::string_view("CONNECTION_RESET"));

    EXPECT_TRUE(detail::IsExpectedDisconnectResult(-EPIPE));
    EXPECT_EQ(detail::DisconnectReasonForResult(-EPIPE), std::string_view("BROKEN_PIPE"));

    EXPECT_TRUE(detail::IsExpectedDisconnectResult(-ENOTCONN));
    EXPECT_EQ(detail::DisconnectReasonForResult(-ENOTCONN),
              std::string_view("NOT_CONNECTED"));

    EXPECT_TRUE(detail::IsExpectedDisconnectResult(-ESHUTDOWN));
    EXPECT_EQ(detail::DisconnectReasonForResult(-ESHUTDOWN),
              std::string_view("SOCKET_SHUTDOWN"));
}

TEST(SessionDisconnectClassificationTest, KeepsUnexpectedTransportErrorsVisible) {
    EXPECT_FALSE(detail::IsExpectedDisconnectResult(-EINVAL));
    EXPECT_EQ(detail::DisconnectReasonForResult(-EINVAL),
              std::string_view("TRANSPORT_ERROR"));

    EXPECT_FALSE(detail::IsExpectedDisconnectResult(12));
    EXPECT_EQ(detail::DisconnectReasonForResult(12), std::string_view("OK"));
}

TEST(SendBufferPoolTest, ReusesReleasedPartiallyFilledChunk) {
    BufferPool pool(1024, 1);

    auto first = MakeBuffer(pool, 100, std::byte{0xAA});
    first.reset();

    auto second = pool.Allocate(100);
    ASSERT_TRUE(second.has_value());
}

TEST(SendQueueTest, TracksPendingBytesAcrossPartialDrains) {
    BufferPool pool(1024, 1);
    SendQueue queue;

    auto first = MakeBuffer(pool, 10, std::byte{0xAA});
    auto second = MakeBuffer(pool, 20, std::byte{0xBB});
    auto third = MakeBuffer(pool, 30, std::byte{0xCC});

    auto pushed = queue.Push(std::move(first));
    EXPECT_EQ(pushed.current_depth, 1u);
    EXPECT_EQ(pushed.current_bytes, 10u);
    pushed = queue.Push(std::move(second));
    EXPECT_EQ(pushed.current_depth, 2u);
    EXPECT_EQ(pushed.current_bytes, 30u);
    pushed = queue.Push(std::move(third));
    EXPECT_EQ(pushed.current_depth, 3u);
    EXPECT_EQ(pushed.current_bytes, 60u);

    auto stats = queue.Snapshot();
    EXPECT_EQ(stats.current_depth, 3u);
    EXPECT_EQ(stats.pending_bytes, 60u);
    EXPECT_EQ(stats.peak_pending_bytes, 60u);

    std::vector<SendBufferRef> drained;
    EXPECT_EQ(queue.DrainInto(drained, 2), 2u);
    stats = queue.Snapshot();
    EXPECT_EQ(stats.current_depth, 1u);
    EXPECT_EQ(stats.pending_bytes, 30u);
    EXPECT_EQ(stats.peak_pending_bytes, 60u);

    EXPECT_EQ(queue.DrainInto(drained), 1u);
    stats = queue.Snapshot();
    EXPECT_EQ(stats.current_depth, 0u);
    EXPECT_EQ(stats.pending_bytes, 0u);
}
