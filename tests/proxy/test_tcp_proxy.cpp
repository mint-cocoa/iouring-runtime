#include <iouring_runtime/proxy/TcpProxyServer.h>

#include <gtest/gtest.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

using SslCtxHandle = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using SslHandle = std::unique_ptr<SSL, decltype(&SSL_free)>;

constexpr char kTestCertPem[] =
R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIURCu2gcFrGaH3ar1oF6q1b9/rgTAwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDQyNDA0MzQzMFoXDTI3MDQy
NDA0MzQzMFowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEApRuqgt4b5QIKbyVtlW59pJzCBVDVoM6rj35/ICEpjz2z
b8KmS5YhxZpSiBvlj35CyqFtDoMi7RaSmhXhUNJb16gV66m0F9atQl587jDpS/nS
YMmp9ebTJ6Hbfy+O5vk3hNzi/vmaoQO/vOFcksEBsNTM2ZuHGYXrmWy3lEvcrFNn
qzmlm9BLSlwh5OSvorHmh2DPyFeJkcxtHS9NbjeBcgkUIXToPHWCG9fDAaC6aIRQ
hWyK/CXacEIOZfDQygRrWkGp3ervaa5Mg7VgTYguFttPwXPyJ/zPOEARzIiOwM/R
dtbav45cmPQwQAsE4+yGOaa/vkG5zWSFtqmSvt9gzQIDAQABo1MwUTAdBgNVHQ4E
FgQUeyOy5okhBBcOcNOP2KHrugtw1Y0wHwYDVR0jBBgwFoAUeyOy5okhBBcOcNOP
2KHrugtw1Y0wDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAhBZD
zxFZG+QW8Do9EydRSYot9mwjt6fglRKlT0rt8juQuezlvZBR0dZhYd1L1eUZBFHc
BZ0Fin5ivOIVN/kgDaBTaSf9/QCohWdAa4+4k64/VzXeIVmqtgEOy32jXOXppYI9
Le25zW0t6Hrm+ZBBud07QRUo8+ZMXbdMzAJI3IOC0EKEr+kvPpTmlg3mMl1vDVI2
BAHmbiNbn2u98F3uZORM7Yzuuzy3Ebi5gGveNZ5ATVgI7iVyVSCiMhepL10ALePC
iCWNytN4wLUdhgs6h5u7TSxyYOR0WemfY+8juX9n47Fl/iwNSo8t2YOLka/WIJfx
nb3Dt/uGnrMazSjVwg==
-----END CERTIFICATE-----
)";

constexpr char kTestKeyPem[] =
R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQClG6qC3hvlAgpv
JW2Vbn2knMIFUNWgzquPfn8gISmPPbNvwqZLliHFmlKIG+WPfkLKoW0OgyLtFpKa
FeFQ0lvXqBXrqbQX1q1CXnzuMOlL+dJgyan15tMnodt/L47m+TeE3OL++ZqhA7+8
4VySwQGw1MzZm4cZheuZbLeUS9ysU2erOaWb0EtKXCHk5K+iseaHYM/IV4mRzG0d
L01uN4FyCRQhdOg8dYIb18MBoLpohFCFbIr8JdpwQg5l8NDKBGtaQand6u9prkyD
tWBNiC4W20/Bc/In/M84QBHMiI7Az9F21tq/jlyY9DBACwTj7IY5pr++QbnNZIW2
qZK+32DNAgMBAAECggEAHpMI+EL5F2dRV1Ou/0T+r0T4CNP3O1gKHQBnuiNp9sSO
Pdvlv4mFp3n0XpAL6o6O/8b00wMlrJHJhA8ZMkZCC6Vsqb87Qk7WtruYluMn0JHq
i1HMz2R3JZerddnwtITh6xCcWGsQX1C6aNeytNpY3c4KQSGz2t4Zysd/F08VBjP/
dGeqNAd9V6kfQm2Yh7EXel4B45lK0FDgHqfFjURt9qTKkooWMMLwlqA/3A4pH7Yv
jLBjNLSXXq5RZ5CmkTz3ezbJdcZF+ZwEU/XjTlSXa1e4b5NImqjPZFgaoZxxxVB4
h37pcOUnjMvJft2g/vBW9NH8dMIt1Z8+xzLXcab0YQKBgQDZOjsD0+k/ScVZ8Cvr
8V4TSVY9/naXZOtQB8TaZP+5eOYgc+AHD0qvrVdDuYEMl9FJoXIrVxL+nvmSrSgB
deUMzO31yGkfu3M/SdCr9lSJRYi1CfF3l+jlhwu8C7od7wMRMoUxXWYjNvf+e2tU
5o9ZQWd4LGhM4YeDCqUbrepThwKBgQDCk/IBN8A0WjrVNm7023tUxcXL+ZGqQIu2
/0+yjM3ndyNDg0t0xrWtnDd1ZzddMhXDfzuvBUSOHyCnz7EOELcQakM8V1Opi2zg
rO/bkkSREyepPppwJLhnpI76QJKhYjS/wuV4cuS2jqJ0CFAJ1twfh3/irmXGMkue
zWWHAMpmCwKBgB4/s5QW4DYF6ctaXGNgmv8IiU/crVrrnLDqRAk9YzbKlRET0bBp
+rZ35tzxafz87xHXv3Q1jdmS/aHlJtr9NcYQi1pca/cVPTkk6D/p71+mJSFAnzVO
HApKQeIPp/0fGUg9xrpXDS/+KY5wVUPfTzrYlrXvSR7bkOk4hEFUeHrlAoGBAI5L
UUSQt8/RWIsKBrSsOZIyG8diWKNQFpyaM3dYv0+Rcclyn+DjXIN85ZXHCNbvxLi2
ODhzOsJQgzoVid7GXIn1m/vpxPYNT28zKSnC2r/ZaTy82oY+ZFjvH9xlEpZQ0lCW
P+gI5beXsbf3SEkZxtqL5fspHKaHq4OTo7bJ3jonAoGAb1AUGSUZZsjvG6g7a6T9
sH1w12gfv9/J5IfkI9g8PAE5fZkZtfsQ9305v2l2b0G8YZwTLx39pjkp35RxXvk+
fUvz+JlRxTV5TfhX6zecsve9z7kqNm1Tn3rcFD1KZiDyGRCoQEAfaRG/MX/41dnm
V7ei5pS9FahzFEC7b+NMmiU=
-----END PRIVATE KEY-----
)";

