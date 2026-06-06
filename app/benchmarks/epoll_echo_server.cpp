#include "BenchCommon.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int) {
    g_stop = 1;
}

struct Client {
    std::vector<char> out;
};

void CloseClient(int epoll_fd, std::unordered_map<int, Client>& clients,
                 int fd) {
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    clients.erase(fd);
    ::close(fd);
}

} // namespace

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    const auto host = bench::ReadStringEnv("EPOLL_ECHO_HOST", "0.0.0.0");
    const auto port = bench::ReadPortEnv("EPOLL_ECHO_PORT", 19091);

    const int listen_fd = bench::CreateListenSocket(host, port);
    if (listen_fd < 0) {
        std::cerr << "failed to listen on " << host << ":" << port << "\n";
        return 1;
    }

    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        std::cerr << "epoll_create1 failed\n";
        return 1;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = listen_fd;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event);

    std::unordered_map<int, Client> clients;
    std::array<epoll_event, 256> events{};
    std::array<char, 8192> buffer{};

    std::cout << "epoll_echo_server listening on " << host << ":" << port
              << "\n";

    while (!g_stop) {
        const int n = ::epoll_wait(epoll_fd, events.data(), events.size(), 100);
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == listen_fd) {
                while (true) {
                    int client = ::accept4(listen_fd, nullptr, nullptr,
                                           SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client < 0) {
                        break;
                    }
                    clients.emplace(client, Client{});
                    epoll_event ev{};
                    ev.events = EPOLLIN | EPOLLRDHUP;
                    ev.data.fd = client;
                    ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &ev);
                }
                continue;
            }

            auto it = clients.find(fd);
            if (it == clients.end()) {
                continue;
            }
            if ((events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                CloseClient(epoll_fd, clients, fd);
                continue;
            }

            bool close_client = false;
            if ((events[i].events & EPOLLIN) != 0) {
                while (true) {
                    const auto r = ::recv(fd, buffer.data(), buffer.size(), 0);
                    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    if (r <= 0) {
                        close_client = true;
                        break;
                    }
                    it->second.out.insert(it->second.out.end(), buffer.data(),
                                          buffer.data() + r);
                }
            }

            if (!close_client && !it->second.out.empty()) {
                while (!it->second.out.empty()) {
                    const auto s = ::send(fd, it->second.out.data(),
                                          it->second.out.size(), MSG_NOSIGNAL);
                    if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    if (s <= 0) {
                        close_client = true;
                        break;
                    }
                    it->second.out.erase(
                        it->second.out.begin(),
                        it->second.out.begin() + static_cast<std::ptrdiff_t>(s));
                }
            }

            if (close_client) {
                CloseClient(epoll_fd, clients, fd);
                continue;
            }

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLRDHUP |
                        (it->second.out.empty() ? 0U : EPOLLOUT);
            ev.data.fd = fd;
            ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
        }
    }

    for (auto& [fd, _] : clients) {
        ::close(fd);
    }
    ::close(epoll_fd);
    ::close(listen_fd);
    return 0;
}
