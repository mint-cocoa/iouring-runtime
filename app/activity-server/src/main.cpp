#include "ActivityHub.h"
#include "ActivitySession.h"
#include "ActivityUtils.h"

#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/Listener.h>
#include <iouring_runtime/core/Session.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>

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

    ring::IoRingConfig config;
    config.queue_depth = 2048;
    config.buf_ring.buf_count = 4096;
    config.buf_ring.buf_size = 8192;
    config.submit_batch_size = 1;

    auto ring_result = ring::IoRing::Create(config);
    if (!ring_result) {
        std::fprintf(stderr, "failed to create io_uring\n");
        return 1;
    }
    auto io_ring = std::move(*ring_result);
    ring::IoRing::SetCurrent(io_ring.get());
    core::buffer::BufferPool pool;
    activity_server::ActivityHub hub;

    io::SessionFactory factory =
        [&hub](int fd, ring::IoRing& loop, core::buffer::BufferPool& buffer_pool,
               core::ContextId) -> io::SessionRef {
        return std::make_shared<activity_server::ActivitySession>(
            fd, loop, buffer_pool, hub);
    };

    auto listener = std::make_shared<io::Listener>(
        *io_ring, pool, core::Address{host, port}, std::move(factory), 0, 0);
    auto listen_result = listener->Start();
    if (!listen_result) {
        std::fprintf(stderr, "failed to listen on %s:%u\n", host.c_str(), port);
        return 1;
    }

    std::printf("activity_server listening on %s:%u\n", host.c_str(), port);
    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        io_ring->Dispatch(std::chrono::milliseconds{10});
        io_ring->ProcessPostedTasks();
    }
    listener->Stop();
    for (int i = 0; i < 8; ++i) {
        io_ring->Dispatch(std::chrono::milliseconds{0});
        io_ring->ProcessPostedTasks();
    }
    return 0;
}