constexpr char kReloadedCertPem[] =
R"(-----BEGIN CERTIFICATE-----
MIIDGzCCAgOgAwIBAgIUE49ulH/HbJADndIA2RrLLyTcuRUwDQYJKoZIhvcNAQEL
BQAwHTEbMBkGA1UEAwwSbG9jYWxob3N0LXJlbG9hZGVkMB4XDTI2MDQyNDA0NTU1
NloXDTI3MDQyNDA0NTU1NlowHTEbMBkGA1UEAwwSbG9jYWxob3N0LXJlbG9hZGVk
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA2yK6TJiwFwHBGkENCzht
x92aFcA5i4cJNWSnj1YhiBWGRlmk56bGHyXfDPd3R1FIALgzdn6k9F4tzmrtKOZH
3SXq/7UcCWF6SaqjAx7js8HZ0xmeJJ6WqEhawQZ+ZmGhiU/Mlx9K/Q97jcn+A1sx
XboHfxuuX7xJloBnCGNeOZku29PFKzeMFdFJ+jCW7N9hT+Tjpuyw9u5a2GNmf7Uy
h5v1hkQ1uB8JQ2npFPX2era48Nyq2hWTgYTDHj2IC+Zz5SR4GoQY2kxTmEs6rPNA
QllTvweyHZKDrrVEniNB7EYIrTmwT2ritcVdhzl7X8clDr3b4qBGAAdl/OqGlPdk
QQIDAQABo1MwUTAdBgNVHQ4EFgQUTWQZsAMqQ7evXbKWl5GSM1FfoY8wHwYDVR0j
BBgwFoAUTWQZsAMqQ7evXbKWl5GSM1FfoY8wDwYDVR0TAQH/BAUwAwEB/zANBgkq
hkiG9w0BAQsFAAOCAQEAkII3nkXWWpK37x9oDSExov9V0qzZCtkdPiz3jqmpaMY3
kA1hxluRrjHT8KW5Rdn1M4WkDzmP4tWrFQSknTpt/5sglxsHpNJKJRwwGEtSi2Re
34zS9wX//Qq+t4NTjHndZKWDvRsdr5JiiB8IcWmaAgUsVaCpQrBqujDE5urFKU0u
tglqhhybYwkXIvhWutTx6AQGbMW6/1Bf+jAuy9Gv/WsdhyhuzBxcz2TKdEWo2Cil
g9pYWu3+ulyCKTJ9FiScU1sEJCdOVatM6zE7lWh0MAuYLkFwyoI6XoQ+ztLO4rml
kAvyZzfe+IYjPC4W/oubDpDx2Iosw8LOsMf7WncK7g==
-----END CERTIFICATE-----
)";

