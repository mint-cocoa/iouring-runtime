#include "BenchCommon.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int) {
    g_stop = 1;
}

struct Client {
    int fd = -1;
    int epoll_fd = -1;
    std::uint64_t player_id = 0;
    std::uint32_t room_id = 0;
    std::vector<char> in;
    std::vector<char> out;
    std::mutex mutex;
    bool closed = false;
};

struct Hub {
    std::mutex mutex;
    std::unordered_map<std::uint32_t, std::vector<std::weak_ptr<Client>>> rooms;
};

void UpdateFdLocked(const Client& client) {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | (client.out.empty() ? 0U : EPOLLOUT);
    ev.data.fd = client.fd;
    ::epoll_ctl(client.epoll_fd, EPOLL_CTL_MOD, client.fd, &ev);
}

void CloseClient(int epoll_fd,
                 std::unordered_map<int, std::shared_ptr<Client>>& clients,
                 const std::shared_ptr<Hub>& hub,
                 int fd) {
    auto it = clients.find(fd);
    if (it == clients.end()) {
        return;
    }

    std::uint32_t room_id = 0;
    {
        std::lock_guard lock(it->second->mutex);
        it->second->closed = true;
        room_id = it->second->room_id;
    }

    if (room_id != 0) {
        std::lock_guard lock(hub->mutex);
        auto& room = hub->rooms[room_id];
        room.erase(std::remove_if(room.begin(), room.end(),
                                  [fd](const auto& weak) {
                                      auto client = weak.lock();
                                      return !client || client->fd == fd;
                                  }),
                   room.end());
    }

    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    clients.erase(it);
    ::close(fd);
}

void HandlePacket(const std::shared_ptr<Client>& client,
                  const std::shared_ptr<Hub>& hub,
                  std::uint16_t id,
                  std::span<const char> payload) {
    if (id == bench::kMsgJoin) {
        auto join = bench::ParseJoin(payload);
        if (!join) {
            std::lock_guard lock(client->mutex);
            client->closed = true;
            return;
        }
        {
            std::lock_guard lock(client->mutex);
            client->player_id = join->first;
            client->room_id = join->second;
        }
        std::lock_guard lock(hub->mutex);
        hub->rooms[join->second].push_back(client);
        return;
    }

    std::uint32_t room_id = 0;
    {
        std::lock_guard lock(client->mutex);
        room_id = client->room_id;
    }
    if (id != bench::kMsgMove || room_id == 0) {
        return;
    }

    const auto packet = bench::MakePacket(id, payload);
    std::vector<std::shared_ptr<Client>> targets;
    {
        std::lock_guard lock(hub->mutex);
        auto& room = hub->rooms[room_id];
        room.erase(std::remove_if(room.begin(), room.end(),
                                  [](const auto& weak) {
                                      auto client = weak.lock();
                                      if (!client) {
                                          return true;
                                      }
                                      std::lock_guard client_lock(client->mutex);
                                      return client->closed;
                                  }),
                   room.end());
        targets.reserve(room.size());
        for (auto& weak : room) {
            if (auto target = weak.lock()) {
                targets.push_back(std::move(target));
            }
        }
    }

    for (auto& target : targets) {
        std::lock_guard lock(target->mutex);
        if (target->closed) {
            continue;
        }
        target->out.insert(target->out.end(), packet.begin(), packet.end());
        UpdateFdLocked(*target);
    }
}

bool RunWorker(std::string host, std::uint16_t port,
               std::shared_ptr<Hub> hub) {
    const int listen_fd = bench::CreateListenSocket(host, port);
    if (listen_fd < 0) {
        std::cerr << "failed to listen on " << host << ":" << port << "\n";
        return false;
    }

    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        std::cerr << "epoll_create1 failed\n";
        ::close(listen_fd);
        return false;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = listen_fd;
    ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event);

    std::unordered_map<int, std::shared_ptr<Client>> clients;
    std::array<epoll_event, 256> events{};
    std::array<char, 8192> buffer{};

    while (!g_stop) {
        const int n = ::epoll_wait(epoll_fd, events.data(), events.size(), 100);
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == listen_fd) {
                while (true) {
                    int accepted = ::accept4(listen_fd, nullptr, nullptr,
                                             SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (accepted < 0) {
                        break;
                    }
                    auto client = std::make_shared<Client>();
                    client->fd = accepted;
                    client->epoll_fd = epoll_fd;
                    clients.emplace(accepted, client);
                    epoll_event ev{};
                    ev.events = EPOLLIN | EPOLLRDHUP;
                    ev.data.fd = accepted;
                    ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, accepted, &ev);
                }
                continue;
            }

            auto it = clients.find(fd);
            if (it == clients.end()) {
                continue;
            }
            auto client = it->second;
            bool close_client =
                (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
            std::vector<std::pair<std::uint16_t, std::vector<char>>> packets;

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

                    std::lock_guard lock(client->mutex);
                    client->in.insert(client->in.end(), buffer.data(),
                                      buffer.data() + r);
                    while (client->in.size() >= 4) {
                        const auto packet_size =
                            bench::ReadLe16(client->in.data());
                        if (packet_size < 4) {
                            close_client = true;
                            break;
                        }
                        if (client->in.size() < packet_size) {
                            break;
                        }
                        const auto id = bench::ReadLe16(client->in.data() + 2);
                        std::vector<char> payload(
                            client->in.begin() + 4,
                            client->in.begin() + packet_size);
                        client->in.erase(
                            client->in.begin(),
                            client->in.begin() +
                                static_cast<std::ptrdiff_t>(packet_size));
                        packets.emplace_back(id, std::move(payload));
                    }
                    if (close_client) {
                        break;
                    }
                }
            }

            for (const auto& [id, payload] : packets) {
                HandlePacket(client, hub, id, payload);
                std::lock_guard lock(client->mutex);
                if (client->closed) {
                    close_client = true;
                    break;
                }
            }

            if (!close_client && (events[i].events & EPOLLOUT) != 0) {
                std::lock_guard lock(client->mutex);
                while (!client->out.empty()) {
                    const auto s = ::send(fd, client->out.data(),
                                          client->out.size(), MSG_NOSIGNAL);
                    if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    if (s <= 0) {
                        close_client = true;
                        break;
                    }
                    client->out.erase(
                        client->out.begin(),
                        client->out.begin() + static_cast<std::ptrdiff_t>(s));
                }
            }

            if (close_client) {
                CloseClient(epoll_fd, clients, hub, fd);
                continue;
            }
            {
                std::lock_guard lock(client->mutex);
                UpdateFdLocked(*client);
            }
        }
    }

    for (auto& [fd, _] : clients) {
        ::close(fd);
    }
    ::close(epoll_fd);
    ::close(listen_fd);
    return true;
}

} // namespace

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    const auto host = bench::ReadStringEnv("EPOLL_FANOUT_HOST", "0.0.0.0");
    const auto port = bench::ReadPortEnv("EPOLL_FANOUT_PORT", 19121);
    const auto worker_count =
        std::max(1, bench::ReadIntEnv("EPOLL_FANOUT_WORKERS", 1));
    auto hub = std::make_shared<Hub>();

    std::cout << "epoll_game_fanout_server listening on " << host << ":"
              << port << " workers=" << worker_count << "\n";

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(worker_count));
    for (int i = 0; i < worker_count; ++i) {
        workers.emplace_back([host, port, hub] {
            if (!RunWorker(host, port, hub)) {
                g_stop = 1;
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }
    return 0;
}
