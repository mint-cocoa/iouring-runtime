#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/Types.h>
#include <iouring_runtime/core/Worker.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_stop_requested{false};

void HandleSignal(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

std::uint16_t ReadPort() {
    if (const char* raw = std::getenv("CORE_IDLE_ECHO_PORT")) {
        return static_cast<std::uint16_t>(std::stoi(raw));
    }
    return 19091;
}

std::chrono::milliseconds ReadIdleTimeout() {
    if (const char* raw = std::getenv("CORE_IDLE_TIMEOUT_MS")) {
        return std::chrono::milliseconds{std::stoi(raw)};
    }
    return std::chrono::milliseconds{5000};
}

class IdleEchoSession final : public iouring_runtime::core::io::Session {
public:
    IdleEchoSession(int fd,
                    iouring_runtime::core::ring::IoRing& ring,
                    iouring_runtime::core::buffer::BufferPool& pool,
                    std::chrono::milliseconds idle_timeout)
        : Session(fd, ring, pool)
        , idle_timeout_(idle_timeout) {
        SetInactivityTimeout(idle_timeout_);
    }

protected:
    void OnConnected() override {
        std::cout << "client connected: " << RemoteAddr()
                  << " idle_timeout=" << idle_timeout_.count() << "ms\n";

        if (!SendText(
                "core_idle_echo ready\n"
                "send bytes to echo them back\n"
                "send 'quit' to flush 'bye' and close\n")) {
            Disconnect();
        }
    }

    void OnRecv(std::span<const std::byte> data) override {
        const auto* raw = reinterpret_cast<const char*>(data.data());
        std::string_view text{raw, data.size()};

        if (text == "quit" || text == "quit\n" || text == "quit\r\n") {
            if (!SendText("bye\n")) {
                Disconnect();
                return;
            }
            DisconnectAfterFlush();
            return;
        }

        if (!SendBytes(data)) {
            Disconnect();
        }
    }

    void OnDisconnected() override {
        std::cout << "client disconnected\n";
    }

private:
    bool SendText(std::string_view text) {
        auto result = Pool().Allocate(static_cast<std::uint32_t>(text.size()));
        if (!result) {
            return false;
        }

        auto buf = std::move(*result);
        std::memcpy(buf->Writable().data(), text.data(), text.size());
        buf->Commit(static_cast<std::uint32_t>(text.size()));
        return Send(std::move(buf)).has_value();
    }

    bool SendBytes(std::span<const std::byte> data) {
        auto result = Pool().Allocate(static_cast<std::uint32_t>(data.size()));
        if (!result) {
            return false;
        }

        auto buf = std::move(*result);
        std::memcpy(buf->Writable().data(), data.data(), data.size());
        buf->Commit(static_cast<std::uint32_t>(data.size()));
        return Send(std::move(buf)).has_value();
    }

    std::chrono::milliseconds idle_timeout_;
};

} // namespace

int main() {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    const auto idle_timeout = ReadIdleTimeout();

    iouring_runtime::core::io::SessionFactory factory =
        [idle_timeout](int fd,
                       iouring_runtime::core::ring::IoRing& ring_ref,
                       iouring_runtime::core::buffer::BufferPool& pool_ref,
                       iouring_runtime::core::ContextId) -> iouring_runtime::core::io::SessionRef {
        return std::make_shared<IdleEchoSession>(fd, ring_ref, pool_ref, idle_timeout);
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

    iouring_runtime::core::io::Worker worker(config, std::move(factory));
    if (!worker.Start()) {
        std::cerr << "failed to listen on port " << config.address.port << "\n";
        return 1;
    }

    std::cout << "core_idle_echo listening on "
              << config.address.host << ":" << config.address.port
              << " idle_timeout=" << idle_timeout.count() << "ms\n";

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    worker.Stop();
    return 0;
}