constexpr char kReloadedKeyPem[] =
R"(-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDbIrpMmLAXAcEa
QQ0LOG3H3ZoVwDmLhwk1ZKePViGIFYZGWaTnpsYfJd8M93dHUUgAuDN2fqT0Xi3O
au0o5kfdJer/tRwJYXpJqqMDHuOzwdnTGZ4knpaoSFrBBn5mYaGJT8yXH0r9D3uN
yf4DWzFdugd/G65fvEmWgGcIY145mS7b08UrN4wV0Un6MJbs32FP5OOm7LD27lrY
Y2Z/tTKHm/WGRDW4HwlDaekU9fZ6trjw3KraFZOBhMMePYgL5nPlJHgahBjaTFOY
Szqs80BCWVO/B7IdkoOutUSeI0HsRgitObBPauK1xV2HOXtfxyUOvdvioEYAB2X8
6oaU92RBAgMBAAECggEAAyBJDxdj9o7qomC/VdFtnu064LZqO75CM98mXjfkRCOm
eDrh3txr2dieGugJJBPQshKA7XEuEj6pasTKcp4iYeN6qWUOPkO68c0XKT2XyLRY
CYXXrX3qim/lPB2QPjM7WsfnAg94uS1TuUG97pLA+AjTtrGQnTBNznGe6zukzvjN
wa0FkFSviGu4mBFvv8XcKbxLBggW8HKabXfVccYReFYNa9IukHrDPCh8+8GnueIz
IdZo1xKc0hMM1X394jPnFHSb7CKWloCCV/W3z3hRObqvsQ3PDYwpaThBdbCC7P6U
FXE+p/Ws0S6nX0pGLyDZXtGRQ2d34abhuv8Oo0oNaQKBgQD/lQZ0gGAnqjMdGAmE
O7GWoapWl+KL8bVtpNO9/c+FmJ9FcCXs2cEkDm7nLlEwRf5zep/TvDXU+8UP9JUi
Pt7Y2YaBK1R8DmWc8ReRC+9c3jg1ex0KBgAmblsgkPUBWqc61BpDhDdEvGSS+Hg5
hBa/sk1i46mL0l0oszTJAGl8lQKBgQDbfnKdpodlho75cZ/vcv4MMdRnnQCvsuix
aVB4KzD3ojvUhO11iPGjtT35y6J4m2v4PC1IJkwMfV/FejU7j2apDIYO+6RLqb7H
REcvpn7lbpkDbI8TDqcqjJ+VLDpOyUNJ173wP57RtMFvGSs2U2qhpyFgJR4ULD5P
KJjV6fPx/QKBgHS4sGwAPOmgdxtwYNg9EsuCJXLKhrH2vy7FudXo1h9jJknl/8v9
XpGC7e8yXe236Y7/j4J5T/RmzwaMYpdzMv7ByQWwrjLBuHJGQMcDNlupOO5jvU1Y
lDT47TEgQmRN7FWXjC8NcNg5WY0tYGqUAneljGX/Ju0uM1FlFlQ9le9JAoGAYpUh
b3NLHqtX7es9tTNFrT54Mu1OUJufbYdqj/5+KMrh6mqlqIOQXEHcCySN3XOZA84r
XFtvDrEE4dIotI6MwmKQl4woV59uw1sESf+SwQNykBojDKEpLbApQLuzmI1GvpS6
h3xbnb58nqSERwsiOmaxU9h38QGKHs2SD4nbehUCgYAmq/MmHBiG3c4fhtCQSF5M
KPN/8xMF4rkUm9UVH8ExkuRmG0n4WLLizQlwa/c4UjgfVNM/NxXam3oobPiVyo0J
x1vJ/KnqeUzUDgFzGc/LDMYxoQnlA8Z1Avg+iFN340M+6cGSW8NpTqLjnW5/mWyz
sLU+f9/BJsWtB3gtbsNzhQ==
-----END PRIVATE KEY-----
)";

void WriteTextFile(const std::string& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good());
    out << content;
    out.close();
    ASSERT_TRUE(out.good());
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

struct TestTlsFiles {
    std::string dir;
    std::string cert_path;
    std::string key_path;

    ~TestTlsFiles() {
        if (!cert_path.empty()) {
            ::unlink(cert_path.c_str());
        }
        if (!key_path.empty()) {
            ::unlink(key_path.c_str());
        }
        if (!dir.empty()) {
            ::rmdir(dir.c_str());
        }
    }
};

struct TestChallengeFiles {
    std::string dir;
    std::string challenge_dir;
    std::string token;
    std::string body;
    std::string path;

    ~TestChallengeFiles() {
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    }
};

TestTlsFiles CreateTestTlsFiles() {
    TestTlsFiles files;
    char dir_template[] = "/tmp/iouring-runtime-tls-XXXXXX";
    char* created = ::mkdtemp(dir_template);
    EXPECT_NE(created, nullptr);
    if (!created) {
        return files;
    }

    files.dir = created;
    files.cert_path = files.dir + "/cert.pem";
    files.key_path = files.dir + "/key.pem";
    WriteTextFile(files.cert_path, kTestCertPem);
    WriteTextFile(files.key_path, kTestKeyPem);
    return files;
}

