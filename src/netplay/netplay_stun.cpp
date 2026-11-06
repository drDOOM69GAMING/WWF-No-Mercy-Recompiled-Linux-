#include "netplay_stun.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

#if defined(__linux__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace wcw::netplay {

#if defined(__linux__)

namespace {

constexpr uint32_t kStunMagicCookie = 0x2112A442u;
constexpr uint16_t kStunBindingRequest = 0x0001u;
constexpr uint16_t kStunBindingResponse = 0x0101u;
constexpr uint16_t kStunAttrMappedAddress = 0x0001u;
constexpr uint16_t kStunAttrXorMappedAddress = 0x0020u;

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
}

uint32_t read_u32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void write_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void write_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

bool parse_mapped_attribute(const uint8_t* data, size_t size, Endpoint& out, bool xor_encoded,
                            const uint8_t* transaction_id) {
    if (size < 8) return false;
    const uint8_t family = data[1];
    if (family != 0x01) return false; // IPv4 only

    uint16_t port = read_u16(data + 2);
    uint32_t address = read_u32(data + 4);
    if (xor_encoded) {
        port ^= static_cast<uint16_t>(kStunMagicCookie >> 16);
        address ^= kStunMagicCookie;
    }
    (void)transaction_id;

    in_addr addr{};
    addr.s_addr = htonl(address);
    char text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &addr, text, sizeof(text)) == nullptr) {
        return false;
    }
    out.ip = text;
    out.port = port;
    return out.valid();
}

} // namespace

bool stun_query(int socket_fd, const std::string& host, uint16_t port,
                Endpoint& mapped, int timeout_ms) {
    if (socket_fd < 0 || host.empty() || port == 0) return false;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
        if (result) freeaddrinfo(result);
        return false;
    }

    uint8_t request[20]{};
    write_u16(request, kStunBindingRequest);
    write_u16(request + 2, 0);
    write_u32(request + 4, kStunMagicCookie);
    std::random_device random;
    for (int i = 0; i < 12; i++) {
        request[8 + i] = static_cast<uint8_t>(random() & 0xFF);
    }

    (void)::sendto(socket_fd, request, sizeof(request), 0,
                   result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{};
        descriptor.fd = socket_fd;
        descriptor.events = POLLIN;
        int ready = ::poll(&descriptor, 1, 20);
        if (ready <= 0) continue;

        uint8_t response[512]{};
        sockaddr_in source{};
        socklen_t source_len = sizeof(source);
        const ssize_t received = ::recvfrom(socket_fd, response, sizeof(response), 0,
                                            reinterpret_cast<sockaddr*>(&source), &source_len);
        if (received < 20) continue;

        const uint16_t type = read_u16(response);
        const uint16_t length = read_u16(response + 2);
        const uint32_t cookie = read_u32(response + 4);
        if (type != kStunBindingResponse || cookie != kStunMagicCookie) continue;
        if (std::memcmp(response + 8, request + 8, 12) != 0) continue;
        if (static_cast<size_t>(received) < 20 + size_t(length)) continue;

        Endpoint found;
        size_t offset = 20;
        while (offset + 4 <= 20 + size_t(length)) {
            const uint16_t attribute_type = read_u16(response + offset);
            const uint16_t attribute_length = read_u16(response + offset + 2);
            offset += 4;
            if (offset + attribute_length > size_t(received)) break;

            if (attribute_type == kStunAttrXorMappedAddress &&
                parse_mapped_attribute(response + offset, attribute_length, found, true, request + 8)) {
                mapped = found;
                return true;
            }
            if (attribute_type == kStunAttrMappedAddress && !found.valid() &&
                parse_mapped_attribute(response + offset, attribute_length, found, false, request + 8)) {
                // Keep looking for XOR-MAPPED-ADDRESS, which is preferred.
            }

            offset += (attribute_length + 3) & ~3u;
        }
        if (found.valid()) {
            mapped = found;
            return true;
        }
    }
    return false;
}

std::vector<std::string> local_ipv4_addresses() {
    std::vector<std::string> addresses;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
            if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) continue;
            const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) == nullptr) continue;
            const std::string value = text;
            if (value == "0.0.0.0" || value == "127.0.0.1") continue;
            if (std::find(addresses.begin(), addresses.end(), value) == addresses.end()) {
                addresses.push_back(value);
            }
        }
        freeifaddrs(interfaces);
    }
    // Loopback is only useful for two processes on one machine, but it makes
    // local end-to-end testing possible.
    addresses.emplace_back("127.0.0.1");
    return addresses;
}

std::vector<Endpoint> build_candidates(const Endpoint& mapped, uint16_t local_port) {
    std::vector<Endpoint> candidates;
    auto add = [&](const std::string& ip, uint16_t port) {
        if (ip.empty() || port == 0) return;
        for (const Endpoint& existing : candidates) {
            if (existing.ip == ip && existing.port == port) return;
        }
        candidates.push_back(Endpoint{ip, port});
    };

    if (mapped.valid()) {
        add(mapped.ip, mapped.port);
    }
    for (const std::string& address : local_ipv4_addresses()) {
        add(address, local_port);
    }
    return candidates;
}

#else

bool stun_query(int, const std::string&, uint16_t, Endpoint&, int) {
    return false;
}

std::vector<std::string> local_ipv4_addresses() {
    return {};
}

std::vector<Endpoint> build_candidates(const Endpoint&, uint16_t) {
    return {};
}

#endif

} // namespace wcw::netplay
