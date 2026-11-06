#include "netplay_ui.hpp"

#include "netplay_input.hpp"
#include "netplay_signaling.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <json/json.hpp>

#include "recompui/config.h"
#include "recompui/recompui.h"
#include "elements/ui_button.h"
#include "elements/ui_container.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_scroll_container.h"
#include "elements/ui_select.h"
#include "elements/ui_text_input.h"
#include "elements/ui_theme.h"
#include "util/file.h"

#include "librecomp/game.hpp"
#include <ultramodern/ultramodern.hpp>

namespace wcw::netplay::ui {

namespace {

using namespace recompui;

constexpr const char* kTabName = "Netplay";
constexpr const char* kTabId = "netplay";
constexpr int kDefaultPort = 27025;
constexpr int kDefaultStunPort = 8766;
constexpr const char* kDefaultServer = "http://127.0.0.1:8765";

struct Settings {
    int connection = 0; // 0 = invite code, 1 = direct IP
    int mode = 0;       // wcw::netplay::Mode
    int players = 2;
    int port = kDefaultPort;
    int seat = 1;
    int spectators = 0;
    int input_delay = 4;
    std::string host;
    std::string secret;
    std::string server_url = kDefaultServer;
    std::string room_code;
};

Settings g_settings;
bool g_settings_loaded = false;

std::filesystem::path settings_path() {
    return recompui::file::get_app_folder_path() / "netplay.json";
}

void load_settings() {
    if (g_settings_loaded) return;
    g_settings_loaded = true;

    std::ifstream in(settings_path());
    if (!in) return;

    try {
        nlohmann::json json;
        in >> json;
        g_settings.connection = json.value("connection", g_settings.connection);
        g_settings.mode = json.value("mode", g_settings.mode);
        g_settings.players = json.value("players", g_settings.players);
        g_settings.port = json.value("port", g_settings.port);
        g_settings.seat = json.value("seat", g_settings.seat);
        g_settings.spectators = json.value("spectators", g_settings.spectators);
        g_settings.input_delay = json.value("input_delay", g_settings.input_delay);
        g_settings.host = json.value("host", g_settings.host);
        g_settings.secret = json.value("secret", g_settings.secret);
        g_settings.server_url = json.value("server_url", g_settings.server_url);
        g_settings.room_code = json.value("room_code", g_settings.room_code);
    } catch (...) {
        std::fprintf(stderr, "[netplay] could not parse %s; using defaults\n",
            settings_path().string().c_str());
    }
}

void save_settings() {
    nlohmann::json json = {
        {"connection", g_settings.connection},
        {"mode", g_settings.mode},
        {"players", g_settings.players},
        {"port", g_settings.port},
        {"seat", g_settings.seat},
        {"spectators", g_settings.spectators},
        {"input_delay", g_settings.input_delay},
        {"host", g_settings.host},
        {"secret", g_settings.secret},
        {"server_url", g_settings.server_url},
        {"room_code", g_settings.room_code},
    };
    std::ofstream out(settings_path());
    if (!out) {
        std::fprintf(stderr, "[netplay] could not write %s\n", settings_path().string().c_str());
        return;
    }
    out << json.dump(2) << "\n";
}

int parse_int(const std::string& text, int fallback) {
    if (text.empty()) return fallback;
    char* end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || value < 0 || value > 65535) return fallback;
    return static_cast<int>(value);
}

void show_info(const std::string& title, const std::string& message) {
    recompui::open_info_prompt(title, message, "OK", []() {}, recompui::ButtonStyle::Primary);
}

std::string escape_rml(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '&': result += "&amp;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&#39;"; break;
            default: result += c; break;
        }
    }
    return result;
}

// ---- Lobby ---------------------------------------------------------------
// The room is created/joined from the menu, then this worker polls the
// signaling service while the player waits in the launcher. The game is only
// started once the host publishes "start", so the game thread is never blocked
// while people are still joining.

struct LobbySnapshot {
    bool active = false;
    bool is_host = false;
    bool start_requested = false;
    bool failed = false;
    std::string error;
    std::string room_code;
    int joined_players = 0;
    int needed_players = 0;
    int joined_spectators = 0;
    Mode mode = Mode::Host;
};

std::mutex g_lobby_mutex;
LobbySnapshot g_lobby;
std::thread g_lobby_thread;
std::atomic<bool> g_lobby_stop{false};
std::atomic<bool> g_lobby_force_start{false};

