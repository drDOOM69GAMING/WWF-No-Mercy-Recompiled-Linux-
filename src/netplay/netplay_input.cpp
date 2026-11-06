#include "netplay_input.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#endif

#include "netplay_signaling.hpp"
#include "netplay_stun.hpp"

#include "recompinput/profiles.h"
#include "ultramodern/ultramodern.hpp"

namespace wcw::netplay {

namespace {

enum class Role : uint8_t { Player = 1, Spectator = 2 };
enum class PacketType : uint8_t {
    Hello = 1,
    Welcome = 2,
    Input = 3,
    Frame = 4,
    Heartbeat = 5,
    Close = 6,
    Punch = 7,
    Resync = 8,
};

struct PadState {
    uint16_t buttons = 0;
    int8_t x = 0;
    int8_t y = 0;
};

struct FramePads {
    uint32_t frame = 0;
    std::array<PadState, 4> pads{};
};

constexpr uint32_t kMagic = 0x4E4D4E50; // "NMNP"
constexpr uint8_t kVersion = 2;
constexpr size_t kPacketSize = 252;
constexpr size_t kAuthenticatedSize = 236;
constexpr size_t kMacSize = 16;
constexpr size_t kPadsOffset = 24;
constexpr size_t kHistoryCountOffset = 40;
constexpr size_t kHistoryOffset = 44;
constexpr size_t kMacOffset = kAuthenticatedSize;
constexpr size_t kHistoryCount = 12;
constexpr int kMaxInputDelay = 8;
constexpr int kMaxSpectators = 16;
constexpr int kInputWaitMs = 12;
constexpr int kFrameTimeoutMs = 5000;
constexpr auto kHandshakeTimeout = std::chrono::seconds(120);
constexpr size_t kFinalizedHistory = 128;

uint32_t read_u32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint64_t read_u64(const uint8_t* p) {
    return (uint64_t(read_u32(p)) << 32) | read_u32(p + 4);
}

void write_u32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

void write_u64(uint8_t* p, uint64_t v) {
    write_u32(p, uint32_t(v >> 32));
    write_u32(p + 4, uint32_t(v));
}

#if defined(__linux__)
using Packet = std::array<uint8_t, kPacketSize>;

bool authenticate(Packet& packet, const std::string& secret) {
    unsigned int digest_size = 0;
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             packet.data(), kAuthenticatedSize, digest, &digest_size) == nullptr ||
        digest_size < kMacSize) {
        return false;
    }
    std::memcpy(packet.data() + kMacOffset, digest, kMacSize);
    return true;
}

bool verify_packet(const Packet& packet, const std::string& secret) {
    Packet expected = packet;
    std::memset(expected.data() + kMacOffset, 0, kMacSize);
    if (!authenticate(expected, secret)) return false;
    return CRYPTO_memcmp(expected.data() + kMacOffset, packet.data() + kMacOffset, kMacSize) == 0;
}

void encode_pads(Packet& packet, size_t offset, const std::array<PadState, 4>& pads) {
    for (size_t i = 0; i < pads.size(); i++) {
        const size_t base = offset + i * 4;
        packet[base] = uint8_t(pads[i].buttons >> 8);
        packet[base + 1] = uint8_t(pads[i].buttons);
        packet[base + 2] = static_cast<uint8_t>(pads[i].x);
        packet[base + 3] = static_cast<uint8_t>(pads[i].y);
    }
}

std::array<PadState, 4> decode_pads(const Packet& packet, size_t offset) {
    std::array<PadState, 4> pads{};
    for (size_t i = 0; i < pads.size(); i++) {
        const size_t base = offset + i * 4;
        pads[i].buttons = uint16_t((uint16_t(packet[base]) << 8) | packet[base + 1]);
        pads[i].x = static_cast<int8_t>(packet[base + 2]);
        pads[i].y = static_cast<int8_t>(packet[base + 3]);
    }
    return pads;
}

PadState decode_pad(const Packet& packet, int port) {
    const size_t offset = kPadsOffset + size_t(port) * 4;
    return {
        .buttons = uint16_t((uint16_t(packet[offset]) << 8) | packet[offset + 1]),
        .x = static_cast<int8_t>(packet[offset + 2]),
        .y = static_cast<int8_t>(packet[offset + 3]),
    };
}

void encode_pad(Packet& packet, int port, const PadState& pad) {
    const size_t offset = kPadsOffset + size_t(port) * 4;
    packet[offset] = uint8_t(pad.buttons >> 8);
    packet[offset + 1] = uint8_t(pad.buttons);
    packet[offset + 2] = static_cast<uint8_t>(pad.x);
    packet[offset + 3] = static_cast<uint8_t>(pad.y);
}

Packet make_packet(PacketType type, Role role, uint8_t seat, uint32_t frame,
                   uint64_t session, uint8_t player_mask, uint8_t input_delay,
                   const std::array<PadState, 4>& pads,
                   const std::vector<FramePads>& history,
                   const std::string& secret) {
    Packet packet{};
    write_u32(packet.data(), kMagic);
    packet[4] = kVersion;
    packet[5] = static_cast<uint8_t>(type);
    packet[6] = static_cast<uint8_t>(role);
    packet[7] = seat;
    write_u32(packet.data() + 8, frame);
    write_u64(packet.data() + 12, session);
    packet[20] = player_mask;
    packet[21] = input_delay;
    encode_pads(packet, kPadsOffset, pads);

    const size_t count = std::min(history.size(), kHistoryCount);
    packet[kHistoryCountOffset] = static_cast<uint8_t>(count);
    for (size_t i = 0; i < count; i++) {
        encode_pads(packet, kHistoryOffset + i * 16, history[i].pads);
    }

    authenticate(packet, secret);
    return packet;
}

