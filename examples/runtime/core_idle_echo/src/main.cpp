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
#include <string_view>

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
        [idle_timeout](int fd,
                       iouring_runtime::core::ring::IoRing& ring_ref,
                       iouring_runtime::core::buffer::BufferPool& pool_ref,
                       iouring_runtime::core::ContextId) -> iouring_runtime::core::io::SessionRef {
        return std::make_shared<IdleEchoSession>(fd, ring_ref, pool_ref, idle_timeout);
    };

    auto listener = std::make_shared<iouring_runtime::core::io::Listener>(
        *ring, pool, addr, std::move(factory), 0);
    auto start_result = listener->Start();
    if (!start_result) {
        std::cerr << "failed to listen on port " << addr.port << "\n";
        iouring_runtime::core::ring::IoRing::SetCurrent(nullptr);
        return 1;
    }

    std::cout << "core_idle_echo listening on " << addr.host << ":" << addr.port
              << " idle_timeout=" << idle_timeout.count() << "ms\n";

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        ring->Dispatch(std::chrono::milliseconds{10});
        ring->ProcessPostedTasks();
    }

    listener->Stop();
    iouring_runtime::core::ring::IoRing::SetCurrent(nullptr);
    return 0;
}
