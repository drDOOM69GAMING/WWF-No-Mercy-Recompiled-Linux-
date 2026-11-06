#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <json/json.hpp>

namespace wcw::netplay {

struct SignalingPeer {
    std::string peer_id;
    std::string name;
    std::string role;
    int player_slot = -1;
    bool queued_for_player = false;
};

struct RoomInfo {
    std::string server_url;
    std::string room_code;
    std::string peer_id;
    std::string peer_token;
    std::string host_peer_id;
    std::string room_secret;
    std::string role;
    int player_slot = -1;
    int max_peers = 0;
    int max_players = 0;
    int stun_port = 0;
    int heartbeat_seconds = 20;
    std::vector<SignalingPeer> peers;
};

struct SignalEvent {
    int cursor = 0;
    std::string kind;
    std::string peer_id;
    std::string reason;
    // kind == "signal"
    std::string from_peer_id;
    std::string type;
    nlohmann::json data;
    // kind == "peer_joined"
    SignalingPeer peer;
};

// Tiny blocking HTTP client for the self-hostable signaling service. All calls
// have short timeouts and never touch gameplay traffic.
class SignalingClient {
public:
    explicit SignalingClient(std::string server_url);

    const std::string& server_url() const { return base_url_; }

    // Splits http(s)://host[:port][/path] into a normalized base URL, host and
    // port. Returns false when the URL cannot be parsed.
    static bool split_server_url(const std::string& url, std::string& base_url,
                                 std::string& host, uint16_t& port);

    bool create_room(const std::string& name, int max_players, RoomInfo& out,
                     std::string& error);
    bool join_room(const std::string& code, const std::string& name,
                   const std::string& role, RoomInfo& out, std::string& error);
    bool signal(const RoomInfo& room, const std::string& to_peer_id,
                const std::string& type, const nlohmann::json& data,
                std::string& error);
    bool poll_events(const RoomInfo& room, int& cursor, int wait_ms,
                     std::vector<SignalEvent>& out, std::string& error);
    bool heartbeat(const RoomInfo& room, std::vector<SignalingPeer>* peers,
                   std::string& error);
    bool leave(const RoomInfo& room, std::string& error);

private:
    std::string base_url_;
};

} // namespace wcw::netplay