TestChallengeFiles CreateTestChallengeFiles() {
    TestChallengeFiles files;
    char dir_template[] = "/tmp/iouring-runtime-acme-XXXXXX";
    char* created = ::mkdtemp(dir_template);
    EXPECT_NE(created, nullptr);
    if (!created) {
        return files;
    }

    files.dir = created;
    files.challenge_dir = files.dir + "/.well-known/acme-challenge";
    files.token = "sample-token";
    files.body = "sample-key-authorization";
    std::error_code ec;
    std::filesystem::create_directories(files.challenge_dir, ec);
    EXPECT_FALSE(ec);
    files.path = files.challenge_dir + "/" + files.token;
    WriteTextFile(files.path, files.body);
    return files;
}

std::uint16_t ReserveTcpPort() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(fd, 0);

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool SendAll(int fd, std::string_view data) {
    const char* current = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const auto written = ::send(fd, current, remaining, 0);
        if (written <= 0) {
            return false;
        }
        current += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

std::string RecvExact(int fd, std::size_t bytes) {
    std::string out;
    out.resize(bytes);

    std::size_t total = 0;
    while (total < bytes) {
        const auto n =
            ::recv(fd, out.data() + total, bytes - total, 0);
        if (n <= 0) {
            break;
        }
        total += static_cast<std::size_t>(n);
    }
    out.resize(total);
    return out;
}

std::string RecvUntilClose(int fd) {
    std::string out;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto n = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (n <= 0) {
            break;
        }
        out.append(buffer.data(), static_cast<std::size_t>(n));
    }
    return out;
}

int ConnectWithRetry(std::uint16_t port,
                     std::chrono::milliseconds timeout = 1500ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }

        ::close(fd);
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    return -1;
}

class EchoServer {
public:
    EchoServer() {
        const int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        listen_fd_.store(listen_fd, std::memory_order_release);
        EXPECT_GE(listen_fd, 0);

        int opt = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                         sizeof(addr)),
                  0);
        EXPECT_EQ(::listen(listen_fd, 8), 0);

        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                                &len),
                  0);
        port_ = ntohs(addr.sin_port);

        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { Run(); });
    }

    ~EchoServer() {
        Stop();
    }

    std::uint16_t Port() const {
        return port_;
    }

    void Stop() {
        running_.store(false, std::memory_order_release);

        const int listen_fd = listen_fd_.exchange(-1, std::memory_order_acq_rel);
        if (listen_fd >= 0) {
            ::shutdown(listen_fd, SHUT_RDWR);
            ::close(listen_fd);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void Run() {
        while (running_.load(std::memory_order_acquire)) {
            pollfd pfd{};
            pfd.fd = listen_fd_.load(std::memory_order_acquire);
            pfd.events = POLLIN;

            if (pfd.fd < 0) {
                break;
            }

            const int rc = ::poll(&pfd, 1, 100);
            if (rc == 0) {
                continue;
            }
            if (rc < 0) {
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                break;
            }
            if ((pfd.revents & POLLIN) == 0) {
                continue;
            }

            const int client = ::accept(pfd.fd, nullptr, nullptr);
            if (client < 0) {
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            std::thread(&EchoServer::EchoClient, client).detach();
        }
    }

    static void EchoClient(int client) {
        std::array<char, 4096> buf{};
        for (;;) {
            const auto n = ::recv(client, buf.data(), buf.size(), 0);
            if (n <= 0) {
                break;
            }
            std::size_t sent = 0;
            while (sent < static_cast<std::size_t>(n)) {
                const auto written = ::send(client, buf.data() + sent,
                                            static_cast<std::size_t>(n) - sent,
                                            0);
                if (written <= 0) {
                    ::close(client);
                    return;
                }
                sent += static_cast<std::size_t>(written);
            }
        }
        ::close(client);
    }

    std::atomic<int> listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::uint16_t port_{0};
    std::thread thread_;
};

class FixedResponseServer {
public:
    explicit FixedResponseServer(std::string response)
        : response_(std::move(response)) {
        const int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        listen_fd_.store(listen_fd, std::memory_order_release);
        EXPECT_GE(listen_fd, 0);

        int opt = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                         sizeof(addr)),
                  0);
        EXPECT_EQ(::listen(listen_fd, 8), 0);

        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                                &len),
                  0);
        port_ = ntohs(addr.sin_port);

        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { Run(); });
    }

    ~FixedResponseServer() {
        Stop();
    }

    std::uint16_t Port() const {
        return port_;
    }

    void Stop() {
        running_.store(false, std::memory_order_release);

        const int listen_fd = listen_fd_.exchange(-1, std::memory_order_acq_rel);
        if (listen_fd >= 0) {
            ::shutdown(listen_fd, SHUT_RDWR);
            ::close(listen_fd);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void Run() {
        while (running_.load(std::memory_order_acquire)) {
            pollfd pfd{};
            pfd.fd = listen_fd_.load(std::memory_order_acquire);
            pfd.events = POLLIN;

            if (pfd.fd < 0) {
                break;
            }

            const int rc = ::poll(&pfd, 1, 100);
            if (rc <= 0) {
                continue;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                break;
            }
            if ((pfd.revents & POLLIN) == 0) {
                continue;
            }

            const int client = ::accept(pfd.fd, nullptr, nullptr);
            if (client < 0) {
                continue;
            }

            std::thread(&FixedResponseServer::RespondClient, response_, client)
                .detach();
        }
    }

    static void RespondClient(std::string response, int client) {
        std::array<char, 4096> buf{};
        const auto n = ::recv(client, buf.data(), buf.size(), 0);
        if (n > 0) {
            SendAll(client, response);
        }
        ::close(client);
    }

    std::string response_;
    std::atomic<int> listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::uint16_t port_{0};
    std::thread thread_;
};

std::string PeerCommonName(SSL* ssl) {
    X509* cert = SSL_get1_peer_certificate(ssl);
    if (!cert) {
        return {};
    }

    std::array<char, 256> common_name{};
    X509_NAME_get_text_by_NID(
        X509_get_subject_name(cert), NID_commonName,
        common_name.data(), common_name.size());
    X509_free(cert);
    return common_name.data();
}

bool TlsSendAll(SSL* ssl, std::string_view data) {
    const char* current = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        std::size_t written = 0;
        const int rc = SSL_write_ex(ssl, current, remaining, &written);
        if (rc != 1) {
            return false;
        }
        current += written;
        remaining -= written;
    }
    return true;
}

