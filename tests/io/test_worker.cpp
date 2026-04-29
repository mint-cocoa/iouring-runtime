#include <iouring_runtime/core/Worker.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

namespace {

iouring_runtime::core::io::SessionFactory NullSessionFactory() {
    return [](int,
              iouring_runtime::core::ring::IoRing&,
              iouring_runtime::core::buffer::BufferPool&,
              iouring_runtime::core::ContextId)
               -> iouring_runtime::core::io::SessionRef {
        return nullptr;
    };
}

} // namespace

TEST(Worker, PostFromNonRingThreadWakesDispatch) {
    iouring_runtime::core::io::WorkerConfig config;
    config.address = iouring_runtime::core::Address{"127.0.0.1", 0};
    config.io_timeout = 5s;
    config.ring.queue_depth = 64;
    config.ring.buf_ring.buf_count = 64;
    config.ring.buf_ring.buf_size = 1024;

    iouring_runtime::core::io::Worker worker(config, NullSessionFactory());
    ASSERT_TRUE(worker.Start());

    std::promise<void> ran;
    auto done = ran.get_future();

    std::this_thread::sleep_for(50ms);
    worker.Post([&ran] {
        ran.set_value();
    });

    EXPECT_EQ(done.wait_for(500ms), std::future_status::ready);
    worker.Stop();
}
