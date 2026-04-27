#include "Auth.pb.h"
#include "Currency.pb.h"
#include "Game.pb.h"
#include "Inventory.pb.h"
#include "Social.pb.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t kCLogin = 101;
constexpr std::uint16_t kSLogin = 102;
constexpr std::uint16_t kCRoomList = 105;
constexpr std::uint16_t kSRoomList = 106;
constexpr std::uint16_t kCCreateRoom = 107;
constexpr std::uint16_t kSCreateRoom = 108;
constexpr std::uint16_t kCCharList = 113;
constexpr std::uint16_t kSCharList = 114;
constexpr std::uint16_t kCSelectChar = 117;
constexpr std::uint16_t kSSelectChar = 118;
constexpr std::uint16_t kCSceneReady = 119;
constexpr std::uint16_t kSSkillData = 209;
constexpr std::uint16_t kSPlayerList = 208;
constexpr std::uint16_t kCChat = 301;
constexpr std::uint16_t kSChat = 302;
constexpr std::uint16_t kCCreateParty = 303;
constexpr std::uint16_t kSCreateParty = 304;
constexpr std::uint16_t kSPartyUpdate = 309;
constexpr std::uint16_t kSInventoryInit = 501;
constexpr std::uint16_t kSCurrencyInit = 601;

struct Packet {
    std::uint16_t id = 0;
    std::vector<char> payload;
};

void WriteLe16(char* out, std::uint16_t value) {
    out[0] = static_cast<char>(value & 0xff);
    out[1] = static_cast<char>((value >> 8) & 0xff);
}

std::uint16_t ReadLe16(const char* in) {
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(in[0]) |
        (static_cast<unsigned char>(in[1]) << 8));
}

void SendAll(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        ssize_t n = ::send(fd, data, size, 0);
        if (n <= 0) throw std::runtime_error("send failed");
        data += n;
        size -= static_cast<std::size_t>(n);
    }
}

template <typename Message>
void SendPacket(int fd, std::uint16_t id, const Message& message) {
    const auto payload_size = static_cast<std::uint16_t>(message.ByteSizeLong());
    std::vector<char> packet(4 + payload_size);
    WriteLe16(packet.data(), static_cast<std::uint16_t>(packet.size()));
    WriteLe16(packet.data() + 2, id);
    if (payload_size > 0 &&
        !message.SerializeToArray(packet.data() + 4, payload_size)) {
        throw std::runtime_error("protobuf serialize failed");
    }
    SendAll(fd, packet.data(), packet.size());
}

void SendEmptyPacket(int fd, std::uint16_t id) {
    char packet[4];
    WriteLe16(packet, 4);
    WriteLe16(packet + 2, id);
    SendAll(fd, packet, sizeof(packet));
}

Packet RecvPacket(int fd) {
    char header[4];
    std::size_t got = 0;
    while (got < sizeof(header)) {
        ssize_t n = ::recv(fd, header + got, sizeof(header) - got, 0);
        if (n <= 0) throw std::runtime_error("recv header failed");
        got += static_cast<std::size_t>(n);
    }

    const auto size = ReadLe16(header);
    if (size < 4) throw std::runtime_error("invalid packet size");

    Packet packet;
    packet.id = ReadLe16(header + 2);
    packet.payload.resize(size - 4);
    got = 0;
    while (got < packet.payload.size()) {
        ssize_t n = ::recv(fd, packet.payload.data() + got,
                           packet.payload.size() - got, 0);
        if (n <= 0) throw std::runtime_error("recv payload failed");
        got += static_cast<std::size_t>(n);
    }
    return packet;
}

template <typename Message>
Message ParsePayload(const Packet& packet, std::uint16_t expected) {
    if (packet.id != expected) {
        throw std::runtime_error("unexpected packet id " +
                                 std::to_string(packet.id) + ", expected " +
                                 std::to_string(expected));
    }
    Message message;
    if (!message.ParseFromArray(packet.payload.data(),
                                static_cast<int>(packet.payload.size()))) {
        throw std::runtime_error("protobuf parse failed for " +
                                 std::to_string(expected));
    }
    return message;
}

int Connect(const char* host, std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket failed");

    timeval timeout{};
    timeout.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid host");
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("connect failed");
    }
    return fd;
}

