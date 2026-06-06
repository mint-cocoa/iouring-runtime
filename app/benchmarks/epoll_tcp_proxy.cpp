#include "BenchCommon.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int) {
    g_stop = 1;
}

struct Peer {
    int fd = -1;
    int other = -1;
    std::vector<char> out;
};

void RegisterFd(int epoll_fd, int fd, std::uint32_t events = EPOLLIN) {
    epoll_event ev{};
    ev.events = events | EPOLLRDHUP;
    ev.data.fd = fd;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

void UpdateFd(int epoll_fd, const Peer& peer) {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | (peer.out.empty() ? 0U : EPOLLOUT);
    ev.data.fd = peer.fd;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, peer.fd, &ev);
}

void ClosePair(int epoll_fd, std::unordered_map<int, Peer>& peers, int fd) {
    auto it = peers.find(fd);
    if (it == peers.end()) {
        return;
    }
    const int other = it->second.other;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    peers.erase(fd);
    if (auto other_it = peers.find(other); other_it != peers.end()) {
        ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, other, nullptr);
        ::close(other);
        peers.erase(other_it);
    }
}

int ConnectUpstream(std::string_view host, std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, std::string(host).c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    const int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr));
    if (ret != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    const auto host = bench::ReadStringEnv("EPOLL_PROXY_HOST", "0.0.0.0");
    const auto upstream_host =
        bench::ReadStringEnv("EPOLL_PROXY_UPSTREAM_HOST", "127.0.0.1");
    const auto port = bench::ReadPortEnv("EPOLL_PROXY_PORT", 18082);
    const auto upstream_port =
        bench::ReadPortEnv("EPOLL_PROXY_UPSTREAM_PORT", 18080);

    const int listen_fd = bench::CreateListenSocket(host, port);
    if (listen_fd < 0) {
        std::cerr << "failed to listen on " << host << ":" << port << "\n";
        return 1;
    }
    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = listen_fd;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event);

    std::unordered_map<int, Peer> peers;
    std::array<epoll_event, 256> events{};
    std::array<char, 8192> buffer{};

    std::cout << "epoll_tcp_proxy listening on " << host << ":" << port
              << " -> " << upstream_host << ":" << upstream_port << "\n";

    while (!g_stop) {
        const int n = ::epoll_wait(epoll_fd, events.data(), events.size(), 100);
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == listen_fd) {
                while (true) {
                    int downstream = ::accept4(listen_fd, nullptr, nullptr,
                                               SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (downstream < 0) {
                        break;
                    }
                    int upstream = ConnectUpstream(upstream_host, upstream_port);
                    if (upstream < 0) {
                        ::close(downstream);
                        continue;
                    }
                    peers.emplace(downstream, Peer{.fd = downstream,
                                                   .other = upstream,
                                                   .out = {}});
                    peers.emplace(upstream, Peer{.fd = upstream,
                                                 .other = downstream,
                                                 .out = {}});
                    RegisterFd(epoll_fd, downstream);
                    RegisterFd(epoll_fd, upstream, EPOLLIN | EPOLLOUT);
                }
                continue;
            }

            auto it = peers.find(fd);
            if (it == peers.end()) {
                continue;
            }

            bool close_pair =
                (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
            if (!close_pair && (events[i].events & EPOLLIN) != 0) {
                while (true) {
                    const auto r = ::recv(fd, buffer.data(), buffer.size(), 0);
                    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    if (r <= 0) {
                        close_pair = true;
                        break;
                    }
                    if (auto other = peers.find(it->second.other);
                        other != peers.end()) {
                        other->second.out.insert(other->second.out.end(),
                                                 buffer.data(),
                                                 buffer.data() + r);
                        UpdateFd(epoll_fd, other->second);
                    } else {
                        close_pair = true;
                        break;
                    }
                }
            }

            if (!close_pair && (events[i].events & EPOLLOUT) != 0) {
                while (!it->second.out.empty()) {
                    const auto s = ::send(fd, it->second.out.data(),
                                          it->second.out.size(), MSG_NOSIGNAL);
                    if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    if (s <= 0) {
                        close_pair = true;
                        break;
                    }
                    it->second.out.erase(
                        it->second.out.begin(),
                        it->second.out.begin() + static_cast<std::ptrdiff_t>(s));
                }
            }

            if (close_pair) {
                ClosePair(epoll_fd, peers, fd);
                continue;
            }
            UpdateFd(epoll_fd, it->second);
        }
    }

    for (auto& [fd, _] : peers) {
        ::close(fd);
    }
    ::close(epoll_fd);
    ::close(listen_fd);
    return 0;
}
