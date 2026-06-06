#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bench {

constexpr std::uint16_t kMsgJoin = 1;
constexpr std::uint16_t kMsgMove = 2;

struct MovePayload {
    std::uint64_t player_id = 0;
    std::uint32_t room_id = 0;
    float rotation_y = 0.0F;
};

inline std::uint16_t ReadLe16(const char* data) {
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(data[0]) |
        (static_cast<unsigned char>(data[1]) << 8));
}

inline void WriteLe16(char* out, std::uint16_t value) {
    out[0] = static_cast<char>(value & 0xff);
    out[1] = static_cast<char>((value >> 8) & 0xff);
}

inline void WriteLe32(char* out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<char>((value >> (i * 8)) & 0xff);
    }
}

inline void WriteLe64(char* out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<char>((value >> (i * 8)) & 0xff);
    }
}

inline std::uint32_t ReadLe32(const char* data) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(data[i]))
                 << (i * 8);
    }
    return value;
}

inline std::uint64_t ReadLe64(const char* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(data[i]))
                 << (i * 8);
    }
    return value;
}

inline std::vector<char> MakePacket(std::uint16_t id,
                                    std::span<const char> payload) {
    const auto total = static_cast<std::uint16_t>(4 + payload.size());
    std::vector<char> packet(total);
    WriteLe16(packet.data(), total);
    WriteLe16(packet.data() + 2, id);
    std::memcpy(packet.data() + 4, payload.data(), payload.size());
    return packet;
}

inline std::vector<char> MakeJoinPacket(std::uint64_t player_id,
                                        std::uint32_t room_id) {
    char payload[12];
    WriteLe64(payload, player_id);
    WriteLe32(payload + 8, room_id);
    return MakePacket(kMsgJoin, payload);
}

inline std::vector<char> MakeMovePacket(MovePayload move) {
    char payload[16];
    WriteLe64(payload, move.player_id);
    WriteLe32(payload + 8, move.room_id);
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t rotation_bits = 0;
    std::memcpy(&rotation_bits, &move.rotation_y, sizeof(rotation_bits));
    WriteLe32(payload + 12, rotation_bits);
    return MakePacket(kMsgMove, payload);
}

inline std::optional<MovePayload> ParseMove(std::span<const char> payload) {
    if (payload.size() < 16) {
        return std::nullopt;
    }
    MovePayload move;
    move.player_id = ReadLe64(payload.data());
    move.room_id = ReadLe32(payload.data() + 8);
    const auto rotation_bits = ReadLe32(payload.data() + 12);
    std::memcpy(&move.rotation_y, &rotation_bits, sizeof(move.rotation_y));
    return move;
}

inline std::optional<std::pair<std::uint64_t, std::uint32_t>> ParseJoin(
    std::span<const char> payload) {
    if (payload.size() < 12) {
        return std::nullopt;
    }
    return std::pair<std::uint64_t, std::uint32_t>{
        ReadLe64(payload.data()), ReadLe32(payload.data() + 8)};
}

inline bool SetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline int CreateListenSocket(std::string_view host, std::uint16_t port,
                              int backlog = 1024) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, std::string(host).c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, backlog) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

inline int ConnectBlocking(std::string_view host, std::uint16_t port,
                           std::chrono::seconds timeout =
                               std::chrono::seconds{3}) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count());
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, std::string(host).c_str(), &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

inline bool SendAll(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const auto n = ::send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

inline bool RecvAll(int fd, char* data, std::size_t size) {
    while (size > 0) {
        const auto n = ::recv(fd, data, size, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

inline std::uint16_t ReadPortEnv(const char* name, std::uint16_t fallback) {
    if (const char* raw = std::getenv(name)) {
        return static_cast<std::uint16_t>(std::stoi(raw));
    }
    return fallback;
}

inline int ReadIntEnv(const char* name, int fallback) {
    if (const char* raw = std::getenv(name)) {
        return std::stoi(raw);
    }
    return fallback;
}

inline std::string ReadStringEnv(const char* name, std::string fallback) {
    if (const char* raw = std::getenv(name)) {
        return raw;
    }
    return fallback;
}

inline double Percentile(std::vector<double> values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double idx = (percentile / 100.0) * (values.size() - 1);
    const auto lo = static_cast<std::size_t>(idx);
    const auto hi = std::min(lo + 1, values.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

inline void SleepUntilReady(std::string_view host, std::uint16_t port) {
    for (int i = 0; i < 100; ++i) {
        int fd = ConnectBlocking(host, port, std::chrono::seconds{1});
        if (fd >= 0) {
            ::close(fd);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

} // namespace bench