void PrintOk(const std::string& text) {
    std::cout << "[ok] " << text << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const auto port = static_cast<std::uint16_t>(argc > 2 ? std::stoi(argv[2]) : 7777);

    try {
        int fd = Connect(host, port);

        game::C_Login login;
        login.set_username("probe_user");
        login.set_password("probe_password");
        SendPacket(fd, kCLogin, login);
        auto login_reply = ParsePayload<game::S_Login>(RecvPacket(fd), kSLogin);
        if (!login_reply.success()) throw std::runtime_error("login failed");
        PrintOk("login player_id=" + std::to_string(login_reply.player_id()));

        game::C_CharList char_list;
        SendPacket(fd, kCCharList, char_list);
        auto char_reply = ParsePayload<game::S_CharList>(RecvPacket(fd), kSCharList);
        if (char_reply.characters_size() == 0) {
            throw std::runtime_error("character list empty");
        }
        PrintOk("char list count=" + std::to_string(char_reply.characters_size()));

        game::C_SelectChar select;
        select.set_char_id(char_reply.characters(0).char_id());
        SendPacket(fd, kCSelectChar, select);
        auto select_reply = ParsePayload<game::S_SelectChar>(RecvPacket(fd), kSSelectChar);
        if (!select_reply.success()) throw std::runtime_error("select char failed");
        PrintOk("select char name=" + select_reply.name());

        auto inventory = ParsePayload<game::S_InventoryInit>(RecvPacket(fd), kSInventoryInit);
        PrintOk("inventory items=" + std::to_string(inventory.items_size()));

        auto currency = ParsePayload<game::S_CurrencyInit>(RecvPacket(fd), kSCurrencyInit);
        PrintOk("currency gold=" + std::to_string(currency.gold()));

        game::C_RoomList rooms;
        SendPacket(fd, kCRoomList, rooms);
        auto room_list = ParsePayload<game::S_RoomList>(RecvPacket(fd), kSRoomList);
        PrintOk("room list count=" + std::to_string(room_list.rooms_size()));

        game::C_CreateRoom create;
        create.set_room_name("probe_room");
        SendPacket(fd, kCCreateRoom, create);

        auto first = RecvPacket(fd);
        auto second = RecvPacket(fd);
        Packet skill_packet;
        Packet create_packet;
        if (first.id == kSSkillData && second.id == kSCreateRoom) {
            skill_packet = std::move(first);
            create_packet = std::move(second);
        } else if (first.id == kSCreateRoom && second.id == kSSkillData) {
            create_packet = std::move(first);
            skill_packet = std::move(second);
        } else {
            throw std::runtime_error("expected skill/create room packets");
        }

        auto skills = ParsePayload<game::S_SkillData>(skill_packet, kSSkillData);
        auto create_reply = ParsePayload<game::S_CreateRoom>(create_packet, kSCreateRoom);
        if (!create_reply.success()) throw std::runtime_error("create room failed");
        if (create_reply.map_data().grid().empty()) throw std::runtime_error("empty map grid");
        PrintOk("skill count=" + std::to_string(skills.skills_size()));
        PrintOk("create room zone=" + std::to_string(create_reply.zone_id()) +
                " portals=" +
                std::to_string(create_reply.map_data().portals_size()));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SendEmptyPacket(fd, kCSceneReady);
        auto snapshot = ParsePayload<game::S_PlayerList>(RecvPacket(fd), kSPlayerList);
        if (snapshot.players_size() < 15) {
            throw std::runtime_error("expected bot snapshot");
        }
        PrintOk("scene ready snapshot entities=" +
                std::to_string(snapshot.players_size()));

        game::C_Chat chat;
        chat.set_chat_type(game::CHAT_ALL);
        chat.set_message("hello from protocol probe");
        SendPacket(fd, kCChat, chat);
        auto chat_reply = ParsePayload<game::S_Chat>(RecvPacket(fd), kSChat);
        if (chat_reply.message() != chat.message()) {
            throw std::runtime_error("chat echo mismatch");
        }
        PrintOk("chat round trip");

        game::C_CreateParty party;
        SendPacket(fd, kCCreateParty, party);
        auto party_reply =
            ParsePayload<game::S_CreateParty>(RecvPacket(fd), kSCreateParty);
        if (!party_reply.success()) throw std::runtime_error("create party failed");
        auto party_update =
            ParsePayload<game::S_PartyUpdate>(RecvPacket(fd), kSPartyUpdate);
        if (party_update.members_size() != 1) {
            throw std::runtime_error("party update member count mismatch");
        }
        PrintOk("party create/update party_id=" +
                std::to_string(party_reply.party_id()));

        ::close(fd);
        google::protobuf::ShutdownProtobufLibrary();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[fail] " << e.what() << '\n';
        google::protobuf::ShutdownProtobufLibrary();
        return 1;
    }
}
