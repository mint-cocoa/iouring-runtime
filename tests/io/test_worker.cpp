#include <iouring/event/Worker.h>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

namespace {

iouring::net::SessionFactory NullSessionFactory() {
    return [](int,
              iouring::event::IoRing&,
              iouring::core::buffer::BufferPool&,
              iouring::core::ContextId)
               -> iouring::net::SessionRef {
        return nullptr;
    };
}

} // namespace

TEST(Worker, PostFromNonRingThreadWakesDispatch) {
    iouring::event::WorkerConfig config;
    config.address = iouring::core::Address{"127.0.0.1", 0};
    config.io_timeout = 5s;
    config.ring.queue_depth = 64;
    config.ring.buf_ring.buf_count = 64;
    config.ring.buf_ring.buf_size = 1024;

    iouring::event::Worker worker(config, NullSessionFactory());
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
