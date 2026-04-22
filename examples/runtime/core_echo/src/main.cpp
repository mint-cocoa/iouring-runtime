#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/core/Listener.h>
#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/Types.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

namespace {

std::atomic<bool> g_stop_requested{false};

void HandleSignal(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

std::uint16_t ReadPort() {
    if (const char* raw = std::getenv("CORE_ECHO_PORT")) {
        return static_cast<std::uint16_t>(std::stoi(raw));
    }
    return 19090;
}

class EchoSession final : public iouring_runtime::core::io::Session {
public:
    using Session::Session;

protected:
    void OnRecv(std::span<const std::byte> data) override {
        auto result = Pool().Allocate(static_cast<std::uint32_t>(data.size()));
        if (!result) {
            Disconnect();
            return;
        }

        auto buf = std::move(*result);
        std::memcpy(buf->Writable().data(), data.data(), data.size());
        buf->Commit(static_cast<std::uint32_t>(data.size()));

        if (!Send(std::move(buf)).has_value()) {
            Disconnect();
        }
    }

    void OnConnected() override {
        std::cout << "client connected: " << RemoteAddr() << "\n";
    }

    void OnDisconnected() override {
        std::cout << "client disconnected\n";
    }
};

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
    iouring_runtime::core::Address addr{
        .host = "0.0.0.0",
        .port = ReadPort(),
    };

    iouring_runtime::core::io::SessionFactory factory =
        [](int fd,
           iouring_runtime::core::ring::IoRing& ring_ref,
           iouring_runtime::core::buffer::BufferPool& pool_ref,
           iouring_runtime::core::ContextId) -> iouring_runtime::core::io::SessionRef {
        return std::make_shared<EchoSession>(fd, ring_ref, pool_ref);
    };

    auto listener = std::make_shared<iouring_runtime::core::io::Listener>(
        *ring, pool, addr, std::move(factory), 0);
    auto start_result = listener->Start();
    if (!start_result) {
        std::cerr << "failed to listen on port " << addr.port << "\n";
        iouring_runtime::core::ring::IoRing::SetCurrent(nullptr);
        return 1;
    }

    std::cout << "core_echo listening on " << addr.host << ":" << addr.port << "\n";

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        ring->Dispatch(std::chrono::milliseconds{10});
        ring->ProcessPostedTasks();
    }

    listener->Stop();
    iouring_runtime::core::ring::IoRing::SetCurrent(nullptr);
    return 0;
}