std::string TlsRecvExact(SSL* ssl, std::size_t bytes) {
    std::string out;
    out.resize(bytes);

    std::size_t total = 0;
    while (total < bytes) {
        std::size_t read = 0;
        const int rc = SSL_read_ex(ssl, out.data() + total, bytes - total, &read);
        if (rc != 1) {
            break;
        }
        total += read;
    }
    out.resize(total);
    return out;
}

} // namespace

TEST(TcpProxyServerTest, ProxiesTcpTrafficToUpstream) {
    EchoServer upstream;

    iouring_runtime::proxy::TcpProxyConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = ReserveTcpPort();
    config.upstream_host = "127.0.0.1";
    config.upstream_port = upstream.Port();
    config.worker_count = 1;
    config.ring.queue_depth = 128;
    config.ring.buf_count = 64;
    config.ring.buf_size = 4096;
    config.ring.io_timeout = 1ms;
    config.timeouts.connect = 500ms;
    config.timeouts.inactivity = 2s;

    iouring_runtime::proxy::TcpProxyServer proxy(config);
    proxy.Start();

    const int client = ConnectWithRetry(config.listen_port);
    ASSERT_GE(client, 0);

    constexpr std::string_view kPayload = "hello through tcp proxy";
    ASSERT_TRUE(SendAll(client, kPayload));
    EXPECT_EQ(RecvExact(client, kPayload.size()), kPayload);

    ::close(client);
    proxy.Stop();
    upstream.Stop();
}

TEST(TcpProxyServerTest, WritesRuntimeMetricsSnapshotAtomically) {
    EchoServer upstream;
    const auto metrics_dir =
        std::filesystem::temp_directory_path() /
        ("iouring-runtime-metrics-" + std::to_string(::getpid()));
    std::filesystem::remove_all(metrics_dir);
    std::filesystem::create_directories(metrics_dir);
    const auto metrics_file = metrics_dir / "tcp_reverse_proxy.metrics.json";

    iouring_runtime::proxy::TcpProxyConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = ReserveTcpPort();
    config.upstream_host = "127.0.0.1";
    config.upstream_port = upstream.Port();
    config.worker_count = 2;
    config.ring.queue_depth = 128;
    config.ring.buf_count = 64;
    config.ring.buf_size = 4096;
    config.ring.io_timeout = 1ms;
    config.timeouts.connect = 500ms;
    config.timeouts.inactivity = 2s;
    config.metrics.file_path = metrics_file.string();
    config.metrics.interval = 25ms;
    config.upstream_routes.push_back({
        .hostname = "demo.example.test",
        .upstream_host = "127.0.0.1",
        .upstream_port = upstream.Port(),
    });

    iouring_runtime::proxy::TcpProxyServer proxy(config);
    proxy.Start();
    ASSERT_TRUE(proxy.WriteMetricsSnapshot());

    const auto body = ReadWholeFile(metrics_file);
    EXPECT_NE(body.find("\"service\":\"tcp_reverse_proxy\""), std::string::npos);
    EXPECT_NE(body.find("\"configured_worker_count\":2"), std::string::npos);
    EXPECT_NE(body.find("\"running_worker_count\":2"), std::string::npos);
    EXPECT_NE(body.find("\"demo.example.test\""), std::string::npos);
    EXPECT_NE(body.find("\"total_live_sessions\""), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(metrics_file.string() + ".tmp." +
                                         std::to_string(static_cast<long long>(::getpid()))));

    proxy.Stop();
    upstream.Stop();
    std::filesystem::remove_all(metrics_dir);
}

