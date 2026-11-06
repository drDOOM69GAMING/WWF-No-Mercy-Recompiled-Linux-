#include "netplay_signaling.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#if defined(__linux__)
#include <curl/curl.h>
#endif

using nlohmann::json;

namespace wcw::netplay {

namespace {

#if defined(__linux__)

std::once_flag curl_init_flag;

bool ensure_curl() {
    std::call_once(curl_init_flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    return true;
}

struct HttpResult {
    long status = 0;
    std::string body;
    std::string transport_error;
};

size_t write_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(data, size * count);
    return size * count;
}

HttpResult http_request(const std::string& method, const std::string& url,
                        const std::vector<std::string>& headers,
                        const std::string& body, long timeout_ms) {
    HttpResult result;
    if (!ensure_curl()) {
        result.transport_error = "could not initialize libcurl";
        return result;
    }

    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        result.transport_error = "could not create a curl handle";
        return result;
    }

    curl_slist* header_list = nullptr;
    for (const std::string& header : headers) {
        header_list = curl_slist_append(header_list, header.c_str());
    }

    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, std::min<long>(timeout_ms, 5000));
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "NoMercyRecompiled-netplay/1");
    if (header_list != nullptr) {
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, header_list);
    }
    if (!body.empty() || method == "POST") {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    CURLcode code = curl_easy_perform(handle);
    if (code != CURLE_OK) {
        result.transport_error = curl_easy_strerror(code);
    } else {
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
    }

    if (header_list != nullptr) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(handle);
    return result;
}

std::vector<std::string> auth_headers(const RoomInfo& room, bool json_body) {
    std::vector<std::string> headers;
    if (json_body) {
        headers.emplace_back("Content-Type: application/json");
    }
    if (!room.peer_id.empty()) {
        headers.emplace_back("X-Peer-ID: " + room.peer_id);
    }
    if (!room.peer_token.empty()) {
        headers.emplace_back("Authorization: Bearer " + room.peer_token);
    }
    return headers;
}

bool response_json(const HttpResult& result, json& out, std::string& error,
                   long expected_status) {
    if (!result.transport_error.empty()) {
        error = "server unreachable: " + result.transport_error;
        return false;
    }

    json parsed;
    if (!result.body.empty()) {
        try {
            parsed = json::parse(result.body);
        } catch (...) {
            parsed = json::object();
        }
    }

    if (result.status != expected_status && !(expected_status == 0 && result.status >= 200 && result.status < 300)) {
        if (parsed.contains("error") && parsed["error"].is_string()) {
            error = parsed["error"].get<std::string>();
        } else {
            error = "server returned HTTP " + std::to_string(result.status);
        }
        return false;
    }

    out = std::move(parsed);
    return true;
}

SignalingPeer parse_peer(const json& value) {
    SignalingPeer peer;
    peer.peer_id = value.value("peer_id", "");
    peer.name = value.value("name", "");
    peer.role = value.value("role", "");
    peer.player_slot = value.value("player_slot", -1);
    peer.queued_for_player = value.value("queued_for_player", false);
    return peer;
}

void parse_room(const json& value, RoomInfo& out) {
    out.room_code = value.value("room_code", out.room_code);
    out.peer_id = value.value("peer_id", out.peer_id);
    out.peer_token = value.value("peer_token", out.peer_token);
    out.role = value.value("role", out.role);
    out.player_slot = value.value("player_slot", out.player_slot);
    out.host_peer_id = value.value("host_peer_id", out.host_peer_id);
    out.room_secret = value.value("room_secret", out.room_secret);
    out.max_peers = value.value("max_peers", out.max_peers);
    out.max_players = value.value("max_players", out.max_players);
    out.stun_port = value.value("stun_port", out.stun_port);
    out.heartbeat_seconds = value.value("heartbeat_seconds", out.heartbeat_seconds);
    out.peers.clear();
    if (value.contains("peers") && value["peers"].is_array()) {
        for (const json& peer : value["peers"]) {
            out.peers.push_back(parse_peer(peer));
        }
    }
}

#else

struct HttpResult {
    long status = 0;
    std::string body;
    std::string transport_error;
};

#endif

} // namespace

SignalingClient::SignalingClient(std::string server_url) {
    std::string host;
    uint16_t port = 0;
    std::string base;
    if (split_server_url(server_url, base, host, port)) {
        base_url_ = std::move(base);
    } else {
        base_url_ = std::move(server_url);
        while (!base_url_.empty() && base_url_.back() == '/') {
            base_url_.pop_back();
        }
    }
}

bool SignalingClient::split_server_url(const std::string& url, std::string& base_url,
                                       std::string& host, uint16_t& port) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    std::string scheme = url.substr(0, scheme_end);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (scheme != "http" && scheme != "https") {
        return false;
    }

    std::string rest = url.substr(scheme_end + 3);
    const size_t path_start = rest.find('/');
    if (path_start != std::string::npos) {
        rest = rest.substr(0, path_start);
    }
    if (rest.empty()) {
        return false;
    }

    std::string host_part = rest;
    uint16_t default_port = (scheme == "https") ? 443 : 80;
    port = default_port;

    if (host_part.front() == '[') {
        // IPv6 literal. Keep the brackets for the URL, strip them for the host.
        const size_t close = host_part.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host = host_part.substr(1, close - 1);
        if (close + 1 < host_part.size() && host_part[close + 1] == ':') {
            const long parsed = std::strtol(host_part.c_str() + close + 2, nullptr, 10);
            if (parsed > 0 && parsed <= 65535) {
                port = static_cast<uint16_t>(parsed);
            }
        }
    } else {
        const size_t colon = host_part.rfind(':');
        if (colon != std::string::npos && host_part.find(':') == colon) {
            host = host_part.substr(0, colon);
            const long parsed = std::strtol(host_part.c_str() + colon + 1, nullptr, 10);
            if (parsed > 0 && parsed <= 65535) {
                port = static_cast<uint16_t>(parsed);
            }
        } else {
            host = host_part;
        }
    }

    if (host.empty()) {
        return false;
    }
    base_url = scheme + "://" + host_part;
    return true;
}

