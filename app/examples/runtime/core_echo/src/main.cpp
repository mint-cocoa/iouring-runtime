#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/Types.h>
#include <iouring_runtime/core/WorkerPool.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

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

std::uint16_t ReadWorkers() {
    if (const char* raw = std::getenv("CORE_ECHO_WORKERS")) {
        return static_cast<std::uint16_t>(std::max(1, std::stoi(raw)));
    }
    return 1;
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

    iouring_runtime::core::io::WorkerSessionFactoryBuilder factory_builder =
        [](iouring_runtime::core::ContextId) {
        return [](int fd,
                  iouring_runtime::core::ring::IoRing& ring_ref,
                  iouring_runtime::core::buffer::BufferPool& pool_ref,
                  iouring_runtime::core::ContextId)
                   -> iouring_runtime::core::io::SessionRef {
        return std::make_shared<EchoSession>(fd, ring_ref, pool_ref);
    };
    };

    iouring_runtime::core::io::WorkerPoolConfig config;
    config.address = iouring_runtime::core::Address{
        .host = "0.0.0.0",
        .port = ReadPort(),
    };
    config.worker_count = ReadWorkers();
    config.ring.queue_depth = 256;
    config.ring.buf_ring.buf_count = 512;
    config.ring.buf_ring.buf_size = 4096;
    config.io_timeout = std::chrono::milliseconds{10};

    iouring_runtime::core::io::WorkerPool workers(config, std::move(factory_builder));
    if (!workers.Start()) {
        std::cerr << "failed to listen on port " << config.address.port << "\n";
        return 1;
    }

    std::cout << "core_echo listening on "
              << config.address.host << ":" << config.address.port
              << " workers=" << workers.Count() << "\n";

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    workers.Stop();
    return 0;
}