void stop_lobby();

LobbySnapshot lobby_snapshot() {
    std::lock_guard lock(g_lobby_mutex);
    return g_lobby;
}

void lobby_set_failed(const std::string& message) {
    std::lock_guard lock(g_lobby_mutex);
    g_lobby.failed = true;
    g_lobby.error = message;
}

void lobby_thread_main(RoomInfo room, Mode mode) {
    SignalingClient client(room.server_url);
    std::string error;
    int cursor = 0;
    int failures = 0;
    std::vector<SignalingPeer> peers;
    auto next_heartbeat = std::chrono::steady_clock::now();

    auto update_peer_counts = [&](const std::vector<SignalingPeer>& list) {
        int players = 0;
        int spectators = 0;
        for (const SignalingPeer& peer : list) {
            if (peer.peer_id == room.peer_id) continue;
            if (peer.role == "player") players++;
            else if (peer.role == "spectator") spectators++;
        }
        std::lock_guard lock(g_lobby_mutex);
        g_lobby.joined_players = players;
        g_lobby.joined_spectators = spectators;
    };

    while (!g_lobby_stop.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_heartbeat) {
            peers.clear();
            if (client.heartbeat(room, &peers, error)) {
                failures = 0;
                update_peer_counts(peers);
            } else {
                failures++;
                if (failures >= 4) {
                    lobby_set_failed(error.empty() ? "lost contact with the signaling server" : error);
                    return;
                }
            }
            const int interval = std::max(5, room.heartbeat_seconds / 2);
            next_heartbeat = now + std::chrono::seconds(interval);
        }

        std::vector<SignalEvent> events;
        int next_cursor = cursor;
        bool peers_changed = false;
        if (client.poll_events(room, next_cursor, 500, events, error)) {
            cursor = next_cursor;
            for (const SignalEvent& event : events) {
                if (event.kind == "signal" && event.type == "start") {
                    std::lock_guard lock(g_lobby_mutex);
                    g_lobby.start_requested = true;
                    return;
                }
                if (event.kind == "room_closed") {
                    lobby_set_failed(event.reason.empty() ? "the room was closed" : event.reason);
                    return;
                }
                if (event.kind == "peer_joined" || event.kind == "peer_left" ||
                    event.kind == "seat_changed") {
                    peers_changed = true;
                }
            }
        }
        if (peers_changed) {
            peers.clear();
            if (client.heartbeat(room, &peers, error)) {
                update_peer_counts(peers);
                next_heartbeat = std::chrono::steady_clock::now() +
                    std::chrono::seconds(std::max(5, room.heartbeat_seconds / 2));
            }
        }

        if (mode == Mode::Host) {
            int joined = 0;
            int needed = 0;
            {
                std::lock_guard lock(g_lobby_mutex);
                joined = g_lobby.joined_players;
                needed = g_lobby.needed_players;
            }
            const bool force = g_lobby_force_start.exchange(false);
            if (force || (needed > 0 && joined >= needed)) {
                std::vector<SignalingPeer> current;
                std::string send_error;
                if (client.heartbeat(room, &current, send_error)) {
                    for (const SignalingPeer& peer : current) {
                        if (peer.peer_id == room.peer_id) continue;
                        std::string ignored;
                        (void)client.signal(room, peer.peer_id, "start", nlohmann::json::object(), ignored);
                    }
                }
                std::lock_guard lock(g_lobby_mutex);
                g_lobby.start_requested = true;
                return;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void start_lobby(const RoomInfo& room, Mode mode) {
    stop_lobby();
    g_lobby_stop.store(false);
    {
        std::lock_guard lock(g_lobby_mutex);
        g_lobby = LobbySnapshot{};
        g_lobby.active = true;
        g_lobby.is_host = mode == Mode::Host;
        g_lobby.mode = mode;
        g_lobby.room_code = room.room_code;
        g_lobby.needed_players = mode == Mode::Host ? std::max(room.max_players - 1, 1) : 0;
    }
    g_lobby_thread = std::thread(lobby_thread_main, room, mode);
}

void stop_lobby() {
    g_lobby_stop.store(true);
    g_lobby_force_start.store(false);
    if (g_lobby_thread.joinable()) {
        g_lobby_thread.join();
    }
    std::lock_guard lock(g_lobby_mutex);
    g_lobby.active = false;
    g_lobby.start_requested = false;
    g_lobby.failed = false;
    g_lobby.error.clear();
}

// Custom page so it can refresh the connection readout every UI update. The
// shared config page helpers only render static option lists.
class NetplayPage : public recompui::Element {
public:
    NetplayPage(ResourceId rid, recompui::Element* parent, std::u8string game_id, std::string mod_game_id)
        : Element(rid, parent, recompui::Events(recompui::EventType::Update), "div", false),
          game_id_(std::move(game_id)),
          mod_game_id_(std::move(mod_game_id)) {
        load_settings();
        build();
        update_visibility();
        queue_update();
    }

protected:
    void process_event(const recompui::Event& e) override {
        if (e.type == recompui::EventType::Update) {
            refresh_status();
            queue_update();
        }
        recompui::Element::process_event(e);
    }

private:
    std::u8string game_id_;
    std::string mod_game_id_;

    TextInput* port_input_ = nullptr;
    TextInput* host_input_ = nullptr;
    TextInput* secret_input_ = nullptr;
    TextInput* spectators_input_ = nullptr;
    TextInput* server_input_ = nullptr;
    TextInput* room_code_input_ = nullptr;
    Select* connection_select_ = nullptr;
    Select* players_select_ = nullptr;
    Select* seat_select_ = nullptr;
    Select* delay_select_ = nullptr;

    Element* server_row_ = nullptr;
    Element* room_row_ = nullptr;
    Element* secret_row_ = nullptr;
    Element* room_actions_row_ = nullptr;
    Element* start_room_row_ = nullptr;
    Element* direct_row_ = nullptr;
    Element* direct_actions_row_ = nullptr;

    Button* start_room_button_ = nullptr;
    Button* cancel_room_button_ = nullptr;
    Label* status_label_ = nullptr;
    std::vector<Button*> action_buttons_;

    bool last_game_running_ = false;
    std::string last_status_text_;
    std::string lobby_error_text_;

    bool has_pending_room_ = false;
    Mode pending_mode_ = Mode::Host;
    RoomInfo pending_room_;

    recompui::Element* add_row(recompui::ContextId context, recompui::Element* parent,
                               const char* text) {
        recompui::Element* row = context.create_element<recompui::Element>(parent, 0, "div", false);
        row->set_display(recompui::Display::Flex);
        row->set_flex_direction(recompui::FlexDirection::Row);
        row->set_align_items(recompui::AlignItems::Center);
        row->set_justify_content(recompui::JustifyContent::SpaceBetween);
        row->set_width(100.0f, recompui::Unit::Percent);
        row->set_gap(24.0f);
        row->set_as_navigation_container(recompui::NavigationType::Horizontal);
        context.create_element<recompui::Label>(row, text, recompui::theme::Typography::LabelMD);
        return row;
    }

    static void set_control_width(recompui::Element* control) {
        control->set_width(360.0f, recompui::Unit::Dp);
        control->set_min_width(200.0f, recompui::Unit::Dp);
    }

    void build() {
        recompui::ContextId context = recompui::get_current_context();

        set_display(recompui::Display::Flex);
        set_flex_direction(recompui::FlexDirection::Column);
        set_width(100.0f, recompui::Unit::Percent);
        set_height(100.0f, recompui::Unit::Percent);

        recompui::ScrollContainer* scroll =
            context.create_element<recompui::ScrollContainer>(this, recompui::ScrollDirection::Vertical);
        scroll->set_flex_grow(1.0f);
        scroll->set_flex_shrink(1.0f);
        scroll->set_flex_basis(100.0f, recompui::Unit::Percent);
        scroll->set_width(100.0f, recompui::Unit::Percent);

        recompui::Element* column =
            context.create_element<recompui::Element>(scroll, 0, "div", false);
        column->set_display(recompui::Display::Flex);
        column->set_flex_direction(recompui::FlexDirection::Column);
        column->set_gap(16.0f);
        column->set_width(100.0f, recompui::Unit::Percent);
        column->set_max_width(860.0f, recompui::Unit::Dp);
        column->set_margin_left_auto();
        column->set_margin_right_auto();
        column->set_padding(24.0f);
        column->set_as_navigation_container(recompui::NavigationType::Vertical);

        context.create_element<recompui::Label>(column, "Play Online",
            recompui::theme::Typography::Header3);
        context.create_element<recompui::Label>(column,
            "Invite-code rooms use the self-hosted signaling service to exchange network "
            "addresses, then play is direct peer-to-peer over UDP. Direct mode needs the "
            "host's reachable IP and an open UDP port. Every player needs the same USA Rev 1 ROM.",
            recompui::theme::Typography::LabelSM);

        status_label_ = context.create_element<recompui::Label>(column, "",
            recompui::theme::Typography::LabelMD);
        status_label_->set_padding(12.0f);
        status_label_->set_border_radius(recompui::theme::border::radius_sm);
        status_label_->set_background_color(recompui::theme::color::Background2);

        // Connection style
        {
            recompui::Element* row = add_row(context, column, "Connection");
            std::vector<recompui::SelectOption> options = {
                {"Invite code (server)", "0"},
                {"Direct IP / LAN", "1"},
            };
            connection_select_ = context.create_element<recompui::Select>(
                row, options, std::to_string(std::clamp(g_settings.connection, 0, 1)));
            set_control_width(connection_select_);
            connection_select_->add_change_callback([this](recompui::SelectOption& option, int) {
                g_settings.connection = parse_int(option.value, 0);
                has_pending_room_ = false;
                if (start_room_button_ != nullptr) start_room_button_->set_enabled(false);
                update_visibility();
            });
        }

        // Online room server
        {
            server_row_ = add_row(context, column, "Signaling server");
            server_input_ = context.create_element<recompui::TextInput>(server_row_);
            set_control_width(server_input_);
            server_input_->set_text(g_settings.server_url);
            server_input_->add_text_changed_callback([](const std::string& text) {
                g_settings.server_url = text;
            });
        }

        // Join room code
        {
            room_row_ = add_row(context, column, "Room code");
            room_code_input_ = context.create_element<recompui::TextInput>(room_row_);
            set_control_width(room_code_input_);
            room_code_input_->set_text(g_settings.room_code);
            room_code_input_->add_text_changed_callback([](const std::string& text) {
                g_settings.room_code = text;
            });
        }

        // Direct host address
        {
            direct_row_ = add_row(context, column, "Host address (direct join)");
            host_input_ = context.create_element<recompui::TextInput>(direct_row_);
            set_control_width(host_input_);
            host_input_->set_text(g_settings.host);
            host_input_->add_text_changed_callback([](const std::string& text) {
                g_settings.host = text;
            });
        }

        // Port
        {
            recompui::Element* row = add_row(context, column, "UDP port");
            port_input_ = context.create_element<recompui::TextInput>(row);
            set_control_width(port_input_);
            port_input_->set_text(std::to_string(g_settings.port));
            port_input_->add_text_changed_callback([](const std::string& text) {
                g_settings.port = parse_int(text, kDefaultPort);
            });
        }

        // Players
        {
            recompui::Element* row = add_row(context, column, "Players in the match");
            std::vector<recompui::SelectOption> options;
            for (int i = 2; i <= 4; i++) {
                options.emplace_back(std::to_string(i) + " players", std::to_string(i));
            }
            players_select_ = context.create_element<recompui::Select>(
                row, options, std::to_string(std::clamp(g_settings.players, 2, 4)));
            set_control_width(players_select_);
            players_select_->add_change_callback([](recompui::SelectOption& option, int) {
                g_settings.players = parse_int(option.value, 2);
            });
        }

        // Direct seat
        {
            recompui::Element* row = add_row(context, column, "Your player seat (direct join)");
            std::vector<recompui::SelectOption> options = {
                {"Player 2", "1"},
                {"Player 3", "2"},
                {"Player 4", "3"},
            };
            seat_select_ = context.create_element<recompui::Select>(
                row, options, std::to_string(std::clamp(g_settings.seat, 1, 3)));
            set_control_width(seat_select_);
            seat_select_->add_change_callback([](recompui::SelectOption& option, int) {
                g_settings.seat = parse_int(option.value, 1);
            });
            seat_row_ = row;
        }

        // Input delay
        {
            recompui::Element* row = add_row(context, column, "Input delay (frames)");
            std::vector<recompui::SelectOption> options;
            for (int i = 0; i <= 8; i++) {
                options.emplace_back(std::to_string(i) + (i == 0 ? " (lowest latency)" : ""), std::to_string(i));
            }
            delay_select_ = context.create_element<recompui::Select>(
                row, options, std::to_string(std::clamp(g_settings.input_delay, 0, 8)));
            set_control_width(delay_select_);
            delay_select_->add_change_callback([](recompui::SelectOption& option, int) {
                g_settings.input_delay = parse_int(option.value, 4);
            });
        }

        // Spectators
        {
            recompui::Element* row = add_row(context, column, "Spectators to wait for (host, direct)");
            spectators_input_ = context.create_element<recompui::TextInput>(row);
            set_control_width(spectators_input_);
            spectators_input_->set_text(std::to_string(g_settings.spectators));
            spectators_input_->add_text_changed_callback([](const std::string& text) {
                g_settings.spectators = std::clamp(parse_int(text, 0), 0, 16);
            });
        }

        // Shared secret (direct mode only; invite rooms get one from the server)
        {
            secret_row_ = add_row(context, column, "Shared secret (16+ characters)");
            secret_input_ = context.create_element<recompui::TextInput>(secret_row_, false);
            set_control_width(secret_input_);
            secret_input_->set_text(g_settings.secret);
            secret_input_->add_text_changed_callback([](const std::string& text) {
                g_settings.secret = text;
            });
        }

        context.create_element<recompui::Label>(column,
            "Invite-code rooms receive an automatic shared secret from the signaling "
            "server. The direct mode secret is typed manually and authenticates "
            "controller packets; it is not encryption and is stored in netplay.json.",
            recompui::theme::Typography::LabelXS);

        // Invite-code actions
        {
            room_actions_row_ = context.create_element<recompui::Element>(column, 0, "div", false);
            room_actions_row_->set_display(recompui::Display::Flex);
            room_actions_row_->set_flex_direction(recompui::FlexDirection::Row);
            room_actions_row_->set_flex_wrap(recompui::FlexWrap::Wrap);
            room_actions_row_->set_gap(12.0f);
            room_actions_row_->set_margin_top(8.0f);

            auto add_room_button = [&](const char* text, recompui::ButtonStyle style, Mode mode) {
                recompui::Button* button = context.create_element<recompui::Button>(
                    room_actions_row_, text, style, recompui::ButtonSize::Medium);
                button->set_min_width(170.0f, recompui::Unit::Dp);
                button->add_pressed_callback([this, mode]() { create_or_join_room(mode); });
                action_buttons_.push_back(button);
            };
            add_room_button("Host & Wait", recompui::ButtonStyle::Primary, Mode::Host);
            add_room_button("Join Game", recompui::ButtonStyle::Success, Mode::Join);
            add_room_button("Spectate", recompui::ButtonStyle::Secondary, Mode::Spectate);
        }

        // Direct actions
        {
            direct_actions_row_ = context.create_element<recompui::Element>(column, 0, "div", false);
            direct_actions_row_->set_display(recompui::Display::Flex);
            direct_actions_row_->set_flex_direction(recompui::FlexDirection::Row);
            direct_actions_row_->set_flex_wrap(recompui::FlexWrap::Wrap);
            direct_actions_row_->set_gap(12.0f);
            direct_actions_row_->set_margin_top(8.0f);

            auto add_direct_button = [&](const char* text, recompui::ButtonStyle style, Mode mode) {
                recompui::Button* button = context.create_element<recompui::Button>(
                    direct_actions_row_, text, style, recompui::ButtonSize::Medium);
                button->set_min_width(150.0f, recompui::Unit::Dp);
                button->add_pressed_callback([this, mode]() { start_direct(mode); });
                action_buttons_.push_back(button);
            };
            add_direct_button("Host Game", recompui::ButtonStyle::Primary, Mode::Host);
            add_direct_button("Join Game", recompui::ButtonStyle::Success, Mode::Join);
            add_direct_button("Spectate", recompui::ButtonStyle::Secondary, Mode::Spectate);
        }

        // Lobby controls and offline start.
        {
            start_room_row_ = context.create_element<recompui::Element>(column, 0, "div", false);
            start_room_row_->set_display(recompui::Display::Flex);
            start_room_row_->set_flex_direction(recompui::FlexDirection::Row);
            start_room_row_->set_flex_wrap(recompui::FlexWrap::Wrap);
            start_room_row_->set_gap(12.0f);
            start_room_row_->set_margin_top(8.0f);

            start_room_button_ = context.create_element<recompui::Button>(
                start_room_row_, "Start Now", recompui::ButtonStyle::Success,
                recompui::ButtonSize::Medium);
            start_room_button_->set_min_width(160.0f, recompui::Unit::Dp);
            start_room_button_->add_pressed_callback([this]() {
                if (lobby_snapshot().is_host) {
                    g_lobby_force_start.store(true);
                }
            });
            start_room_button_->set_enabled(false);
            action_buttons_.push_back(start_room_button_);

            cancel_room_button_ = context.create_element<recompui::Button>(
                start_room_row_, "Cancel Lobby", recompui::ButtonStyle::Danger,
                recompui::ButtonSize::Medium);
            cancel_room_button_->set_min_width(160.0f, recompui::Unit::Dp);
            cancel_room_button_->add_pressed_callback([this]() {
                stop_lobby();
                has_pending_room_ = false;
                lobby_error_text_.clear();
                refresh_status();
            });
            cancel_room_button_->set_enabled(false);
            action_buttons_.push_back(cancel_room_button_);

            recompui::Button* offline = context.create_element<recompui::Button>(
                start_room_row_, "Play Offline", recompui::ButtonStyle::Tertiary,
                recompui::ButtonSize::Medium);
            offline->set_min_width(150.0f, recompui::Unit::Dp);
            offline->add_pressed_callback([this]() { start_offline(); });
            action_buttons_.push_back(offline);
        }
    }

    Element* seat_row_ = nullptr;

    void update_visibility() {
        const bool invite = g_settings.connection == 0;
        auto show = [](Element* element, bool visible) {
            if (element != nullptr) {
                element->set_display(visible ? Display::Flex : Display::None);
            }
        };
        show(server_row_, invite);
        show(room_row_, invite);
        show(room_actions_row_, invite);
        show(secret_row_, !invite);
        show(start_room_row_, true);
        show(direct_row_, !invite);
        show(seat_row_, !invite);
        show(direct_actions_row_, !invite);
    }

    void refresh_status() {
        if (status_label_ == nullptr) return;

        const bool game_running = ultramodern::is_game_started();
        if (game_running != last_game_running_) {
            last_game_running_ = game_running;
            for (recompui::Button* button : action_buttons_) {
                button->set_enabled(!game_running);
            }
        }

        const LobbySnapshot lobby = lobby_snapshot();

        if (!game_running && lobby.start_requested) {
            if (!start_pending_room()) {
                lobby_error_text_ = "Could not start the match. Check the ROM and settings.";
                stop_lobby();
                has_pending_room_ = false;
            } else {
                stop_lobby();
            }
            return;
        }
        if (!game_running && lobby.failed) {
            lobby_error_text_ = lobby.error;
            stop_lobby();
            has_pending_room_ = false;
        }

        if (!game_running) {
            if (start_room_button_ != nullptr) {
                start_room_button_->set_enabled(has_pending_room_ && lobby.active && lobby.is_host);
            }
            if (cancel_room_button_ != nullptr) {
                cancel_room_button_->set_enabled(has_pending_room_ || lobby.active);
            }
        }

        const Status status = InputSession::instance().status();
        std::string text;

        if (game_running) {
            if (!status.enabled) {
                text = "Offline match in progress. Online settings apply when you next start the game.";
            } else if (status.failed) {
                text = "Netplay session failed. Check the log for the reason.";
            } else if (!status.ready) {
                text = status.mode == Mode::Host
                    ? "Starting host session; waiting for players to connect..."
                    : "Connecting to the host...";
            } else {
                text = "Netplay active";
                if (status.mode == Mode::Join) {
                    text += " - seat " + std::to_string(unsigned(status.seat) + 1);
                } else if (status.mode == Mode::Host) {
                    text += " - hosting";
                } else if (status.mode == Mode::Spectate) {
                    text += " - spectating";
                }
                text += ", frame " + std::to_string(status.frame);
                text += ", delay " + std::to_string(status.input_delay) + "f";
            }
        } else if (lobby.active) {
            text = "Room " + escape_rml(lobby.room_code);
            if (lobby.is_host) {
                text += " - waiting for players (" + std::to_string(lobby.joined_players) +
                    "/" + std::to_string(lobby.needed_players) + ").";
                text += "<br/>The match starts automatically when everyone is in. "
                        "Press Start Now to begin early.";
            } else {
                text += " - waiting for the host to start...";
            }
            if (lobby.joined_spectators > 0) {
                text += "<br/>Spectators: " + std::to_string(lobby.joined_spectators);
            }
        } else if (!lobby_error_text_.empty()) {
            text = "Online room error: " + escape_rml(lobby_error_text_);
            text += "<br/>Create or join again, or choose Play Offline.";
        } else if (has_pending_room_) {
            text = "Room " + escape_rml(pending_room_.room_code) + " ready";
            if (pending_mode_ == Mode::Join && pending_room_.player_slot >= 0) {
                text += " - you are player " + std::to_string(pending_room_.player_slot + 1);
            } else if (pending_mode_ == Mode::Spectate) {
                text += " - spectator";
            } else {
                text += " - host";
            }
        } else if (!status.enabled) {
            text = g_settings.connection == 0
                ? "No room yet. Host a room and wait, or join with a code."
                : "Offline. Fill in the settings above, then choose Host Game or Join Game.";
        } else if (status.failed) {
            text = "Last session failed. Check the log for the reason.";
        } else {
            text = "Settings loaded. Choose a room action or Play Offline.";
        }

        if (status.env_configured) {
            text += " (WCW_NETPLAY_* environment variables override this menu.)";
        }

        if (!status.room_code.empty()) {
            text += "<br/>Room code: " + escape_rml(status.room_code);
        }
        if (!status.host_candidates.empty()) {
            text += "<br/>Host reachable at: " + escape_rml(status.host_candidates);
        }
        for (const PeerStatus& peer : status.peer_status) {
            text += "<br/>";
            if (peer.seat >= 0) {
                text += "P" + std::to_string(peer.seat + 1);
            } else {
                text += "Spectator";
            }
            text += " (" + escape_rml(peer.role) + "): ";
            if (!peer.connected) {
                text += "connecting";
                continue;
            }
            text += peer.lead_frames >= 0
                ? "lead +" + std::to_string(peer.lead_frames) + "f"
                : "lag " + std::to_string(-peer.lead_frames) + "f";
            text += ", heard " + std::to_string(peer.ms_since_packet) + "ms ago";
            if (peer.substitutions != 0) {
                text += ", substitutions " + std::to_string(peer.substitutions);
            }
        }

        if (text != last_status_text_) {
            last_status_text_ = text;
            status_label_->set_text_unsafe(text);
        }
    }

    void read_fields() {
        if (port_input_) g_settings.port = std::clamp(parse_int(port_input_->get_text(), kDefaultPort), 1, 65535);
        if (host_input_) g_settings.host = host_input_->get_text();
        if (secret_input_) g_settings.secret = secret_input_->get_text();
        if (spectators_input_) g_settings.spectators = std::clamp(parse_int(spectators_input_->get_text(), 0), 0, 16);
        if (server_input_) g_settings.server_url = server_input_->get_text();
        if (room_code_input_) g_settings.room_code = room_code_input_->get_text();
        if (players_select_) g_settings.players = std::clamp(g_settings.players, 2, 4);
        if (delay_select_) g_settings.input_delay = std::clamp(g_settings.input_delay, 0, 8);
    }

    bool ensure_rom() {
        std::error_code ec;
        std::filesystem::path stored_rom =
            recomp::get_config_path() / std::filesystem::path(game_id_ + u8".z64");
        if (!std::filesystem::exists(stored_rom, ec)) {
            show_info("ROM required",
                "Load the USA Rev 1 ROM from the launcher first (Start Game / Load ROM), "
                "then open Play Online again.");
            return false;
        }
        return true;
    }

    void create_or_join_room(Mode mode) {
        read_fields();
        save_settings();

        // Invite-code rooms receive their HMAC secret from the server, so the
        // typed secret is only needed for direct mode.
        if (g_settings.connection != 0 && g_settings.secret.size() < 16) {
            show_info("Netplay settings", "The shared secret must be at least 16 characters.");
            return;
        }
        if (g_settings.server_url.empty()) {
            show_info("Netplay settings", "Enter the signaling server URL.");
            return;
        }

        SignalingClient client(g_settings.server_url);
        RoomInfo room;
        std::string error;
        bool ok = false;
        if (mode == Mode::Host) {
            ok = client.create_room("Host", std::clamp(g_settings.players, 2, 4), room, error);
        } else {
            if (g_settings.room_code.empty()) {
                show_info("Netplay settings", "Enter the room code from the host.");
                return;
            }
            const std::string role = (mode == Mode::Join) ? "player" : "spectator";
            ok = client.join_room(g_settings.room_code, mode == Mode::Join ? "Player" : "Spectator",
                                  role, room, error);
        }

        if (!ok) {
            show_info("Could not connect to room", error);
            return;
        }

        pending_room_ = room;
        pending_mode_ = mode;
        has_pending_room_ = true;
        if (mode == Mode::Host) {
            g_settings.players = room.max_players;
            g_settings.room_code = room.room_code;
            if (room_code_input_ != nullptr) room_code_input_->set_text(room.room_code);
        } else {
            g_settings.room_code = room.room_code;
            if (room.player_slot >= 0) g_settings.seat = static_cast<uint8_t>(room.player_slot);
        }
        save_settings();
        std::fprintf(stderr, "[netplay] room %s ready (%s, slot %d, stun %d, auto-secret=%s)\n",
            room.room_code.c_str(), room.role.c_str(), room.player_slot, room.stun_port,
            room.room_secret.empty() ? "no" : "yes");

        lobby_error_text_.clear();
        start_lobby(room, mode);
        refresh_status();
    }

    bool start_pending_room() {
        if (!has_pending_room_) return false;
        if (!ensure_rom()) return false;

        Config config;
        config.use_signaling = true;
        config.mode = pending_mode_;
        config.server_url = pending_room_.server_url;
        config.room_code = pending_room_.room_code;
        config.peer_id = pending_room_.peer_id;
        config.peer_token = pending_room_.peer_token;
        config.host_peer_id = pending_room_.host_peer_id;
        config.stun_port = pending_room_.stun_port;
        config.players = static_cast<uint8_t>(std::clamp(
            pending_mode_ == Mode::Host ? pending_room_.max_players : std::max(pending_room_.max_players, 2), 2, 4));
        config.seat = pending_mode_ == Mode::Join ? static_cast<uint8_t>(std::max(pending_room_.player_slot, 1)) : 1;
        config.input_delay = static_cast<uint8_t>(std::clamp(g_settings.input_delay, 0, 8));
        config.port = static_cast<uint16_t>(std::clamp(g_settings.port, 1, 65535));
        config.spectators = static_cast<uint8_t>(std::clamp(g_settings.spectators, 0, 16));
        config.secret = !pending_room_.room_secret.empty() ? pending_room_.room_secret : g_settings.secret;
        return start_with_config(config);
    }

    void start_direct(Mode mode) {
        stop_lobby();
        has_pending_room_ = false;
        read_fields();
        save_settings();
        if (!ensure_rom()) return;

        Config config;
        config.use_signaling = false;
        config.mode = mode;
        config.host = g_settings.host;
        config.port = static_cast<uint16_t>(std::clamp(g_settings.port, 1, 65535));
        config.players = static_cast<uint8_t>(std::clamp(g_settings.players, 2, 4));
        config.seat = static_cast<uint8_t>(std::clamp(g_settings.seat, 1, 3));
        config.spectators = static_cast<uint8_t>(std::clamp(g_settings.spectators, 0, 16));
        config.input_delay = static_cast<uint8_t>(std::clamp(g_settings.input_delay, 0, 8));
        config.secret = g_settings.secret;
        start_with_config(config);
    }

    void start_offline() {
        stop_lobby();
        has_pending_room_ = false;
        lobby_error_text_.clear();
        read_fields();
        g_settings.mode = static_cast<int>(Mode::Off);
        save_settings();

        Config config;
        config.mode = Mode::Off;
        start_with_config(config);
    }

    bool start_with_config(const Config& config) {
        std::string error;
        if (!InputSession::validate(config, error)) {
            show_info("Netplay settings", error);
            return false;
        }

        if (!InputSession::instance().configure(config)) {
            show_info("Netplay settings",
                "The session could not be configured. Close and relaunch the game, or unset "
                "WCW_NETPLAY_* environment variables.");
            return false;
        }

        if (ultramodern::is_game_started()) {
            refresh_status();
            return false;
        }

        g_settings.mode = static_cast<int>(config.mode);
        save_settings();
        has_pending_room_ = false;

        std::u8string game_id = game_id_;
        recompui::update_game_mod_id(mod_game_id_);
        recompui::hide_all_contexts();
        recomp::start_game(game_id, "");
        return true;
    }
};


} // namespace

void register_netplay_ui(std::u8string game_id, std::string mod_game_id) {
    load_settings();

    recompui::config::create_tab(
        kTabName,
        kTabId,
        [game_id, mod_game_id](recompui::ContextId context, recompui::Element* parent) {
            context.create_element<NetplayPage>(parent, game_id, mod_game_id);
        }
    );
}

} // namespace wcw::netplay::ui
