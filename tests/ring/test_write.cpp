#include <iouring/event/IoRing.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unistd.h>

using namespace iouring::event;

namespace {

class WriteObserver : public std::enable_shared_from_this<WriteObserver> {
public:
    bool fired = false;
    std::int32_t last_result = 0;

    DispatchResult OnWrite(WriteEvent&, std::int32_t result) {
        fired = true;
        last_result = result;
        return DispatchResult::kComplete;
    }
};

} // namespace

TEST(IoRingWrite, WritesToPipe) {
    auto ring_result = IoRing::Create();
    ASSERT_TRUE(ring_result.has_value());
    auto& ring = *ring_result.value();
    IoRing::SetCurrent(&ring);

    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);

    constexpr std::string_view payload = "hello through io_uring write";
    auto observer = std::make_shared<WriteObserver>();
    auto* ev = new WriteEvent();
    BindCompletion(*ev, observer, &WriteObserver::OnWrite);
    ev->SetAutoDelete(true);

    ASSERT_TRUE(ring.PrepWrite(*ev, fds[1], payload.data(),
                               static_cast<unsigned>(payload.size()), 0));
    ASSERT_GE(ring.Submit(), 0);
    ring.Dispatch(std::chrono::milliseconds{500});

    ASSERT_TRUE(observer->fired);
    ASSERT_EQ(observer->last_result, static_cast<std::int32_t>(payload.size()));

    std::array<char, 64> read_buf{};
    const auto read_bytes = read(fds[0], read_buf.data(), read_buf.size());
    ASSERT_EQ(read_bytes, static_cast<ssize_t>(payload.size()));
    EXPECT_EQ(std::string_view(read_buf.data(), static_cast<std::size_t>(read_bytes)),
              payload);

    close(fds[0]);
    close(fds[1]);
    IoRing::SetCurrent(nullptr);
}