TEST(TcpProxyServerTest, TerminatesDownstreamTlsAndProxiesTraffic) {
    EchoServer upstream;
    auto tls_files = CreateTestTlsFiles();
    ASSERT_FALSE(tls_files.cert_path.empty());
    ASSERT_FALSE(tls_files.key_path.empty());

    iouring_runtime::proxy::TcpProxyConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = ReserveTcpPort();
    config.upstream_host = "127.0.0.1";
    config.upstream_port = upstream.Port();
    config.worker_count = 1;
    config.ring.queue_depth = 128;
    config.ring.buf_count = 64;
    config.ring.buf_size = 4096;
    config.ring.io_timeout = 1ms;
    config.timeouts.connect = 500ms;
    config.timeouts.inactivity = 2s;
    config.downstream_tls.certificate_chain_file = tls_files.cert_path;
    config.downstream_tls.private_key_file = tls_files.key_path;

    iouring_runtime::proxy::TcpProxyServer proxy(config);
    proxy.Start();

    ASSERT_EQ(OPENSSL_init_ssl(0, nullptr), 1);
    SslCtxHandle client_ctx(SSL_CTX_new(TLS_client_method()), &SSL_CTX_free);
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);

    const int client_fd = ConnectWithRetry(config.listen_port);
    ASSERT_GE(client_fd, 0);

    SslHandle ssl(SSL_new(client_ctx.get()), &SSL_free);
    ASSERT_NE(ssl, nullptr);
    ASSERT_EQ(SSL_set_fd(ssl.get(), client_fd), 1);
    ASSERT_EQ(SSL_connect(ssl.get()), 1);

    constexpr std::string_view kPayload = "hello through tls proxy";
    ASSERT_TRUE(TlsSendAll(ssl.get(), kPayload));
    EXPECT_EQ(TlsRecvExact(ssl.get(), kPayload.size()), kPayload);

    ::close(client_fd);
    proxy.Stop();
    upstream.Stop();
}

TEST(TcpProxyServerTest, FlushesUpstreamResponseBeforeClosingDownstreamTls) {
    const std::string response(30 * 1024 * 1024, 'x');
    FixedResponseServer upstream(response);
    auto tls_files = CreateTestTlsFiles();
    ASSERT_FALSE(tls_files.cert_path.empty());
    ASSERT_FALSE(tls_files.key_path.empty());

    iouring_runtime::proxy::TcpProxyConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = ReserveTcpPort();
    config.upstream_host = "127.0.0.1";
    config.upstream_port = upstream.Port();
    config.worker_count = 1;
    config.ring.queue_depth = 128;
    config.ring.buf_count = 64;
    config.ring.buf_size = 4096;
    config.ring.io_timeout = 1ms;
    config.timeouts.connect = 500ms;
    config.timeouts.inactivity = 2s;
    config.downstream_tls.certificate_chain_file = tls_files.cert_path;
    config.downstream_tls.private_key_file = tls_files.key_path;

    iouring_runtime::proxy::TcpProxyServer proxy(config);
    proxy.Start();

    ASSERT_EQ(OPENSSL_init_ssl(0, nullptr), 1);
    SslCtxHandle client_ctx(SSL_CTX_new(TLS_client_method()), &SSL_CTX_free);
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);

    const int client_fd = ConnectWithRetry(config.listen_port);
    ASSERT_GE(client_fd, 0);

    SslHandle ssl(SSL_new(client_ctx.get()), &SSL_free);
    ASSERT_NE(ssl, nullptr);
    ASSERT_EQ(SSL_set_fd(ssl.get(), client_fd), 1);
    ASSERT_EQ(SSL_connect(ssl.get()), 1);

    ASSERT_TRUE(TlsSendAll(ssl.get(), "GET /downloading HTTP/1.1\r\n\r\n"));
    EXPECT_EQ(TlsRecvExact(ssl.get(), response.size()), response);

    ::close(client_fd);
    proxy.Stop();
    upstream.Stop();
}

