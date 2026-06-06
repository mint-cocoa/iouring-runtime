#include "BenchCommon.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
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

struct Client {
    std::uint64_t player_id = 0;
    std::uint32_t room_id = 0;
    std::vector<char> in;
    std::vector<char> out;
};

void UpdateFd(int epoll_fd, int fd, const Client& client) {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | (client.out.empty() ? 0U : EPOLLOUT);
    ev.data.fd = fd;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

void CloseClient(int epoll_fd, std::unordered_map<int, Client>& clients,
                 std::unordered_map<std::uint32_t, std::vector<int>>& rooms,
                 int fd) {
    auto it = clients.find(fd);
    if (it != clients.end() && it->second.room_id != 0) {
        auto& room = rooms[it->second.room_id];
        room.erase(std::remove(room.begin(), room.end(), fd), room.end());
    }
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    clients.erase(fd);
    ::close(fd);
}

void HandlePacket(int epoll_fd, std::unordered_map<int, Client>& clients,
                  std::unordered_map<std::uint32_t, std::vector<int>>& rooms,
                  int fd, std::uint16_t id, std::span<const char> payload) {
    auto it = clients.find(fd);
    if (it == clients.end()) {
        return;
    }
    if (id == bench::kMsgJoin) {
        auto join = bench::ParseJoin(payload);
        if (!join) {
            CloseClient(epoll_fd, clients, rooms, fd);
            return;
        }
        it->second.player_id = join->first;
        it->second.room_id = join->second;
        rooms[it->second.room_id].push_back(fd);
        return;
    }
    if (id != bench::kMsgMove || it->second.room_id == 0) {
        return;
    }

    const auto packet = bench::MakePacket(id, payload);
    auto& room = rooms[it->second.room_id];
    room.erase(std::remove_if(room.begin(), room.end(),
                              [&](int target_fd) {
                                  return !clients.contains(target_fd);
                              }),
               room.end());
    for (int target_fd : room) {
        auto target = clients.find(target_fd);
        if (target == clients.end()) {
            continue;
        }
        target->second.out.insert(target->second.out.end(), packet.begin(),
                                  packet.end());
        UpdateFd(epoll_fd, target_fd, target->second);
    }
}

} // namespace

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    const auto host = bench::ReadStringEnv("EPOLL_FANOUT_HOST", "0.0.0.0");
    const auto port = bench::ReadPortEnv("EPOLL_FANOUT_PORT", 19121);
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
    std::unordered_map<std::uint32_t, std::vector<int>> rooms;
    std::array<epoll_event, 256> events{};
    std::array<char, 8192> buffer{};

    std::cout << "epoll_game_fanout_server listening on " << host << ":"
              << port << "\n";

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
            bool close_client =
                (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
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
                    it->second.in.insert(it->second.in.end(), buffer.data(),
                                         buffer.data() + r);
                    while (it->second.in.size() >= 4) {
                        const auto packet_size =
                            bench::ReadLe16(it->second.in.data());
                        if (packet_size < 4) {
                            close_client = true;
                            break;
                        }
                        if (it->second.in.size() < packet_size) {
                            break;
                        }
                        const auto id = bench::ReadLe16(it->second.in.data() + 2);
                        std::vector<char> payload(
                            it->second.in.begin() + 4,
                            it->second.in.begin() + packet_size);
                        it->second.in.erase(
                            it->second.in.begin(),
                            it->second.in.begin() +
                                static_cast<std::ptrdiff_t>(packet_size));
                        HandlePacket(epoll_fd, clients, rooms, fd, id, payload);
                        it = clients.find(fd);
                        if (it == clients.end()) {
                            close_client = true;
                            break;
                        }
                    }
                    if (close_client) {
                        break;
                    }
                }
            }

            if (!close_client && (events[i].events & EPOLLOUT) != 0) {
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
                CloseClient(epoll_fd, clients, rooms, fd);
                continue;
            }
            UpdateFd(epoll_fd, fd, it->second);
        }
    }

    for (auto& [fd, _] : clients) {
        ::close(fd);
    }
    ::close(epoll_fd);
    ::close(listen_fd);
    return 0;
}
