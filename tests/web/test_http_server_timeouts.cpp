#include <iouring_runtime/web/WebServer.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace iouring_runtime::web;
using namespace std::chrono_literals;

namespace {

constexpr std::uint16_t kTimeoutPort = 19877;
constexpr std::uint16_t kLimitPort = 19878;
constexpr std::uint16_t kShutdownPort = 19879;
constexpr std::uint16_t kPressurePort = 19880;
constexpr std::uint16_t kStreamFailurePort = 19881;
constexpr std::uint16_t kDeferredPort = 19882;
constexpr std::uint16_t kFilePort = 19883;

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

std::string RecvUntilClose(int fd, int timeout_ms) {
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::string out;
    char buf[64 * 1024];
    while (true) {
        const auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            return out;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return out;
        }
        return out;
    }
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

std::size_t CountOccurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string_view BodyBytes(std::string_view payload) {
    const auto marker = payload.find("\r\n\r\n");
    if (marker == std::string_view::npos) {
        return {};
    }
    return payload.substr(marker + 4);
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

TEST(HttpServerBackpressure, RejectsNewSessionsPastWorkerLimitWith503) {
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

    const auto response = RecvWithTimeout(second_fd, 1000);
    const auto peer_closed = WaitForPeerClose(second_fd, 1000);

    ::close(second_fd);
    ::close(first_fd);
    server.Stop();

    EXPECT_NE(response.find("503 Service Unavailable"), std::string::npos);
    EXPECT_NE(response.find("Connection: close"), std::string::npos);
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

TEST(HttpServerStreaming, BodyCallbackFailureSendsSingleErrorResponse) {
    WebServerConfig config;
    config.port = kStreamFailurePort;
    config.worker_count = 1;
    config.ring.io_timeout = 1ms;
    config.timeouts.inactivity = 5s;

    WebServer server(config);
    server.PostStream("/upload", HttpStreamHandler{
        .on_body = [](RequestContext&, std::span<const std::byte>) {
            return false;
        },
    });
    server.Start();
    std::this_thread::sleep_for(200ms);

    const int fd = ConnectTcp(kStreamFailurePort);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(SendAll(fd,
                        "POST /upload HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Length: 5\r\n"
                        "\r\n"
                        "hello"));

    const auto first = RecvWithTimeout(fd, 1500);
    const auto second = RecvWithTimeout(fd, 200);
    ::close(fd);
    server.Stop();

    const auto combined = first + second;
    EXPECT_NE(combined.find("400 Bad Request"), std::string::npos);
    EXPECT_EQ(CountOccurrences(combined, "HTTP/1.1 "), 1u);
}

TEST(HttpServerStreaming, DeferredResponseDoesNotDispatchPipelinedRequestFirst) {
    WebServerConfig config;
    config.port = kDeferredPort;
    config.worker_count = 1;
    config.ring.io_timeout = 1ms;
    config.timeouts.inactivity = 5s;

    WebServer server(config);
    server.PostStream("/defer", HttpStreamHandler{
        .on_complete = [](RequestContext& ctx) {
            auto deferred = ctx.DeferResponse();
            std::this_thread::sleep_for(200ms);
            deferred.Response().Body("deferred").Send();
            deferred.Complete();
        },
    });
    server.Get("/fast", [](RequestContext& ctx) {
        ctx.response.Body("fast").Send();
    });
    server.Start();
    std::this_thread::sleep_for(200ms);

    const int fd = ConnectTcp(kDeferredPort);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(SendAll(fd,
                        "POST /defer HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Length: 0\r\n"
                        "\r\n"
                        "GET /fast HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "\r\n"));

    const auto response = RecvWithTimeout(fd, 1500);
    ::close(fd);
    server.Stop();

    EXPECT_NE(response.find("200 OK"), std::string::npos);
    EXPECT_NE(response.find("deferred"), std::string::npos);
    EXPECT_EQ(response.find("fast"), std::string::npos);
}

TEST(HttpServerFiles, StreamsFileLargerThanDefaultSendChunk) {
    char path[] = "/tmp/iouring-runtime-file-XXXXXX";
    const int file_fd = ::mkstemp(path);
    ASSERT_GE(file_fd, 0);

    std::string expected;
    expected.reserve(5 * 1024 * 1024 + 123);
    for (std::size_t i = 0; i < 5 * 1024 * 1024 + 123; ++i) {
        expected.push_back(static_cast<char>('a' + (i % 26)));
    }

    std::size_t written = 0;
    while (written < expected.size()) {
        const auto n = ::write(file_fd, expected.data() + written,
                               expected.size() - written);
        ASSERT_GT(n, 0);
        written += static_cast<std::size_t>(n);
    }
    ::close(file_fd);
    const std::string file_path = path;

    WebServerConfig config;
    config.port = kFilePort;
    config.worker_count = 1;
    config.ring.io_timeout = 1ms;
    config.timeouts.inactivity = 5s;

    WebServer server(config);
    server.Get("/file", [file_path](RequestContext& ctx) {
        ctx.response.SendFile(file_path, "application/octet-stream", 128 * 1024, 4);
    });
    server.Start();
    std::this_thread::sleep_for(200ms);

    const int fd = ConnectTcp(kFilePort);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(SendAll(fd,
        "GET /file HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));

    const auto response = RecvUntilClose(fd, 5000);
    ::close(fd);
    server.Stop();
    ::unlink(path);

    EXPECT_NE(response.find("200 OK"), std::string::npos);
    EXPECT_NE(response.find("Content-Length: " + std::to_string(expected.size())),
              std::string::npos);
    EXPECT_NE(response.find("Connection: close"), std::string::npos);
    EXPECT_EQ(BodyBytes(response), expected);
}
