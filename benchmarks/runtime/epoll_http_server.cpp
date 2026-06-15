#include "BenchCommon.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int) {
    g_stop = 1;
}

struct Client {
    std::string in;
    std::string out;
};

std::string Payload(std::size_t size) {
    std::string body;
    body.resize(size);
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<char>('a' + (i % 26));
    }
    return body;
}

std::string Response(std::string_view request) {
    const bool head = request.starts_with("HEAD ");
    std::string body = "hello from iouring_web";
    if (request.starts_with("GET /health ") ||
        request.starts_with("HEAD /health ")) {
        body = "ok";
    } else if (request.starts_with("GET /payload/256b ")) {
        body = Payload(256);
    } else if (request.starts_with("GET /payload/4k ")) {
        body = Payload(4096);
    } else if (request.starts_with("GET /payload/64k ")) {
        body = Payload(65536);
    }

    std::string out = "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\n"
                      "Content-Type: text/plain\r\nContent-Length: ";
    out += std::to_string(head ? 0 : body.size());
    out += "\r\n\r\n";
    if (!head) {
        out += body;
    }
    return out;
}

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

    const auto host = bench::ReadStringEnv("EPOLL_HTTP_HOST", "0.0.0.0");
    const auto port = bench::ReadPortEnv("EPOLL_HTTP_PORT", 18081);

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

    std::unordered_map<int, Client> clients;
    std::array<epoll_event, 256> events{};
    std::array<char, 8192> buffer{};

    std::cout << "epoll_http_server listening on " << host << ":" << port
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
            bool close_client = (events[i].events &
                                 (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
            if (!close_client && (events[i].events & EPOLLIN) != 0) {
                while (true) {
                    const auto r = ::recv(fd, buffer.data(), buffer.size(), 0);
                    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    if (r <= 0) {
                        close_client = true;
                        break;
                    }
                    it->second.in.append(buffer.data(), r);
                    std::size_t request_end = std::string::npos;
                    while ((request_end = it->second.in.find("\r\n\r\n")) !=
                           std::string::npos) {
                        const auto request =
                            it->second.in.substr(0, request_end + 4);
                        it->second.in.erase(0, request_end + 4);
                        it->second.out += Response(request);
                    }
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
                    it->second.out.erase(0, static_cast<std::size_t>(s));
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
