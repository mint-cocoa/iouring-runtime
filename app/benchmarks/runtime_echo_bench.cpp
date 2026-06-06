#include "BenchCommon.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 19090;
    int clients = 10;
    int payload = 64;
    int pipeline = 1;
    int duration_sec = 10;
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
        } else if (key == "--clients") {
            opts.clients = std::stoi(value);
        } else if (key == "--payload") {
            opts.payload = std::stoi(value);
        } else if (key == "--pipeline") {
            opts.pipeline = std::max(1, std::stoi(value));
        } else if (key == "--duration") {
            opts.duration_sec = std::stoi(value);
        }
    }
    return opts;
}

} // namespace

int main(int argc, char** argv) {
    const auto opts = ParseArgs(argc, argv);
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> errors{0};
    std::mutex latency_mutex;
    std::vector<double> latencies_us;
    latencies_us.reserve(static_cast<std::size_t>(opts.clients) * 1000);

    std::vector<std::thread> threads;
    threads.reserve(opts.clients);

    const auto started_at = std::chrono::steady_clock::now();
    for (int i = 0; i < opts.clients; ++i) {
        threads.emplace_back([&, i] {
            int fd = bench::ConnectBlocking(opts.host, opts.port);
            if (fd < 0) {
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            std::vector<char> payload(static_cast<std::size_t>(opts.payload));
            std::vector<char> echo(static_cast<std::size_t>(opts.payload));
            for (int j = 0; j < opts.payload; ++j) {
                payload[static_cast<std::size_t>(j)] =
                    static_cast<char>((i + j) & 0xff);
            }

            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (!stop.load(std::memory_order_relaxed)) {
                const auto t0 = std::chrono::steady_clock::now();
                if (!bench::SendAll(fd, payload.data(), payload.size()) ||
                    !bench::RecvAll(fd, echo.data(), echo.size())) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                const auto t1 = std::chrono::steady_clock::now();
                completed.fetch_add(1, std::memory_order_relaxed);
                const auto us =
                    std::chrono::duration<double, std::micro>(t1 - t0).count();
                std::lock_guard lock(latency_mutex);
                latencies_us.push_back(us);
            }
            ::close(fd);
        });
    }

    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(opts.duration_sec));
    stop.store(true, std::memory_order_relaxed);
    for (auto& thread : threads) {
        thread.join();
    }
    const auto ended_at = std::chrono::steady_clock::now();

    std::vector<double> latencies;
    {
        std::lock_guard lock(latency_mutex);
        latencies = latencies_us;
    }
    const double elapsed =
        std::chrono::duration<double>(ended_at - started_at).count();
    const auto done = completed.load(std::memory_order_relaxed);
    const double throughput = elapsed > 0.0 ? done / elapsed : 0.0;

    std::cout << "{\n"
              << "  \"type\": \"echo\",\n"
              << "  \"host\": \"" << opts.host << "\",\n"
              << "  \"port\": " << opts.port << ",\n"
              << "  \"clients\": " << opts.clients << ",\n"
              << "  \"payload_bytes\": " << opts.payload << ",\n"
              << "  \"pipeline\": " << opts.pipeline << ",\n"
              << "  \"duration_sec\": " << opts.duration_sec << ",\n"
              << "  \"completed\": " << done << ",\n"
              << "  \"errors\": " << errors.load(std::memory_order_relaxed)
              << ",\n"
              << "  \"throughput_per_sec\": " << throughput << ",\n"
              << "  \"p50_us\": " << bench::Percentile(latencies, 50) << ",\n"
              << "  \"p90_us\": " << bench::Percentile(latencies, 90) << ",\n"
              << "  \"p99_us\": " << bench::Percentile(latencies, 99) << "\n"
              << "}\n";

    return errors.load(std::memory_order_relaxed) == 0 ? 0 : 2;
}
