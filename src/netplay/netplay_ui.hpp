#pragma once

#include <string>

namespace wcw::netplay::ui {

// Adds the "Netplay" tab to the shared config modal. Must be called before
// recompui::config::finalize(). The tab is reachable from the launcher's
// "Play Online" entry and, during gameplay, from the Esc / Menu menu.
void register_netplay_ui(std::u8string game_id, std::string mod_game_id);

} // namespace wcw::netplay::ui
