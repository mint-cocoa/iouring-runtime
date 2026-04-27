#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/Listener.h>
#include <iouring_runtime/game/PlayerRegistry.h>
#include <iouring_runtime/game/RoomManager.h>

#include "DungeonGameSession.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>

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

    iouring_runtime::core::ring::IoRingConfig ring_config;
    ring_config.queue_depth = 256;
    ring_config.buf_ring.buf_count = 512;
    ring_config.buf_ring.buf_size = 4096;

    auto ring_result = iouring_runtime::core::ring::IoRing::Create(ring_config);
    if (!ring_result) {
        std::cerr << "failed to create IoRing\n";
        return 1;
    }
    auto ring = std::move(*ring_result);
    iouring_runtime::core::ring::IoRing::SetCurrent(ring.get());

    iouring_runtime::core::buffer::BufferPool pool;
    iouring_runtime::core::job::GlobalQueue global_queue;
    auto player_registry =
        std::make_shared<iouring_runtime::game::PlayerRegistry>();
    auto room_manager =
        std::make_shared<iouring_runtime::game::RoomManager>(global_queue);
    iouring_runtime::core::Address addr{
        .host = "0.0.0.0",
        .port = ReadPort(),
    };

    iouring_runtime::core::io::SessionFactory factory =
        [player_registry, room_manager](
            int fd,
            iouring_runtime::core::ring::IoRing& ring_ref,
            iouring_runtime::core::buffer::BufferPool& pool_ref,
            iouring_runtime::core::ContextId)
            -> iouring_runtime::core::io::SessionRef {
        auto session = std::make_shared<DungeonGameSession>(
            fd, ring_ref, pool_ref, player_registry, room_manager);
        session->SetSessionId(
            g_next_session_id.fetch_add(1, std::memory_order_relaxed));
        return session;
    };

    auto listener = std::make_shared<iouring_runtime::core::io::Listener>(
        *ring, pool, addr, std::move(factory), 0);
    auto start_result = listener->Start();
    if (!start_result) {
        std::cerr << "failed to listen on port " << addr.port << "\n";
        iouring_runtime::core::ring::IoRing::SetCurrent(nullptr);
        return 1;
    }

    std::cout << "dungeon_packet_echo listening on "
              << addr.host << ":" << addr.port << "\n";

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        ring->Dispatch(std::chrono::milliseconds{10});
        ring->ProcessPostedTasks();
        while (auto* queue = global_queue.TryPop()) {
            queue->Execute();
        }
    }

    listener->Stop();
    iouring_runtime::core::ring::IoRing::SetCurrent(nullptr);
    return 0;
}