bool SignalingClient::create_room(const std::string& name, int max_players,
                                  RoomInfo& out, std::string& error) {
#if defined(__linux__)
    json body = {{"name", name}, {"max_players", max_players}};
    HttpResult result = http_request(
        "POST", base_url_ + "/v1/rooms",
        {"Content-Type: application/json"}, body.dump(), 8000);
    json parsed;
    if (!response_json(result, parsed, error, 201)) {
        return false;
    }
    out = RoomInfo{};
    out.server_url = base_url_;
    parse_room(parsed, out);
    return true;
#else
    (void)name; (void)max_players; (void)out;
    error = "online rooms are currently Linux-only";
    return false;
#endif
}

bool SignalingClient::join_room(const std::string& code, const std::string& name,
                                const std::string& role, RoomInfo& out,
                                std::string& error) {
#if defined(__linux__)
    json body = {{"name", name}, {"role", role}};
    HttpResult result = http_request(
        "POST", base_url_ + "/v1/rooms/" + code + "/join",
        {"Content-Type: application/json"}, body.dump(), 8000);
    json parsed;
    if (!response_json(result, parsed, error, 201)) {
        return false;
    }
    out = RoomInfo{};
    out.server_url = base_url_;
    parse_room(parsed, out);
    return true;
#else
    (void)code; (void)name; (void)role; (void)out;
    error = "online rooms are currently Linux-only";
    return false;
#endif
}

bool SignalingClient::signal(const RoomInfo& room, const std::string& to_peer_id,
                             const std::string& type, const nlohmann::json& data,
                             std::string& error) {
#if defined(__linux__)
    json body = {
        {"to_peer_id", to_peer_id},
        {"type", type},
        {"data", data},
    };
    HttpResult result = http_request(
        "POST", base_url_ + "/v1/rooms/" + room.room_code + "/signal",
        auth_headers(room, true), body.dump(), 8000);
    json parsed;
    return response_json(result, parsed, error, 202);
#else
    (void)room; (void)to_peer_id; (void)type; (void)data;
    error = "online rooms are currently Linux-only";
    return false;
#endif
}

bool SignalingClient::poll_events(const RoomInfo& room, int& cursor, int wait_ms,
                                  std::vector<SignalEvent>& out, std::string& error) {
#if defined(__linux__)
    wait_ms = std::clamp(wait_ms, 0, 15000);
    const std::string url = base_url_ + "/v1/rooms/" + room.room_code +
        "/events?after=" + std::to_string(cursor) + "&wait_ms=" + std::to_string(wait_ms);
    HttpResult result = http_request("GET", url, auth_headers(room, false), "", wait_ms + 5000);
    json parsed;
    if (!response_json(result, parsed, error, 200)) {
        if (result.status == 409 && parsed.contains("cursor")) {
            cursor = parsed.value("cursor", cursor);
        }
        return false;
    }

    out.clear();
    if (parsed.contains("events") && parsed["events"].is_array()) {
        for (const json& item : parsed["events"]) {
            SignalEvent event;
            event.cursor = item.value("cursor", 0);
            event.kind = item.value("kind", "");
            event.peer_id = item.value("peer_id", "");
            event.reason = item.value("reason", "");
            event.from_peer_id = item.value("from_peer_id", "");
            event.type = item.value("type", "");
            if (item.contains("data") && item["data"].is_object()) {
                event.data = item["data"];
            }
            if (item.contains("peer") && item["peer"].is_object()) {
                event.peer = parse_peer(item["peer"]);
            }
            out.push_back(std::move(event));
        }
    }
    cursor = parsed.value("cursor", cursor);
    return true;
#else
    (void)room; (void)cursor; (void)wait_ms; (void)out;
    error = "online rooms are currently Linux-only";
    return false;
#endif
}

bool SignalingClient::heartbeat(const RoomInfo& room,
                                std::vector<SignalingPeer>* peers,
                                std::string& error) {
#if defined(__linux__)
    HttpResult result = http_request(
        "POST", base_url_ + "/v1/rooms/" + room.room_code + "/heartbeat",
        auth_headers(room, true), "{}", 8000);
    json parsed;
    if (!response_json(result, parsed, error, 200)) {
        return false;
    }
    if (peers != nullptr) {
        peers->clear();
        if (parsed.contains("peers") && parsed["peers"].is_array()) {
            for (const json& peer : parsed["peers"]) {
                peers->push_back(parse_peer(peer));
            }
        }
    }
    return true;
#else
    (void)room; (void)peers;
    error = "online rooms are currently Linux-only";
    return false;
#endif
}

bool SignalingClient::leave(const RoomInfo& room, std::string& error) {
#if defined(__linux__)
    HttpResult result = http_request(
        "POST", base_url_ + "/v1/rooms/" + room.room_code + "/leave",
        auth_headers(room, true), "{}", 8000);
    json parsed;
    return response_json(result, parsed, error, 200);
#else
    (void)room;
    error = "online rooms are currently Linux-only";
    return false;
#endif
}

} // namespace wcw::netplay
