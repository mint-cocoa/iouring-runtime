#include "ActivityHub.h"
#include "ActivitySession.h"
#include "ActivityUtils.h"

#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/Worker.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <thread>

namespace core = iouring_runtime::core;
namespace io = iouring_runtime::core::io;
namespace ring = iouring_runtime::core::ring;

namespace {

std::atomic<bool> g_stop_requested{false};

void HandleSignal(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

} // namespace

int main() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    const auto host = activity_server::EnvString("ACTIVITY_HOST", "0.0.0.0");
    const auto port = activity_server::EnvPort("ACTIVITY_PORT", 8000);

    activity_server::ActivityHub hub;

    io::SessionFactory factory =
        [&hub](int fd, ring::IoRing& loop, core::buffer::BufferPool& buffer_pool,
               core::ContextId) -> io::SessionRef {
        return std::make_shared<activity_server::ActivitySession>(
            fd, loop, buffer_pool, hub);
    };

    io::WorkerConfig config;
    config.address = core::Address{host, port};
    config.ring.queue_depth = 2048;
    config.ring.buf_ring.buf_count = 4096;
    config.ring.buf_ring.buf_size = 8192;
    config.ring.submit_batch_size = 1;
    config.io_timeout = std::chrono::milliseconds{10};

    io::Worker worker(config, std::move(factory));
    if (!worker.Start()) {
        std::fprintf(stderr, "failed to listen on %s:%u\n", host.c_str(), port);
        return 1;
    }

    std::printf("activity_server listening on %s:%u\n", host.c_str(), port);
    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    worker.Stop();
    return 0;
}
