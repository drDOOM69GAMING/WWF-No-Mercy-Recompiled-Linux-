#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wcw::netplay {

enum class Mode : uint8_t {
    Off = 0,
    Host = 1,
    Join = 2,
    Spectate = 3,
};

// Plain settings object shared between the menu and the transport. Keeping it
// free of UI/engine types means it can be validated and applied without any of
// the recompui headers being involved.
struct Config {
    Mode mode = Mode::Off;
    std::string secret;
    // Direct mode: hostname/IP of the host. Signaling mode: unused.
    std::string host;
    uint16_t port = 27025;
    uint8_t players = 2;
    uint8_t seat = 1;
    uint8_t spectators = 0;
    // Host-authoritative input delay, in frames. Clients send their current
    // sample tagged for `frame + input_delay` so the host does not stall on
    // network latency.
    uint8_t input_delay = 4;

    // Invite-code mode. The menu creates/joins the room over HTTP and stores the
    // credentials here before the game thread starts.
    bool use_signaling = false;
    std::string server_url;
    std::string room_code;
    std::string peer_id;
    std::string peer_token;
    std::string host_peer_id;
    int stun_port = 0;
};

struct PeerStatus {
    int seat = -1;
    std::string role;
    bool connected = false;
    // Received input frame minus the current host frame. Positive means the
    // peer is sending inputs ahead of the host.
    int lead_frames = 0;
    int ms_since_packet = 0;
    uint64_t packets = 0;
    uint64_t substitutions = 0;
};

// Snapshot the menu can display without touching live session state.
struct Status {
    bool enabled = false;
    bool ready = false;
    bool failed = false;
    bool env_configured = false;
    Mode mode = Mode::Off;
    uint8_t seat = 0;
    uint8_t players = 0;
    uint16_t port = 0;
    uint8_t input_delay = 0;
    uint32_t frame = 0;
    int peers = 0;
    std::string room_code;
    std::string host_candidates;
    std::vector<PeerStatus> peer_status;
};

// Experimental direct-UDP input session. The host is authoritative: clients
// send their input a few frames ahead and the host broadcasts the finalized
// pads, so neither side blocks on a single lost packet.
class InputSession {
public:
    static InputSession& instance();

    bool enabled() const;
    bool poll_input();
    bool get_input(int port, uint16_t* buttons, float* x, float* y) const;

    // Applies settings from the menu. Returns false when environment variables
    // are controlling the session or when the session has already started.
    bool configure(const Config& config);
    Status status() const;

    // Fills `error` and returns false when the settings are incomplete.
    static bool validate(const Config& config, std::string& error);

private:
    InputSession();
    ~InputSession();
    InputSession(const InputSession&) = delete;
    InputSession& operator=(const InputSession&) = delete;

    struct Impl;
    Impl* impl_;
};

} // namespace wcw::netplay
