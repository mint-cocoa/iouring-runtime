#include "BenchCommon.h"

#include <iouring_runtime/core/SendBuffer.h>
#include <iouring_runtime/core/Session.h>
#include <iouring_runtime/core/Worker.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

class FanoutSession;

struct Hub {
    std::mutex mutex;
    std::unordered_map<std::uint32_t,
                       std::vector<std::weak_ptr<FanoutSession>>>
        rooms;
};

class FanoutSession final
    : public iouring_runtime::core::io::Session {
public:
    FanoutSession(int fd, iouring_runtime::core::ring::IoRing& ring,
                  iouring_runtime::core::buffer::BufferPool& pool,
                  std::shared_ptr<Hub> hub)
        : Session(fd, ring, pool), hub_(std::move(hub)) {}

    void SendPacket(std::uint16_t id, std::span<const char> payload) {
        const auto total = static_cast<std::uint32_t>(4 + payload.size());
        auto result = Pool().Allocate(total);
        if (!result) {
            Disconnect();
            return;
        }
        auto buffer = std::move(*result);
        auto writable = buffer->Writable();
        bench::WriteLe16(reinterpret_cast<char*>(writable.data()),
                         static_cast<std::uint16_t>(total));
        bench::WriteLe16(reinterpret_cast<char*>(writable.data()) + 2, id);
        std::memcpy(writable.data() + 4, payload.data(), payload.size());
        buffer->Commit(total);
        if (!Send(std::move(buffer)).has_value()) {
            Disconnect();
        }
    }

protected:
    void OnRecv(std::span<const std::byte> data) override {
        recv_.insert(recv_.end(), reinterpret_cast<const char*>(data.data()),
                     reinterpret_cast<const char*>(data.data()) + data.size());
        while (recv_.size() >= 4) {
            const auto packet_size = bench::ReadLe16(recv_.data());
            if (packet_size < 4) {
                Disconnect();
                return;
            }
            if (recv_.size() < packet_size) {
                return;
            }
            const auto id = bench::ReadLe16(recv_.data() + 2);
            const auto payload =
                std::span<const char>(recv_.data() + 4, packet_size - 4);
            HandlePacket(id, payload);
            recv_.erase(recv_.begin(),
                        recv_.begin() + static_cast<std::ptrdiff_t>(packet_size));
        }
    }

    void OnDisconnected() override {
        if (room_id_ == 0) {
            return;
        }
        std::lock_guard lock(hub_->mutex);
        auto& room = hub_->rooms[room_id_];
        room.erase(std::remove_if(room.begin(), room.end(),
                                  [this](const auto& weak) {
                                      auto locked = weak.lock();
                                      return !locked || locked.get() == this;
                                  }),
                   room.end());
    }

private:
    void HandlePacket(std::uint16_t id, std::span<const char> payload) {
        if (id == bench::kMsgJoin) {
            auto join = bench::ParseJoin(payload);
            if (!join) {
                Disconnect();
                return;
            }
            player_id_ = join->first;
            room_id_ = join->second;
            std::lock_guard lock(hub_->mutex);
            hub_->rooms[room_id_].push_back(
                std::static_pointer_cast<FanoutSession>(shared_from_this()));
            return;
        }
        if (id != bench::kMsgMove) {
            return;
        }

        const auto packet = bench::MakePacket(id, payload);
        std::vector<std::shared_ptr<FanoutSession>> targets;
        {
            std::lock_guard lock(hub_->mutex);
            auto& room = hub_->rooms[room_id_];
            room.erase(std::remove_if(room.begin(), room.end(),
                                      [](const auto& weak) {
                                          return weak.expired();
                                      }),
                       room.end());
            targets.reserve(room.size());
            for (auto& weak : room) {
                if (auto target = weak.lock()) {
                    targets.push_back(std::move(target));
                }
            }
        }

        const auto out_payload =
            std::span<const char>(packet.data() + 4, packet.size() - 4);
        for (auto& target : targets) {
            target->SendPacket(id, out_payload);
        }
    }

    std::shared_ptr<Hub> hub_;
    std::vector<char> recv_;
    std::uint64_t player_id_ = 0;
    std::uint32_t room_id_ = 0;
};

} // namespace

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    const auto port = bench::ReadPortEnv("IOURING_FANOUT_PORT", 19120);
    auto hub = std::make_shared<Hub>();

    iouring_runtime::core::io::SessionFactory factory =
        [hub](int fd, iouring_runtime::core::ring::IoRing& ring,
              iouring_runtime::core::buffer::BufferPool& pool,
              iouring_runtime::core::ContextId)
        -> iouring_runtime::core::io::SessionRef {
        return std::make_shared<FanoutSession>(fd, ring, pool, hub);
    };

    iouring_runtime::core::io::WorkerConfig config;
    config.address = iouring_runtime::core::Address{"0.0.0.0", port};
    config.ring.queue_depth = 4096;
    config.ring.buf_ring.buf_count = 8192;
    config.ring.buf_ring.buf_size = 4096;
    config.io_timeout = std::chrono::milliseconds{1};

    iouring_runtime::core::io::Worker worker(config, std::move(factory));
    if (!worker.Start()) {
        std::cerr << "failed to listen on 0.0.0.0:" << port << "\n";
        return 1;
    }

    std::cout << "iouring_game_fanout_server listening on 0.0.0.0:" << port
              << "\n";
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    worker.Stop();
    return 0;
}