bool decode_packet(const uint8_t* data, size_t size, Packet& packet, const std::string& secret) {
    if (size != kPacketSize) return false;
    std::memcpy(packet.data(), data, kPacketSize);
    return read_u32(packet.data()) == kMagic && packet[4] == kVersion && verify_packet(packet, secret);
}

std::vector<FramePads> decode_history(const Packet& packet, uint32_t base_frame) {
    std::vector<FramePads> history;
    const size_t count = std::min<size_t>(packet[kHistoryCountOffset], kHistoryCount);
    history.reserve(count);
    for (size_t i = 0; i < count; i++) {
        if (base_frame < i) break;
        FramePads entry;
        entry.frame = base_frame - uint32_t(i);
        entry.pads = decode_pads(packet, kHistoryOffset + i * 16);
        history.push_back(entry);
    }
    return history;
}

bool same_endpoint(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

sockaddr_in to_sockaddr(const Endpoint& endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    inet_pton(AF_INET, endpoint.ip.c_str(), &address.sin_addr);
    return address;
}

Endpoint from_sockaddr(const sockaddr_in& address) {
    char text[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
    return Endpoint{text, ntohs(address.sin_port)};
}

bool resolve_endpoint(const std::string& host, uint16_t port, Endpoint& out) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
        if (result) freeaddrinfo(result);
        return false;
    }
    const auto* address = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
    char text[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
    out.ip = text;
    out.port = port;
    freeaddrinfo(result);
    return true;
}

nlohmann::json candidates_to_json(const std::vector<Endpoint>& candidates) {
    nlohmann::json list = nlohmann::json::array();
    for (const Endpoint& endpoint : candidates) {
        list.push_back({{"ip", endpoint.ip}, {"port", endpoint.port}});
    }
    return list;
}

std::vector<Endpoint> candidates_from_json(const nlohmann::json& value) {
    std::vector<Endpoint> candidates;
    if (!value.is_array()) return candidates;
    for (const nlohmann::json& item : value) {
        Endpoint endpoint;
        endpoint.ip = item.value("ip", "");
        const int port = item.value("port", 0);
        if (endpoint.ip.empty() || port <= 0 || port > 65535) continue;
        endpoint.port = static_cast<uint16_t>(port);
        candidates.push_back(endpoint);
    }
    return candidates;
}

void add_unique(std::vector<Endpoint>& list, const Endpoint& endpoint) {
    if (!endpoint.valid()) return;
    for (const Endpoint& existing : list) {
        if (existing.ip == endpoint.ip && existing.port == endpoint.port) return;
    }
    list.push_back(endpoint);
}

std::string describe_candidates(const std::vector<Endpoint>& candidates) {
    std::string text;
    for (const Endpoint& endpoint : candidates) {
        if (!text.empty()) text += ", ";
        text += endpoint.ip + ":" + std::to_string(endpoint.port);
    }
    return text;
}
#endif

} // namespace

#if defined(__linux__)
struct InputSession::Impl {
    struct Peer {
        sockaddr_in address{};
        Role role = Role::Player;
        uint8_t seat = 0;
        uint32_t last_frame = 0;
        uint64_t packets = 0;
        uint64_t substitutions = 0;
        std::chrono::steady_clock::time_point last_packet{};
    };

    Mode mode = Mode::Off;
    std::string secret;
    std::string host_name;
    uint16_t port = 27025;
    uint8_t seat = 0;
    uint8_t player_count = 2;
    uint8_t spectator_count = 0;
    uint8_t player_mask = 0;
    uint8_t input_delay = 4;
    uint64_t session_id = 0;
    uint32_t frame = 0;
    int socket_fd = -1;
    uint16_t local_port = 0;
    bool initialized = false;
    bool ready = false;
    bool failed = false;
    bool env_configured = false;
    bool room_closed = false;

    std::array<PadState, 4> current_pads{};
    std::map<uint8_t, Peer> players;
    std::vector<Peer> spectators;

    // Host: per-seat input samples keyed by target frame.
    std::map<uint8_t, std::map<uint32_t, PadState>> seat_inputs;
    // Host: finalized pads for resync requests and packet redundancy.
    std::deque<FramePads> finalized;
    // Client: frames received from the host, jitter-buffered.
    std::map<uint32_t, std::array<PadState, 4>> received;
    // Client: locally sampled inputs keyed by the frame they were tagged for.
    std::map<uint32_t, PadState> local_inputs;
    std::chrono::steady_clock::time_point last_frame_time{};

    // Invite-code signaling.
    Config config;
    std::unique_ptr<SignalingClient> signaling;
    RoomInfo room;
    int event_cursor = 0;
    std::map<std::string, SignalingPeer> room_peers;
    std::set<std::string> sent_hello;
    std::vector<Endpoint> our_candidates;
    std::vector<Endpoint> host_candidates;
    std::vector<Endpoint> punch_targets;
    Endpoint mapped;
    Endpoint host_endpoint;
    std::chrono::steady_clock::time_point next_heartbeat{};
    std::chrono::steady_clock::time_point next_signaling_poll{};
    std::chrono::steady_clock::time_point next_join_hello{};
    std::chrono::steady_clock::time_point next_punch{};
    std::chrono::steady_clock::time_point next_udp_hello{};

    mutable std::mutex status_mutex;
    Status status_snapshot;

