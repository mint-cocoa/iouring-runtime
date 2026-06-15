#include "BenchCommon.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 19120;
    int bots = 40;
    int rooms = 1;
    int move_hz = 20;
    int attack_hz = 0;
    int duration_sec = 10;
};

struct BotStats {
    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> errors{0};
};

struct BotState {
    std::uint64_t player_id = 0;
    std::uint32_t room_id = 0;
    int fd = -1;
    std::mutex mutex;
    std::unordered_map<std::uint32_t, Clock::time_point> sent_at;
    BotStats stats;
};

Options ParseArgs(int argc, char** argv) {
    Options opts;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--host") {
            opts.host = value;
        } else if (key == "--port") {
            opts.port = static_cast<std::uint16_t>(std::stoi(value));
        } else if (key == "--bots") {
            opts.bots = std::stoi(value);
        } else if (key == "--rooms") {
            opts.rooms = std::max(1, std::stoi(value));
        } else if (key == "--move-hz") {
            opts.move_hz = std::max(1, std::stoi(value));
        } else if (key == "--attack-hz") {
            opts.attack_hz = std::max(0, std::stoi(value));
        } else if (key == "--duration") {
            opts.duration_sec = std::stoi(value);
        }
    }
    return opts;
}

bool RecvPacket(int fd, std::uint16_t& id, std::vector<char>& payload) {
    char header[4];
    if (!bench::RecvAll(fd, header, sizeof(header))) {
        return false;
    }
    const auto size = bench::ReadLe16(header);
    if (size < 4) {
        return false;
    }
    id = bench::ReadLe16(header + 2);
    payload.resize(size - 4);
    return payload.empty() || bench::RecvAll(fd, payload.data(), payload.size());
}

} // namespace

