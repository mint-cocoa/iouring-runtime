#include <iouring_runtime/web/WebServer.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <string>
#include <string_view>
#include <thread>

using namespace iouring_runtime::web;
using namespace std::chrono_literals;

namespace {

constexpr std::uint16_t kTimeoutPort = 19877;
constexpr std::uint16_t kLimitPort = 19878;
constexpr std::uint16_t kShutdownPort = 19879;
constexpr std::uint16_t kPressurePort = 19880;

int ConnectTcp(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string RecvWithTimeout(int fd, int timeout_ms) {
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char buf[1024];
    const auto n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        return {};
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

bool WaitForPeerClose(int fd, int timeout_ms) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    char byte = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto n = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0) {
            ::fcntl(fd, F_SETFL, flags);
            return true;
        }
        if (n < 0) {
            if (errno == ECONNRESET || errno == ENOTCONN) {
                ::fcntl(fd, F_SETFL, flags);
                return true;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ::fcntl(fd, F_SETFL, flags);
                return false;
            }
        }
        std::this_thread::sleep_for(10ms);
    }

    ::fcntl(fd, F_SETFL, flags);
    return false;
}

bool SendAll(int fd, std::string_view data) {
    const char* current = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const auto n = ::send(fd, current, remaining, 0);
        if (n <= 0) {
            return false;
        }
        current += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

TEST(HttpServerTimeouts, PartialRequestPastDeadlineReturns408) {
    WebServerConfig config;
    config.port = kTimeoutPort;
    config.worker_count = 1;
    config.ring.io_timeout = 1ms;
    config.timeouts.inactivity = 2s;
    config.timeouts.request = 200ms;

    WebServer server(config);
    server.Get("/", [](RequestContext& ctx) {
        ctx.response.Body("ok").Send();
    });
    server.Start();
    std::this_thread::sleep_for(200ms);

    const int fd = ConnectTcp(kTimeoutPort);
    ASSERT_GE(fd, 0);

    const std::string partial = "GET / HTTP/1.1\r\nHost: localhost\r\n";
    ASSERT_EQ(::send(fd, partial.data(), partial.size(), 0),
              static_cast<ssize_t>(partial.size()));

    const auto response = RecvWithTimeout(fd, 1500);
    ::close(fd);
    server.Stop();

    EXPECT_NE(response.find("408 Request Timeout"), std::string::npos);
    EXPECT_NE(response.find("Connection: close"), std::string::npos);
}

TEST(HttpServerBackpressure, RejectsNewSessionsPastWorkerLimit) {
    WebServerConfig config;
    config.port = kLimitPort;
    config.worker_count = 1;
    config.max_sessions_per_worker = 1;
    config.ring.io_timeout = 1ms;
    config.timeouts.inactivity = 5s;

    WebServer server(config);
    server.Get("/", [](RequestContext& ctx) {
        ctx.response.Body("ok").Send();
    });
    server.Start();
    std::this_thread::sleep_for(200ms);

    const int first_fd = ConnectTcp(kLimitPort);
    ASSERT_GE(first_fd, 0);

    const int second_fd = ConnectTcp(kLimitPort);
    ASSERT_GE(second_fd, 0);

    ASSERT_TRUE(SendAll(
        second_fd,
        "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    const auto peer_closed = WaitForPeerClose(second_fd, 1000);

    ::close(second_fd);
    ::close(first_fd);
    server.Stop();

    EXPECT_TRUE(peer_closed);
}

TEST(HttpServerBackpressure, HighWatermarkDisconnectsSlowClient) {
    WebServerConfig config;
    config.port = kPressurePort;
    config.worker_count = 1;
    config.ring.io_timeout = 1ms;
    config.timeouts.inactivity = 5s;
    config.backpressure.send_queue_max_pending = 16;
    config.backpressure.send_queue_high_watermark = 1;
    config.backpressure.send_queue_low_watermark = 0;
    config.backpressure.disconnect_on_high_watermark = true;

    WebServer server(config);
    server.Get("/", [](RequestContext& ctx) {
        ctx.response.Body("ok").Send();
    });
    server.Start();
    std::this_thread::sleep_for(200ms);

    const int fd = ConnectTcp(kPressurePort);
    ASSERT_GE(fd, 0);

    ASSERT_TRUE(SendAll(fd, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));
    const auto peer_closed = WaitForPeerClose(fd, 1000);

    ::close(fd);
    server.Stop();

    EXPECT_TRUE(peer_closed);
}

TEST(HttpServerShutdown, StopDrainsAndClosesActiveSessions) {
    WebServerConfig config;
    config.port = kShutdownPort;
    config.worker_count = 1;
    config.ring.io_timeout = 1ms;
    config.timeouts.inactivity = 5s;
    config.shutdown.drain_timeout = 300ms;
    config.shutdown.force_close_timeout = 300ms;

    WebServer server(config);
    server.Get("/", [](RequestContext& ctx) {
        ctx.response.Body("ok").Send();
    });
    server.Start();
    std::this_thread::sleep_for(200ms);

    const int fd = ConnectTcp(kShutdownPort);
    ASSERT_GE(fd, 0);

    std::thread stopper([&server] { server.Stop(); });
    const auto peer_closed = WaitForPeerClose(fd, 1500);

    ::close(fd);
    stopper.join();

    EXPECT_TRUE(peer_closed);
}
