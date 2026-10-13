// The port's pause menu (docs/menus.md, "In game: the pause menu"): drawn in the launcher's style
// over the frozen, dimmed game frame while play::PauseMenuOpen(). Resume, Start again, Score,
// Options and Quit game run the game's own items (play::RunPauseAction); Settings shows the
// launcher's pages with only the rows that change live (ingame_settings.h).

#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace kk {

// Creates the pause menu for the rest of the session; later calls do nothing. `config_path`: the
// settings file (Settings saves to it); `user_dir`, `game_dir`: the user data and game files (the
// button pictures, the logo and the menu sounds).
void CreatePauseMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                     const std::filesystem::path& config_path, const std::filesystem::path& user_dir,
                     const std::filesystem::path& game_dir);

// The game side, while the pause menu is open: the controller as the game read it (XInput's buttons,
// left stick and triggers), which the game itself is then not given. Any thread.
void PauseMenuPad(uint16_t buttons, int16_t lx, int16_t ly, uint8_t lt, uint8_t rt);

}  // namespace kk
