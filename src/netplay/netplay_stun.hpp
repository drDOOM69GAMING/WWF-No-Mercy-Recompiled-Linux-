#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wcw::netplay {

struct Endpoint {
    std::string ip;
    uint16_t port = 0;

    bool valid() const { return !ip.empty() && port != 0; }
};

// Server-reflexive address plus the machine's local IPv4 addresses. The local
// addresses let LAN players connect even when a public STUN server is not
// reachable or NAT hairpinning is broken.
struct StunResult {
    Endpoint mapped;
    std::vector<Endpoint> local;
};

// Sends one RFC 5389 Binding request from `socket_fd` and waits for the
// response. Returns false on timeout or malformed response.
bool stun_query(int socket_fd, const std::string& host, uint16_t port,
                Endpoint& mapped, int timeout_ms = 1200);

// Enumerates non-loopback IPv4 interfaces (loopback is appended last as a
// fallback so two processes on one machine can still connect for testing).
std::vector<std::string> local_ipv4_addresses();

std::vector<Endpoint> build_candidates(const Endpoint& mapped, uint16_t local_port);

} // namespace wcw::netplay