TEST(TcpProxyServerTest, FlushesLargeSniRoutedUpstreamResponse) {
    FixedResponseServer default_upstream("default");
    const std::string response(30 * 1024 * 1024, 'x');
    FixedResponseServer routed_upstream(response);
    auto tls_files = CreateTestTlsFiles();
    ASSERT_FALSE(tls_files.cert_path.empty());
    ASSERT_FALSE(tls_files.key_path.empty());

    iouring_runtime::proxy::TcpProxyConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = ReserveTcpPort();
    config.upstream_host = "127.0.0.1";
    config.upstream_port = default_upstream.Port();
    config.upstream_routes.push_back({
        .hostname = "speed.example.test",
        .upstream_host = "127.0.0.1",
        .upstream_port = routed_upstream.Port(),
    });
    config.worker_count = 1;
    config.ring.queue_depth = 128;
    config.ring.buf_count = 64;
    config.ring.buf_size = 4096;
    config.ring.io_timeout = 1ms;
    config.timeouts.connect = 500ms;
    config.timeouts.inactivity = 2s;
    config.downstream_tls.certificate_chain_file = tls_files.cert_path;
    config.downstream_tls.private_key_file = tls_files.key_path;

    iouring_runtime::proxy::TcpProxyServer proxy(config);
    proxy.Start();

    ASSERT_EQ(OPENSSL_init_ssl(0, nullptr), 1);
    SslCtxHandle client_ctx(SSL_CTX_new(TLS_client_method()), &SSL_CTX_free);
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);

    const int client_fd = ConnectWithRetry(config.listen_port);
    ASSERT_GE(client_fd, 0);

    SslHandle ssl(SSL_new(client_ctx.get()), &SSL_free);
    ASSERT_NE(ssl, nullptr);
    ASSERT_EQ(SSL_set_tlsext_host_name(ssl.get(), "speed.example.test"), 1);
    ASSERT_EQ(SSL_set_fd(ssl.get(), client_fd), 1);
    ASSERT_EQ(SSL_connect(ssl.get()), 1);

    ASSERT_TRUE(TlsSendAll(ssl.get(), "GET /downloading HTTP/1.1\r\n\r\n"));
    EXPECT_EQ(TlsRecvExact(ssl.get(), response.size()), response);

    ::close(client_fd);
    proxy.Stop();
    default_upstream.Stop();
    routed_upstream.Stop();
}

TEST(TcpProxyServerTest, RoutesDownstreamTlsBySni) {
    FixedResponseServer default_upstream("default-upstream");
    FixedResponseServer routed_upstream("routed-upstream");
    auto tls_files = CreateTestTlsFiles();
    ASSERT_FALSE(tls_files.cert_path.empty());
    ASSERT_FALSE(tls_files.key_path.empty());

    iouring_runtime::proxy::TcpProxyConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = ReserveTcpPort();
    config.upstream_host = "127.0.0.1";
    config.upstream_port = default_upstream.Port();
    config.upstream_routes.push_back({
        .hostname = "app.example.test",
        .upstream_host = "127.0.0.1",
        .upstream_port = routed_upstream.Port(),
    });
    config.worker_count = 1;
    config.ring.queue_depth = 128;
    config.ring.buf_count = 64;
    config.ring.buf_size = 4096;
    config.ring.io_timeout = 1ms;
    config.timeouts.connect = 500ms;
    config.timeouts.inactivity = 2s;
    config.downstream_tls.certificate_chain_file = tls_files.cert_path;
    config.downstream_tls.private_key_file = tls_files.key_path;

    iouring_runtime::proxy::TcpProxyServer proxy(config);
    proxy.Start();

    ASSERT_EQ(OPENSSL_init_ssl(0, nullptr), 1);
    SslCtxHandle client_ctx(SSL_CTX_new(TLS_client_method()), &SSL_CTX_free);
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);

    const int routed_fd = ConnectWithRetry(config.listen_port);
    ASSERT_GE(routed_fd, 0);

    SslHandle routed_ssl(SSL_new(client_ctx.get()), &SSL_free);
    ASSERT_NE(routed_ssl, nullptr);
    ASSERT_EQ(SSL_set_tlsext_host_name(routed_ssl.get(), "app.example.test"), 1);
    ASSERT_EQ(SSL_set_fd(routed_ssl.get(), routed_fd), 1);
    ASSERT_EQ(SSL_connect(routed_ssl.get()), 1);

    ASSERT_TRUE(TlsSendAll(routed_ssl.get(), "ping"));
    EXPECT_EQ(TlsRecvExact(routed_ssl.get(), 15), "routed-upstream");

    const int default_fd = ConnectWithRetry(config.listen_port);
    ASSERT_GE(default_fd, 0);

    SslHandle default_ssl(SSL_new(client_ctx.get()), &SSL_free);
    ASSERT_NE(default_ssl, nullptr);
    ASSERT_EQ(SSL_set_tlsext_host_name(default_ssl.get(), "other.example.test"), 1);
    ASSERT_EQ(SSL_set_fd(default_ssl.get(), default_fd), 1);
    ASSERT_EQ(SSL_connect(default_ssl.get()), 1);

    ASSERT_TRUE(TlsSendAll(default_ssl.get(), "ping"));
    EXPECT_EQ(TlsRecvExact(default_ssl.get(), 16), "default-upstream");

    ::close(routed_fd);
    ::close(default_fd);
    proxy.Stop();
    default_upstream.Stop();
    routed_upstream.Stop();
}

