// Pre-game launcher: installs the game files and edits video, graphics,
// gameplay and control settings before the runtime starts (shown from
// OnFinalizePaths). Uses art from the player's own game files.

#pragma once

#include <filesystem>
#include <functional>
#include <utility>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class ImmediateDrawer;
}

namespace kk {

struct LauncherCallbacks {
  std::function<void()> play;                // start the game (called deferred, UI thread)
  std::function<void()> restart_and_play;    // relaunch with saved settings, skip launcher
  std::function<void()> quit;
  std::function<void(bool)> set_fullscreen;  // apply window mode live
  std::function<double()> dpi_scale;         // window DPI / 96
  std::function<std::pair<int, int>()> screen_size;  // monitor size in physical pixels
  std::function<std::pair<int, int>()> output_size;  // size the game will be shown at
};

struct LauncherPaths {
  std::filesystem::path game_dir;
  std::filesystem::path user_dir;
  std::filesystem::path config_path;
};

// True when the game files needed to boot exist under `game_dir`.
bool GameFilesPresent(const std::filesystem::path& game_dir);

// Loads the GPU plugin early so its cvars (resolution scale, FXAA, ...) are
// registered before the config is read and can be edited by the launcher.
void PreloadGpuPlugin();

// Creates the launcher dialog; it deletes itself when closed.
void ShowLauncher(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                  LauncherPaths paths, LauncherCallbacks callbacks);

}  // namespace kk