    Impl() {
        const char* mode_env = std::getenv("WCW_NETPLAY_MODE");
        if (mode_env == nullptr || mode_env[0] == '\0') return;
        if (std::strcmp(mode_env, "host") == 0) mode = Mode::Host;
        else if (std::strcmp(mode_env, "join") == 0) mode = Mode::Join;
        else if (std::strcmp(mode_env, "spectate") == 0) mode = Mode::Spectate;
        else {
            std::fprintf(stderr, "[netplay] WCW_NETPLAY_MODE must be host, join, or spectate\n");
            mode = Mode::Off;
            return;
        }

        const char* secret_env = std::getenv("WCW_NETPLAY_SECRET");
        if (secret_env == nullptr || std::strlen(secret_env) < 16) {
            std::fprintf(stderr, "[netplay] set WCW_NETPLAY_SECRET to a shared secret of at least 16 characters\n");
            mode = Mode::Off;
            return;
        }
        secret = secret_env;
        config.secret = secret;
        port = static_cast<uint16_t>(read_env_int("WCW_NETPLAY_PORT", 27025, 1, 65535));
        config.port = port;
        input_delay = static_cast<uint8_t>(read_env_int("WCW_NETPLAY_DELAY", 4, 0, kMaxInputDelay));
        config.input_delay = input_delay;
        if (mode == Mode::Host) {
            player_count = static_cast<uint8_t>(read_env_int("WCW_NETPLAY_PLAYERS", 2, 2, 4));
            spectator_count = static_cast<uint8_t>(read_env_int("WCW_NETPLAY_SPECTATORS", 0, 0, kMaxSpectators));
            player_mask = static_cast<uint8_t>((1u << player_count) - 1u);
            seat = 0;
        } else {
            const char* host_env = std::getenv("WCW_NETPLAY_HOST");
            if (host_env == nullptr || host_env[0] == '\0') {
                std::fprintf(stderr, "[netplay] join/spectate requires WCW_NETPLAY_HOST\n");
                mode = Mode::Off;
                return;
            }
            host_name = host_env;
            if (mode == Mode::Join) {
                seat = static_cast<uint8_t>(read_env_int("WCW_NETPLAY_SEAT", 1, 1, 3));
            } else {
                seat = 0xFF;
            }
        }
        config.mode = mode;
        config.players = player_count;
        config.seat = seat;
        config.spectators = spectator_count;
        config.host = host_name;
        env_configured = true;
        std::fprintf(stderr, "[netplay] direct UDP mode configured by environment: %s, port=%u\n",
            mode == Mode::Host ? "host" : (mode == Mode::Join ? "player" : "spectator"), port);
        publish_status();
    }

    static int read_env_int(const char* name, int fallback, int lo, int hi) {
        const char* value = std::getenv(name);
        if (value == nullptr || value[0] == '\0') return fallback;
        char* end = nullptr;
        long parsed = std::strtol(value, &end, 10);
        if (end == value || *end != '\0' || parsed < lo || parsed > hi) {
            std::fprintf(stderr, "[netplay] invalid %s; using %d\n", name, fallback);
            return fallback;
        }
        return static_cast<int>(parsed);
    }

    void publish_status() {
        std::lock_guard lock(status_mutex);
        status_snapshot.enabled = mode != Mode::Off;
        status_snapshot.ready = ready;
        status_snapshot.failed = failed;
        status_snapshot.env_configured = env_configured;
        status_snapshot.mode = mode;
        status_snapshot.seat = seat;
        status_snapshot.players = player_count;
        status_snapshot.port = port;
        status_snapshot.input_delay = input_delay;
        status_snapshot.frame = frame;
        status_snapshot.room_code = room.room_code;
        status_snapshot.host_candidates = describe_candidates(host_candidates);
        status_snapshot.peers = mode == Mode::Host
            ? static_cast<int>(players.size() + spectators.size())
            : (ready ? 1 : 0);
        status_snapshot.peer_status.clear();

        const auto now = std::chrono::steady_clock::now();
        auto ms_since = [&](std::chrono::steady_clock::time_point then) {
            if (then.time_since_epoch().count() == 0) return 0;
            return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count());
        };

        PeerStatus self;
        self.seat = mode == Mode::Host ? 0 : seat;
        self.role = mode == Mode::Host ? "host" : (mode == Mode::Join ? "player" : "spectator");
        self.connected = ready;
        self.packets = 0;
        status_snapshot.peer_status.push_back(self);