TEST(TcpProxyServerTest, ReloadsDownstreamTlsWithoutDroppingExistingSessions) {
    EchoServer upstream;
    auto tls_files = CreateTestTlsFiles();
    ASSERT_FALSE(tls_files.cert_path.empty());
    ASSERT_FALSE(tls_files.key_path.empty());

    iouring_runtime::proxy::TcpProxyConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = ReserveTcpPort();
    config.upstream_host = "127.0.0.1";
    config.upstream_port = upstream.Port();
    config.worker_count = 1;
    config.ring.queue_depth = 128;
    config.ring.buf_count = 64;
    config.ring.buf_size = 4096;
    config.ring.io_timeout = 1ms;
    config.timeouts.connect = 500ms;
    config.timeouts.inactivity = 2s;
    config.downstream_tls.certificate_chain_file = tls_files.cert_path;
    config.downstream_tls.private_key_file = tls_files.key_path;

    iouring_runtime::proxy::TcpProxyServer proxy(config);
    proxy.Start();

    ASSERT_EQ(OPENSSL_init_ssl(0, nullptr), 1);
    SslCtxHandle client_ctx(SSL_CTX_new(TLS_client_method()), &SSL_CTX_free);
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx.get(), SSL_VERIFY_NONE, nullptr);

    const int first_fd = ConnectWithRetry(config.listen_port);
    ASSERT_GE(first_fd, 0);

    SslHandle first_ssl(SSL_new(client_ctx.get()), &SSL_free);
    ASSERT_NE(first_ssl, nullptr);
    ASSERT_EQ(SSL_set_fd(first_ssl.get(), first_fd), 1);
    ASSERT_EQ(SSL_connect(first_ssl.get()), 1);
    EXPECT_EQ(PeerCommonName(first_ssl.get()), "localhost");

    constexpr std::string_view kBeforeReload = "before reload";
    ASSERT_TRUE(TlsSendAll(first_ssl.get(), kBeforeReload));
    EXPECT_EQ(TlsRecvExact(first_ssl.get(), kBeforeReload.size()), kBeforeReload);

    WriteTextFile(tls_files.cert_path, kReloadedCertPem);
    WriteTextFile(tls_files.key_path, kReloadedKeyPem);
    ASSERT_TRUE(proxy.ReloadDownstreamTlsContext());

    constexpr std::string_view kAfterReload = "existing session still alive";
    ASSERT_TRUE(TlsSendAll(first_ssl.get(), kAfterReload));
    EXPECT_EQ(TlsRecvExact(first_ssl.get(), kAfterReload.size()), kAfterReload);

    const int second_fd = ConnectWithRetry(config.listen_port);
    ASSERT_GE(second_fd, 0);

    SslHandle second_ssl(SSL_new(client_ctx.get()), &SSL_free);
    ASSERT_NE(second_ssl, nullptr);
    ASSERT_EQ(SSL_set_fd(second_ssl.get(), second_fd), 1);
    ASSERT_EQ(SSL_connect(second_ssl.get()), 1);
    EXPECT_EQ(PeerCommonName(second_ssl.get()), "localhost-reloaded");

    constexpr std::string_view kNewSession = "new session after reload";
    ASSERT_TRUE(TlsSendAll(second_ssl.get(), kNewSession));
    EXPECT_EQ(TlsRecvExact(second_ssl.get(), kNewSession.size()), kNewSession);

    ::close(first_fd);
    ::close(second_fd);
    proxy.Stop();
    upstream.Stop();
}

TEST(TcpProxyServerTest, ServesCertbotHttp01ChallengeFiles) {
    EchoServer upstream;
    auto challenge_files = CreateTestChallengeFiles();
    ASSERT_FALSE(challenge_files.dir.empty());

    iouring_runtime::proxy::TcpProxyConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = ReserveTcpPort();
    config.upstream_host = "127.0.0.1";
    config.upstream_port = upstream.Port();
    config.worker_count = 1;
    config.ring.queue_depth = 128;
    config.ring.buf_count = 64;
    config.ring.buf_size = 4096;
    config.ring.io_timeout = 1ms;
    config.timeouts.connect = 500ms;
    config.timeouts.inactivity = 2s;
    config.certbot.challenge_host = "127.0.0.1";
    config.certbot.challenge_port = ReserveTcpPort();
    config.certbot.challenge_webroot = challenge_files.dir;

    iouring_runtime::proxy::TcpProxyServer proxy(config);
    proxy.Start();

    const int client = ConnectWithRetry(config.certbot.challenge_port);
    ASSERT_GE(client, 0);

    const std::string request =
        "GET /.well-known/acme-challenge/" + challenge_files.token +
        " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(SendAll(client, request));
    const auto response = RecvUntilClose(client);

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find(challenge_files.body), std::string::npos);

    ::close(client);
    proxy.Stop();
    upstream.Stop();
}
