#include <iouring_runtime/core/Worker.h>
#include <iouring_runtime/core/SessionControl.h>
#include <iouring_runtime/game/PlayerRegistry.h>
#include <iouring_runtime/game/RoomManager.h>

#include "DungeonGameSession.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

namespace {

std::atomic<bool> g_stop_requested{false};
std::atomic<iouring_runtime::core::SessionId> g_next_session_id{1};

void HandleSignal(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

std::uint16_t ReadPort() {
    if (const char* raw = std::getenv("DUNGEON_PACKET_ECHO_PORT")) {
        return static_cast<std::uint16_t>(std::stoi(raw));
    }
    return 19110;
}

} // namespace

int main() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    iouring_runtime::core::job::GlobalQueue global_queue;
    auto player_registry =
        std::make_shared<iouring_runtime::game::PlayerRegistry>();
    auto room_manager =
        std::make_shared<iouring_runtime::game::RoomManager>(global_queue);

    iouring_runtime::core::io::SessionFactory factory =
        [player_registry, room_manager](
            int fd,
            iouring_runtime::core::ring::IoRing& ring_ref,
            iouring_runtime::core::buffer::BufferPool& pool_ref,
            iouring_runtime::core::ContextId)
            -> iouring_runtime::core::io::SessionRef {
        auto session = std::make_shared<DungeonGameSession>(
            fd, ring_ref, pool_ref, player_registry, room_manager);
        iouring_runtime::core::io::SessionControl::SetSessionId(
            *session,
            g_next_session_id.fetch_add(1, std::memory_order_relaxed));
        return session;
    };

    iouring_runtime::core::io::WorkerConfig config;
    config.address = iouring_runtime::core::Address{
        .host = "0.0.0.0",
        .port = ReadPort(),
    };
    config.ring.queue_depth = 256;
    config.ring.buf_ring.buf_count = 512;
    config.ring.buf_ring.buf_size = 4096;
    config.io_timeout = std::chrono::milliseconds{10};

    iouring_runtime::core::io::WorkerHooks hooks;
    hooks.tick = [&global_queue](iouring_runtime::core::io::Worker&) {
        while (auto* queue = global_queue.TryPop()) {
            queue->Execute();
        }
    };

    iouring_runtime::core::io::Worker worker(
        config, std::move(factory), std::move(hooks));
    if (!worker.Start()) {
        std::cerr << "failed to listen on port " << config.address.port << "\n";
        return 1;
    }

    std::cout << "dungeon_packet_echo listening on "
              << config.address.host << ":" << config.address.port << "\n";

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    worker.Stop();
    return 0;
}