        if (mode == Mode::Host) {
            for (const auto& [peer_seat, peer] : players) {
                if (peer_seat == 0) continue;
                PeerStatus entry;
                entry.seat = peer_seat;
                entry.role = "player";
                entry.connected = true;
                entry.lead_frames = static_cast<int>(int64_t(peer.last_frame) - int64_t(frame));
                entry.ms_since_packet = ms_since(peer.last_packet);
                entry.packets = peer.packets;
                entry.substitutions = peer.substitutions;
                status_snapshot.peer_status.push_back(entry);
            }
            for (const Peer& peer : spectators) {
                PeerStatus entry;
                entry.seat = -1;
                entry.role = "spectator";
                entry.connected = true;
                entry.ms_since_packet = ms_since(peer.last_packet);
                entry.packets = peer.packets;
                status_snapshot.peer_status.push_back(entry);
            }
        } else {
            PeerStatus host_entry;
            host_entry.seat = 0;
            host_entry.role = "host";
            host_entry.connected = ready;
            host_entry.ms_since_packet = ms_since(last_frame_time);
            status_snapshot.peer_status.push_back(host_entry);
        }
    }

    bool apply_config(const Config& config) {
        if (env_configured) {
            std::fprintf(stderr, "[netplay] WCW_NETPLAY_* variables are set; ignoring menu settings\n");
            return false;
        }
        if (initialized) {
            std::fprintf(stderr, "[netplay] session already started; menu settings apply to the next launch\n");
            return false;
        }

        std::string error;
        if (!InputSession::validate(config, error)) {
            std::fprintf(stderr, "[netplay] invalid menu settings: %s\n", error.c_str());
            return false;
        }

        this->config = config;
        mode = config.mode;
        secret = config.secret;
        host_name = config.host;
        port = config.port;
        player_count = config.players;
        spectator_count = config.spectators;
        player_mask = static_cast<uint8_t>((1u << player_count) - 1u);
        input_delay = config.input_delay;
        if (mode == Mode::Host) {
            seat = 0;
        } else if (mode == Mode::Join) {
            seat = config.seat;
        } else {
            seat = 0xFF;
        }
        ready = false;
        failed = false;
        room_closed = false;
        publish_status();
        std::fprintf(stderr, "[netplay] menu mode configured: %s, port=%u, players=%u, delay=%u%s\n",
            mode == Mode::Host ? "host" : (mode == Mode::Join ? "player" : "spectator"),
            unsigned(port), unsigned(player_count), unsigned(input_delay),
            config.use_signaling ? " (invite code)" : " (direct)");
        return true;
    }

    bool open_socket() {
        socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd < 0) {
            std::perror("[netplay] socket");
            return false;
        }
        int flags = fcntl(socket_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            std::perror("[netplay] fcntl");
            ::close(socket_fd);
            socket_fd = -1;
            return false;
        }

        if (mode == Mode::Host) {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = htons(port);
            if (::bind(socket_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
                std::perror("[netplay] bind");
                ::close(socket_fd);
                socket_fd = -1;
                return false;
            }
            local_port = port;
            uint8_t random_bytes[8]{};
            if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1) {
                std::fprintf(stderr, "[netplay] failed to create session id\n");
                return false;
            }
            session_id = read_u64(random_bytes);
            if (session_id == 0) session_id = 1;
        } else {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = 0;
            if (::bind(socket_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
                std::perror("[netplay] bind");
                ::close(socket_fd);
                socket_fd = -1;
                return false;
            }
            sockaddr_in bound{};
            socklen_t bound_len = sizeof(bound);
            if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
                local_port = ntohs(bound.sin_port);
            }
        }
        return true;
    }

    void send_packet(const sockaddr_in& destination, const Packet& packet) {
        if (socket_fd < 0) return;
        (void)::sendto(socket_fd, packet.data(), packet.size(), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    }

    void send_packet_to_candidates(const std::vector<Endpoint>& candidates, const Packet& packet) {
        for (const Endpoint& endpoint : candidates) {
            send_packet(to_sockaddr(endpoint), packet);
        }
    }

    bool receive_packet(Packet& packet, sockaddr_in& source) {
        uint8_t bytes[kPacketSize + 1]{};
        socklen_t source_len = sizeof(source);
        ssize_t n = ::recvfrom(socket_fd, bytes, sizeof(bytes), 0,
            reinterpret_cast<sockaddr*>(&source), &source_len);
        if (n < 0) return false;
        return decode_packet(bytes, static_cast<size_t>(n), packet, secret);
    }

    void drain_packets() {
        Packet packet{};
        sockaddr_in source{};
        while (receive_packet(packet, source)) {
            if (mode == Mode::Host) handle_host_packet(packet, source);
            else handle_client_packet(packet, source);
        }
    }

    void send_punch(const sockaddr_in& destination) {
        std::array<PadState, 4> empty{};
        Packet packet = make_packet(PacketType::Punch, Role::Player, 0, 0, 0, 0, 0,
            empty, {}, secret);
        send_packet(destination, packet);
    }

    void send_punch_to_candidates(const std::vector<Endpoint>& candidates) {
        for (const Endpoint& endpoint : candidates) {
            send_punch(to_sockaddr(endpoint));
        }
    }

    Peer* find_peer(const sockaddr_in& address) {
        for (auto& [peer_seat, peer] : players) {
            (void)peer_seat;
            if (same_endpoint(peer.address, address)) return &peer;
        }
        for (Peer& peer : spectators) {
            if (same_endpoint(peer.address, address)) return &peer;
        }
        return nullptr;
    }

    std::vector<FramePads> history_ending_at(uint32_t base) const {
        std::vector<FramePads> history;
        history.reserve(kHistoryCount);
        for (auto it = finalized.rbegin(); it != finalized.rend() && history.size() < kHistoryCount; ++it) {
            if (it->frame <= base) {
                history.push_back(*it);
                if (it->frame == 0) break;
            }
        }
        return history;
    }

    void send_frame_to(const sockaddr_in& destination, const FramePads& entry) {
        send_packet(destination, make_packet(PacketType::Frame, Role::Player, 0,
            entry.frame, session_id, player_mask, input_delay, entry.pads,
            history_ending_at(entry.frame), secret));
    }

    void broadcast_frame(const FramePads& entry) {
        const std::vector<FramePads> history = history_ending_at(entry.frame);
        const Packet packet = make_packet(PacketType::Frame, Role::Player, 0,
            entry.frame, session_id, player_mask, input_delay, entry.pads,
            history, secret);
        for (const auto& [peer_seat, peer] : players) {
            (void)peer_seat;
            send_packet(peer.address, packet);
        }
        for (const Peer& peer : spectators) send_packet(peer.address, packet);
    }

    void send_welcome(const sockaddr_in& destination, Role role, uint8_t remote_seat) {
        std::array<PadState, 4> empty{};
        send_packet(destination, make_packet(PacketType::Welcome, role, remote_seat,
            0, session_id, player_mask, input_delay, empty, {}, secret));
    }

    void send_close(const sockaddr_in& destination, uint8_t remote_seat) {
        std::array<PadState, 4> empty{};
        send_packet(destination, make_packet(PacketType::Close, Role::Spectator,
            remote_seat, frame, session_id, player_mask, input_delay, empty, {}, secret));
    }

    void handle_host_packet(const Packet& packet, const sockaddr_in& source) {
        const PacketType type = static_cast<PacketType>(packet[5]);
        const Role role = static_cast<Role>(packet[6]);
        const uint8_t remote_seat = packet[7];
        const uint32_t packet_frame = read_u32(packet.data() + 8);
        const uint64_t packet_session = read_u64(packet.data() + 12);

        if (type == PacketType::Punch) {
            send_punch(source);
            return;
        }

        if (type == PacketType::Hello) {
            auto existing = players.find(remote_seat);
            const bool seat_known = existing != players.end();
            if ((frame != 0 || packet_session != 0) && !seat_known) {
                send_close(source, remote_seat);
                return;
            }
            if (role == Role::Player) {
                if (remote_seat == 0 || remote_seat >= player_count) {
                    send_close(source, remote_seat);
                    return;
                }
                if (seat_known) {
                    // The same client can reach us through several candidates
                    // (loopback, LAN, STUN). Treat a Hello from another source
                    // as an alternate path instead of tearing the seat down.
                    existing->second.address = source;
                    existing->second.last_packet = std::chrono::steady_clock::now();
                    send_welcome(source, role, remote_seat);
                    publish_status();
                    return;
                }
                Peer peer{source, Role::Player, remote_seat, 0, 0, 0, std::chrono::steady_clock::now()};
                players[remote_seat] = peer;
                std::fprintf(stderr, "[netplay] player connected at seat %u\n", remote_seat);
            } else if (role == Role::Spectator) {
                if (spectators.size() >= kMaxSpectators) {
                    send_close(source, remote_seat);
                    return;
                }
                if (find_peer(source) == nullptr) {
                    Peer peer{source, Role::Spectator, 0xFF, 0, 0, 0, std::chrono::steady_clock::now()};
                    spectators.push_back(peer);
                    std::fprintf(stderr, "[netplay] spectator connected (%zu/%d)\n", spectators.size(), kMaxSpectators);
                }
            } else {
                send_close(source, remote_seat);
                return;
            }
            send_welcome(source, role, remote_seat);
            publish_status();
            return;
        }

        Peer* peer = find_peer(source);
        if (peer == nullptr || packet_session != session_id) return;
        peer->last_packet = std::chrono::steady_clock::now();
        peer->packets++;

        if (type == PacketType::Input && peer->role == Role::Player && remote_seat == peer->seat) {
            auto& inputs = seat_inputs[peer->seat];
            const std::vector<FramePads> history = decode_history(packet, packet_frame);
            for (const FramePads& entry : history) {
                inputs[entry.frame] = entry.pads[peer->seat];
            }
            inputs[packet_frame] = decode_pad(packet, peer->seat);
            peer->last_frame = std::max(peer->last_frame, packet_frame);

            const uint32_t oldest_keep = frame > 64 ? frame - 64 : 0;
            while (!inputs.empty() && inputs.begin()->first < oldest_keep) {
                inputs.erase(inputs.begin());
            }
        } else if (type == PacketType::Resync && peer->role == Role::Player) {
            // Burst the requested frame and everything newer so a client that
            // fell behind can rebuild its jitter buffer in one go.
            uint32_t wanted = packet_frame;
            for (const FramePads& entry : finalized) {
                if (entry.frame >= wanted) {
                    send_frame_to(source, entry);
                }
            }
            std::fprintf(stderr, "[netplay] resync requested at frame %u (host frame %u)\n",
                wanted, frame);
        } else if (type == PacketType::Heartbeat && peer->role == Role::Spectator) {
            // Nothing to do: the host already broadcasts every finalized frame.
        }
    }

    void handle_client_packet(const Packet& packet, const sockaddr_in& source) {
        const PacketType type = static_cast<PacketType>(packet[5]);
        const uint64_t packet_session = read_u64(packet.data() + 12);

        if (type == PacketType::Punch) {
            if (!host_endpoint.valid()) {
                host_endpoint = from_sockaddr(source);
                std::fprintf(stderr, "[netplay] punched through to host at %s:%u\n",
                    host_endpoint.ip.c_str(), host_endpoint.port);
            }
            send_punch(source);
            return;
        }
        if (type == PacketType::Close) {
            failed = true;
            ready = false;
            std::fprintf(stderr, "[netplay] host closed the session\n");
            return;
        }
        if (type == PacketType::Welcome && packet[7] == seat) {
            if (packet_session == 0) {
                failed = true;
                return;
            }
            session_id = packet_session;
            player_mask = packet[20];
            player_count = 0;
            for (int i = 0; i < 4; i++) {
                if (player_mask & (1u << i)) player_count = static_cast<uint8_t>(i + 1);
            }
            if (mode == Mode::Join && !(player_mask & (1u << seat))) {
                failed = true;
                return;
            }
            if (packet[21] != 0) input_delay = packet[21];
            host_endpoint = from_sockaddr(source);
            ready = true;
            last_frame_time = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[netplay] connected to host; seat=%s, delay=%u\n",
                mode == Mode::Join ? std::to_string(seat).c_str() : "spectator",
                unsigned(input_delay));
            publish_status();
            return;
        }

        if (packet_session != session_id) return;

        if (type == PacketType::Frame) {
            const uint32_t base = read_u32(packet.data() + 8);
            const std::vector<FramePads> history = decode_history(packet, base);
            for (const FramePads& entry : history) {
                if (entry.frame >= frame) received.emplace(entry.frame, entry.pads);
            }
            if (base >= frame) {
                received.emplace(base, decode_pads(packet, kPadsOffset));
            }
            last_frame_time = std::chrono::steady_clock::now();
        }
    }

    // ---- Signaling -------------------------------------------------------

    void setup_signaling() {
        signaling = std::make_unique<SignalingClient>(config.server_url);
        room.server_url = config.server_url;
        room.room_code = config.room_code;
        room.peer_id = config.peer_id;
        room.peer_token = config.peer_token;
        room.host_peer_id = config.host_peer_id;
        room.stun_port = config.stun_port;
        event_cursor = 0;

        std::string server_host;
        std::string base;
        uint16_t server_port = 0;
        if (config.stun_port > 0 &&
            SignalingClient::split_server_url(config.server_url, base, server_host, server_port)) {
            Endpoint stun_mapped;
            if (stun_query(socket_fd, server_host, static_cast<uint16_t>(config.stun_port), stun_mapped)) {
                mapped = stun_mapped;
                std::fprintf(stderr, "[netplay] STUN mapped endpoint: %s:%u\n",
                    mapped.ip.c_str(), mapped.port);
            } else {
                std::fprintf(stderr, "[netplay] STUN query failed; using local candidates only\n");
            }
        }
        our_candidates = build_candidates(mapped, local_port);
        std::fprintf(stderr, "[netplay] local candidates: %s\n",
            describe_candidates(our_candidates).c_str());
        next_heartbeat = std::chrono::steady_clock::now();
        next_signaling_poll = std::chrono::steady_clock::now();
        next_join_hello = std::chrono::steady_clock::now();
        next_punch = std::chrono::steady_clock::now();
        next_udp_hello = std::chrono::steady_clock::now();
        publish_status();
    }

    nlohmann::json hello_payload() const {
        return {
            {"candidates", candidates_to_json(our_candidates)},
            {"seat", seat},
            {"role", mode == Mode::Host ? "host" : (mode == Mode::Join ? "player" : "spectator")},
        };
    }

    void send_signal_hello(const std::string& peer_id) {
        if (!signaling || peer_id.empty()) return;
        std::string error;
        if (signaling->signal(room, peer_id, "p2p_hello", hello_payload(), error)) {
            sent_hello.insert(peer_id);
        } else {
            std::fprintf(stderr, "[netplay] p2p_hello to %s failed: %s\n",
                peer_id.c_str(), error.c_str());
        }
    }

    void handle_signaling_event(const SignalEvent& event) {
        if (event.kind == "peer_joined") {
            room_peers[event.peer.peer_id] = event.peer;
            std::fprintf(stderr, "[netplay] room peer joined: %s (%s, slot %d)\n",
                event.peer.name.c_str(), event.peer.role.c_str(), event.peer.player_slot);
        } else if (event.kind == "peer_left") {
            room_peers.erase(event.peer_id);
        } else if (event.kind == "room_closed") {
            room_closed = true;
            std::fprintf(stderr, "[netplay] room closed: %s\n", event.reason.c_str());
        } else if (event.kind == "seat_changed") {
            auto it = room_peers.find(event.peer_id);
            if (it != room_peers.end()) {
                it->second.role = event.peer.role;
                it->second.player_slot = event.peer.player_slot;
            }
        } else if (event.kind == "signal" && event.type == "p2p_hello") {
            const std::vector<Endpoint> candidates = candidates_from_json(event.data.value("candidates", nlohmann::json::array()));
            if (mode == Mode::Host) {
                for (const Endpoint& endpoint : candidates) {
                    add_unique(punch_targets, endpoint);
                }
                if (!sent_hello.contains(event.from_peer_id)) {
                    send_signal_hello(event.from_peer_id);
                }
            } else {
                host_candidates.clear();
                for (const Endpoint& endpoint : candidates) {
                    add_unique(host_candidates, endpoint);
                }
                if (!host_candidates.empty()) {
                    std::fprintf(stderr, "[netplay] host candidates: %s\n",
                        describe_candidates(host_candidates).c_str());
                }
            }
        }
    }

    void pump_signaling(int wait_ms) {
        if (!signaling) return;
        const auto now = std::chrono::steady_clock::now();

        if (now >= next_heartbeat) {
            std::string error;
            if (!signaling->heartbeat(room, nullptr, error) && !error.empty()) {
                std::fprintf(stderr, "[netplay] heartbeat failed: %s\n", error.c_str());
            }
            const int interval = std::max(5, room.heartbeat_seconds / 2);
            next_heartbeat = now + std::chrono::seconds(interval);
        }

        std::vector<SignalEvent> events;
        int cursor = event_cursor;
        std::string error;
        if (now >= next_signaling_poll) {
            if (signaling->poll_events(room, cursor, wait_ms, events, error)) {
                event_cursor = cursor;
                for (const SignalEvent& event : events) {
                    handle_signaling_event(event);
                }
            } else if (!error.empty()) {
                static auto last_error = std::chrono::steady_clock::time_point{};
                if (now - last_error > std::chrono::seconds(5)) {
                    last_error = now;
                    std::fprintf(stderr, "[netplay] signaling poll failed: %s\n", error.c_str());
                }
            }
            next_signaling_poll = now + std::chrono::milliseconds(std::max(wait_ms, 100));
        }

        if (mode != Mode::Host && host_candidates.empty() && now >= next_join_hello) {
            send_signal_hello(room.host_peer_id);
            next_join_hello = now + std::chrono::seconds(1);
        }

        if (now >= next_punch) {
            if (mode == Mode::Host) {
                send_punch_to_candidates(punch_targets);
            } else {
                send_punch_to_candidates(host_candidates);
            }
            next_punch = now + std::chrono::milliseconds(100);
        }
    }

    // ---- Handshake -------------------------------------------------------

    bool all_remote_inputs_ready() const {
        for (uint8_t i = 1; i < player_count; i++) {
            auto it = seat_inputs.find(i);
            if (it == seat_inputs.end() || it->second.find(frame) == it->second.end()) {
                return false;
            }
        }
        return true;
    }

    bool init_host() {
        if (!open_socket()) return false;
        initialized = true;
        if (config.use_signaling) setup_signaling();

        std::fprintf(stderr, "[netplay] host listening on UDP %u; waiting for %u player(s)%s\n",
            port, unsigned(player_count - 1),
            config.use_signaling ? " (invite code)" : "");

        const auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
        while (players.size() < size_t(player_count - 1) || spectators.size() < spectator_count) {
            drain_packets();
            if (config.use_signaling) pump_signaling(0);
            if (room_closed) {
                std::fprintf(stderr, "[netplay] signaling room closed while waiting for players\n");
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "[netplay] timed out waiting for all active players\n");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ready = true;

        // Give clients a moment to send their first input window so the host
        // does not substitute the opening frames while they boot.
        const auto grace = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < grace && !all_remote_inputs_ready()) {
            drain_packets();
            if (config.use_signaling) pump_signaling(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        publish_status();
        return true;
    }

    bool init_client() {
        if (!open_socket()) return false;
        initialized = true;

        if (config.use_signaling) {
            setup_signaling();
            if (!room.host_peer_id.empty()) {
                send_signal_hello(room.host_peer_id);
            }
        } else {
            Endpoint direct;
            if (!resolve_endpoint(host_name, port, direct)) {
                std::fprintf(stderr, "[netplay] failed to resolve host %s\n", host_name.c_str());
                return false;
            }
            host_candidates.push_back(direct);
        }

        Role role = mode == Mode::Join ? Role::Player : Role::Spectator;
        std::array<PadState, 4> empty{};
        Packet hello = make_packet(PacketType::Hello, role, seat, 0, 0, 0, input_delay,
            empty, {}, secret);

        const auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            drain_packets();
            if (ready) {
                break;
            }
            if (config.use_signaling) pump_signaling(0);
            if (room_closed) {
                std::fprintf(stderr, "[netplay] signaling room closed while connecting\n");
                return false;
            }
            if (failed) return false;

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_udp_hello && !host_candidates.empty()) {
                send_packet_to_candidates(host_candidates, hello);
                next_udp_hello = now + std::chrono::milliseconds(250);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (ready) {
            return true;
        }
        std::fprintf(stderr, "[netplay] timed out connecting to host\n");
        return false;
    }

    bool initialize() {
        if (initialized) return ready;
        initialized = true;
        bool ok = mode == Mode::Host ? init_host() : init_client();
        ready = ok;
        failed = !ok;
        publish_status();
        return ok;
    }

    bool poll_host(PadState local) {
        drain_packets();

        // Bounded jitter buffer: wait briefly for a late input, then substitute
        // that player's most recent sample so the match keeps running.
        const auto wait_until = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(kInputWaitMs);
        while (!all_remote_inputs_ready() && std::chrono::steady_clock::now() < wait_until) {
            drain_packets();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::array<PadState, 4> pads{};
        pads[0] = local;
        for (uint8_t seat_index = 1; seat_index < player_count; seat_index++) {
            auto& inputs = seat_inputs[seat_index];
            auto exact = inputs.find(frame);
            if (exact != inputs.end()) {
                pads[seat_index] = exact->second;
                continue;
            }

            auto it = inputs.upper_bound(frame);
            if (it != inputs.begin()) {
                --it;
                pads[seat_index] = it->second;
            } else {
                pads[seat_index] = PadState{};
            }
            auto peer = players.find(seat_index);
            if (peer != players.end()) {
                peer->second.substitutions++;
            }
        }

        FramePads entry{frame, pads};
        finalized.push_back(entry);
        while (finalized.size() > kFinalizedHistory) finalized.pop_front();
        current_pads = pads;
        broadcast_frame(entry);
        frame++;
        publish_status();
        return true;
    }

    bool poll_client(PadState local) {
        const Role role = mode == Mode::Join ? Role::Player : Role::Spectator;
        const uint32_t target_frame = frame + input_delay;

        local_inputs[target_frame] = local;
        const uint32_t oldest_keep = target_frame > 64 ? target_frame - 64 : 0;
        while (!local_inputs.empty() && local_inputs.begin()->first < oldest_keep) {
            local_inputs.erase(local_inputs.begin());
        }

        std::vector<FramePads> history;
        history.reserve(kHistoryCount);
        for (size_t i = 0; i < kHistoryCount; i++) {
            if (target_frame < i) break;
            FramePads entry;
            entry.frame = target_frame - uint32_t(i);
            auto sample = local_inputs.find(entry.frame);
            entry.pads[seat] = sample != local_inputs.end() ? sample->second : local;
            history.push_back(entry);
        }

        std::array<PadState, 4> pads{};
        if (role == Role::Player) pads[seat] = local;
        const Packet input = make_packet(
            role == Role::Player ? PacketType::Input : PacketType::Heartbeat,
            role, seat, target_frame, session_id, player_mask, input_delay,
            pads, history, secret);
        send_packet_to_candidates(host_candidates, input);

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(kFrameTimeoutMs);
        auto next_resync = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (std::chrono::steady_clock::now() < deadline) {
            drain_packets();
            if (failed) return false;

            auto found = received.find(frame);
            if (found != received.end()) {
                current_pads = found->second;
                received.erase(found);
                while (!received.empty() && received.begin()->first < frame) {
                    received.erase(received.begin());
                }
                frame++;
                publish_status();
                return true;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_resync) {
                std::array<PadState, 4> empty{};
                const Packet resync = make_packet(PacketType::Resync, role, seat,
                    frame, session_id, player_mask, input_delay, empty, {}, secret);
                send_packet_to_candidates(host_candidates, resync);
                next_resync = now + std::chrono::milliseconds(100);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::fprintf(stderr, "[netplay] timed out waiting for frame %u\n", frame);
        return false;
    }

    bool poll(PadState local) {
        if (mode == Mode::Off) return true;
        if (failed || !initialize()) return false;
        return mode == Mode::Host ? poll_host(local) : poll_client(local);
    }

    bool get(int port_index, uint16_t* buttons, float* x, float* y) const {
        if (port_index < 0 || port_index >= 4 || !buttons || !x || !y) return false;
        const PadState& pad = current_pads[port_index];
        *buttons = pad.buttons;
        *x = static_cast<float>(pad.x) / 80.0f;
        *y = static_cast<float>(pad.y) / 80.0f;
        return true;
    }
};

InputSession& InputSession::instance() {
    static InputSession session;
    return session;
}

InputSession::InputSession() : impl_(new Impl()) {}
InputSession::~InputSession() {
    if (impl_ && impl_->socket_fd >= 0) ::close(impl_->socket_fd);
    delete impl_;
}

bool InputSession::enabled() const {
    return impl_->mode != Mode::Off;
}

bool InputSession::poll_input() {
    auto& impl = *impl_;
    if (impl.mode == Mode::Off) return true;
    uint16_t buttons = 0;
    float x = 0.0f, y = 0.0f;
    if (impl.mode != Mode::Spectate) {
        recompinput::profiles::get_n64_input(0, &buttons, &x, &y);
    }
    PadState local{};
    local.buttons = buttons;
    local.x = static_cast<int8_t>(std::clamp(x, -1.0f, 1.0f) * 80.0f);
    local.y = static_cast<int8_t>(std::clamp(y, -1.0f, 1.0f) * 80.0f);
    return impl.poll(local);
}

bool InputSession::get_input(int port, uint16_t* buttons, float* x, float* y) const {
    return impl_->get(port, buttons, x, y);
}

bool InputSession::configure(const Config& config) {
    return impl_->apply_config(config);
}

Status InputSession::status() const {
    std::lock_guard lock(impl_->status_mutex);
    return impl_->status_snapshot;
}
#else
struct InputSession::Impl {
    mutable std::mutex status_mutex;
    Status status_snapshot;
};

InputSession& InputSession::instance() {
    static InputSession session;
    return session;
}

InputSession::InputSession() : impl_(new Impl()) {}
InputSession::~InputSession() { delete impl_; }
bool InputSession::enabled() const { return false; }
bool InputSession::poll_input() { return true; }
bool InputSession::get_input(int, uint16_t*, float*, float*) const { return false; }
bool InputSession::configure(const Config&) { return false; }
Status InputSession::status() const {
    std::lock_guard lock(impl_->status_mutex);
    return impl_->status_snapshot;
}
#endif

bool InputSession::validate(const Config& config, std::string& error) {
    if (config.mode == Mode::Off) return true;

    if (config.secret.size() < 16) {
        error = "The shared secret must be at least 16 characters.";
        return false;
    }
    if (config.port == 0) {
        error = "The UDP port must be between 1 and 65535.";
        return false;
    }
    if (config.players < 2 || config.players > 4) {
        error = "Player count must be 2, 3, or 4.";
        return false;
    }
    if (config.input_delay > kMaxInputDelay) {
        error = "Input delay must be between 0 and 8 frames.";
        return false;
    }
    if (config.mode == Mode::Host) {
        if (config.spectators > kMaxSpectators) {
            error = "At most 16 spectators can connect.";
            return false;
        }
    } else if (config.mode == Mode::Join) {
        if (config.seat < 1 || config.seat > 3 || config.seat >= config.players) {
            error = "Pick a player seat between 2 and the configured player count.";
            return false;
        }
    }

    if (config.use_signaling) {
        if (config.server_url.empty()) {
            error = "Enter the signaling server URL.";
            return false;
        }
        if (config.room_code.empty() || config.peer_id.empty() || config.peer_token.empty()) {
            error = "Create or join a room before starting.";
            return false;
        }
        if (config.mode != Mode::Host && config.host_peer_id.empty()) {
            error = "The room did not return a host peer.";
            return false;
        }
        return true;
    }

    if (config.mode != Mode::Host && config.host.empty()) {
        error = "Enter the host's address (IP or hostname).";
        return false;
    }
    return true;
}

} // namespace wcw::netplay