int main(int argc, char** argv) {
    const auto opts = ParseArgs(argc, argv);
    std::atomic<bool> start{false};
    std::atomic<bool> stop_sending{false};
    std::atomic<bool> stop_receiving{false};
    std::mutex latency_mutex;
    std::vector<double> latencies_us;
    latencies_us.reserve(static_cast<std::size_t>(opts.bots) * opts.move_hz *
                         opts.duration_sec);

    std::vector<std::unique_ptr<BotState>> bots;
    bots.reserve(opts.bots);
    for (int i = 0; i < opts.bots; ++i) {
        auto bot = std::make_unique<BotState>();
        bot->player_id = static_cast<std::uint64_t>(i + 1);
        bot->room_id = static_cast<std::uint32_t>((i % opts.rooms) + 1);
        bot->fd = bench::ConnectBlocking(opts.host, opts.port);
        if (bot->fd < 0) {
            std::cerr << "connect failed for bot " << i << "\n";
            return 2;
        }
        auto join = bench::MakeJoinPacket(bot->player_id, bot->room_id);
        if (!bench::SendAll(bot->fd, join.data(), join.size())) {
            std::cerr << "join failed for bot " << i << "\n";
            return 2;
        }
        bots.push_back(std::move(bot));
    }

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(opts.bots) * 2);
    for (auto& bot_ptr : bots) {
        auto* bot = bot_ptr.get();
        threads.emplace_back([&, bot] {
            std::uint32_t seq = 1;
            const auto interval =
                std::chrono::microseconds(1'000'000 / opts.move_hz);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto next = Clock::now();
            while (!stop_sending.load(std::memory_order_relaxed)) {
                bench::MovePayload move{
                    .player_id = bot->player_id,
                    .room_id = bot->room_id,
                    .rotation_y = static_cast<float>(seq),
                };
                auto packet = bench::MakeMovePacket(move);
                {
                    std::lock_guard lock(bot->mutex);
                    bot->sent_at[seq] = Clock::now();
                }
                if (!bench::SendAll(bot->fd, packet.data(), packet.size())) {
                    bot->stats.errors.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                bot->stats.sent.fetch_add(1, std::memory_order_relaxed);
                ++seq;
                next += interval;
                std::this_thread::sleep_until(next);
            }
        });

        threads.emplace_back([&, bot] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!stop_receiving.load(std::memory_order_relaxed)) {
                std::uint16_t id = 0;
                std::vector<char> payload;
                if (!RecvPacket(bot->fd, id, payload)) {
                    if (!stop_receiving.load(std::memory_order_relaxed)) {
                        bot->stats.errors.fetch_add(1,
                                                    std::memory_order_relaxed);
                    }
                    break;
                }
                if (id != bench::kMsgMove) {
                    continue;
                }
                auto move = bench::ParseMove(payload);
                if (!move) {
                    bot->stats.errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                bot->stats.received.fetch_add(1, std::memory_order_relaxed);
                if (move->player_id == bot->player_id) {
                    const auto seq =
                        static_cast<std::uint32_t>(std::llround(move->rotation_y));
                    std::optional<Clock::time_point> sent_at;
                    {
                        std::lock_guard lock(bot->mutex);
                        auto it = bot->sent_at.find(seq);
                        if (it != bot->sent_at.end()) {
                            sent_at = it->second;
                            bot->sent_at.erase(it);
                        }
                    }
                    if (sent_at) {
                        const auto us =
                            std::chrono::duration<double, std::micro>(
                                Clock::now() - *sent_at)
                                .count();
                        std::lock_guard lock(latency_mutex);
                        latencies_us.push_back(us);
                    }
                }
            }
        });
    }

    const auto started_at = Clock::now();
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(opts.duration_sec));
    stop_sending.store(true, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    stop_receiving.store(true, std::memory_order_relaxed);
    for (auto& bot : bots) {
        ::shutdown(bot->fd, SHUT_RDWR);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto ended_at = Clock::now();

    std::vector<std::uint64_t> sent_by_room(static_cast<std::size_t>(opts.rooms) + 1);
    std::vector<std::uint64_t> members_by_room(static_cast<std::size_t>(opts.rooms) + 1);
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    std::uint64_t errors = 0;
    for (const auto& bot : bots) {
        sent += bot->stats.sent.load(std::memory_order_relaxed);
        received += bot->stats.received.load(std::memory_order_relaxed);
        errors += bot->stats.errors.load(std::memory_order_relaxed);
        sent_by_room[bot->room_id] +=
            bot->stats.sent.load(std::memory_order_relaxed);
        members_by_room[bot->room_id] += 1;
        ::close(bot->fd);
    }

    std::uint64_t expected = 0;
    for (std::size_t room = 1; room < sent_by_room.size(); ++room) {
        expected += sent_by_room[room] * members_by_room[room];
    }
    const auto missing = expected > received ? expected - received : 0;
    const double missing_ratio =
        expected == 0 ? 0.0 : static_cast<double>(missing) / expected;
    const double elapsed = std::chrono::duration<double>(ended_at - started_at)
                               .count();
    const double msg_per_sec = elapsed > 0.0 ? received / elapsed : 0.0;

    std::vector<double> latencies;
    {
        std::lock_guard lock(latency_mutex);
        latencies = latencies_us;
    }

    std::cout << "{\n"
              << "  \"type\": \"game_fanout\",\n"
              << "  \"host\": \"" << opts.host << "\",\n"
              << "  \"port\": " << opts.port << ",\n"
              << "  \"bots\": " << opts.bots << ",\n"
              << "  \"rooms\": " << opts.rooms << ",\n"
              << "  \"move_hz\": " << opts.move_hz << ",\n"
              << "  \"attack_hz\": " << opts.attack_hz << ",\n"
              << "  \"duration_sec\": " << opts.duration_sec << ",\n"
              << "  \"sent_moves\": " << sent << ",\n"
              << "  \"received_messages\": " << received << ",\n"
              << "  \"expected_messages\": " << expected << ",\n"
              << "  \"missing_messages\": " << missing << ",\n"
              << "  \"missing_ratio\": " << missing_ratio << ",\n"
              << "  \"errors\": " << errors << ",\n"
              << "  \"messages_per_sec\": " << msg_per_sec << ",\n"
              << "  \"p50_us\": " << bench::Percentile(latencies, 50) << ",\n"
              << "  \"p90_us\": " << bench::Percentile(latencies, 90) << ",\n"
              << "  \"p99_us\": " << bench::Percentile(latencies, 99) << "\n"
              << "}\n";

    return errors == 0 && missing_ratio <= 0.001 ? 0 : 2;
}
