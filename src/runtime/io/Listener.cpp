#include <iouring_runtime/core/Listener.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/IoRing.h>
#include <iouring_runtime/observability/Profiler.h>

#include <liburing.h>
#include <iouring_runtime/observability/Logging.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

namespace obs = iouring_runtime::observability;
namespace {
constexpr auto kLogCategory = obs::LogCategory::kListener;
}

namespace iouring_runtime::core::io {

namespace {

int CreateListenSocket(const Address& addr) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    // Disable Nagle
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(addr.port);
    ::inet_pton(AF_INET, addr.host.c_str(), &sin.sin_addr);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) < 0) {
        ::close(fd);
        return -1;
    }

    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

bool SetNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if ((flags & O_NONBLOCK) != 0) return true;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

Listener::Listener(IoRing& ring, buffer::BufferPool& pool,
                   const Address& addr, SessionFactory factory,
                   ContextId shard_id, std::uint32_t max_sessions)
    : ring_(ring)
    , pool_(pool)
    , addr_(addr)
    , factory_(std::move(factory))
    , shard_id_(shard_id)
    , max_sessions_(max_sessions) {}

std::expected<void, io::IoError> Listener::Start() {
    int fd = CreateListenSocket(addr_);
    if (fd < 0) {
        obs::LogError(kLogCategory, "Listener: failed to create listen socket on {}:{}", addr_.host, addr_.port);
        return std::unexpected(IoError::kBindFailed);
    }

    listen_fd_.Reset(fd);
    accept_ev_.SetOwner(shared_from_this());

    obs::LogInfo(kLogCategory, "Listener: listening on {}:{}", addr_.host, addr_.port);
    if (!ring_.PrepAcceptMultishot(accept_ev_, listen_fd_.Get())) {
        obs::LogError(kLogCategory, "Listener: PrepAcceptMultishot failed (SQE full)");
        return std::unexpected(IoError::kListenFailed);
    }
    ring_.Submit();
    return {};
}

void Listener::Stop() {
    listen_fd_.Reset();
}

void Listener::OnAccept(ring::AcceptEvent& ev, std::int32_t result, std::uint32_t flags) {
    ZoneScoped;

    if (result < 0) {
        if (result == -ECANCELED)
            obs::LogWarn(kLogCategory, "Listener: accept multishot cancelled");
        else
            obs::LogError(kLogCategory, "Listener: accept error {}", result);
        // Multishot may have been cancelled, re-register
        if (listen_fd_.Valid()) {
            if (ring_.PrepAcceptMultishot(accept_ev_, listen_fd_.Get()))
                ring_.Submit();
            else
                obs::LogError(kLogCategory, "Listener: re-register accept failed (SQE full)");
        }
        return;
    }

    OnAccept(result);

    // If CQE_F_MORE not set, multishot accept ended -> re-register
    if (!(flags & IORING_CQE_F_MORE) && listen_fd_.Valid()) {
        if (ring_.PrepAcceptMultishot(accept_ev_, listen_fd_.Get()))
            ring_.Submit();
        else
            obs::LogError(kLogCategory, "Listener: re-register accept failed (SQE full)");
    }
}

void Listener::OnAccept(int client_fd) {
    // Backpressure: reject new sessions when at capacity
    if (max_sessions_ > 0 && session_count_fn_) {
        if (session_count_fn_() >= max_sessions_) {
            obs::LogWarn(kLogCategory, "Listener: [REJECT:MAX_SESS] max={} reached, rejecting fd={}", max_sessions_, client_fd);
            ::close(client_fd);
            return;
        }
    }

    // Set TCP_NODELAY on accepted socket
    int opt = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    if (!SetNonBlocking(client_fd)) {
        obs::LogWarn(kLogCategory, "Listener: failed to set accepted fd={} nonblocking", client_fd);
        ::close(client_fd);
        return;
    }

    auto sess = factory_(client_fd, ring_, pool_, shard_id_);
    if (sess) {
        sess->Start();
    } else {
        ::close(client_fd);
    }
}

} // namespace iouring_runtime::core::io
