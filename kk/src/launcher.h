// Pre-game launcher: installs the game files and edits video, graphics,
// gameplay and control settings before the runtime starts (shown from
// OnFinalizePaths). Uses art from the player's own game files.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <rex/ui/imgui_dialog.h>

#include "display_modes.h"

namespace rex::ui {
class ImmediateDrawer;
}

namespace kk {

struct LauncherCallbacks {
  std::function<void()> play;                // start the game (called deferred, UI thread)
  std::function<void()> restart_and_play;    // relaunch with saved settings, skip launcher
  std::function<void()> benchmark;           // relaunch the fixed replay with private user data
  std::function<void()> quit;
  // Task 330 (menu_scene.h): start the game's start menu scene behind the launcher (deferred, UI thread).
  std::function<void()> start_scene;
  // Starts the launcher again as a new process with these arguments, then this one closes.
  std::function<void(const std::wstring&)> restart_launcher;
  std::function<void(bool)> set_fullscreen;  // apply window mode live
  std::function<double()> dpi_scale;         // window DPI / 96
  std::function<std::pair<int, int>()> screen_size;  // monitor size in physical pixels
  std::function<std::pair<int, int>()> output_size;  // size the game will be shown at
  std::function<std::vector<DisplayMode>(int)> display_modes;  // a monitor's fullscreen modes (display_modes.h)
  std::function<DisplayMode(int)> desktop_mode;                // a monitor's mode on the desktop
  // The launcher's heartbeat (task 173): its setup steps and jobs go on while nothing is drawn (window
  // minimised, or the presenter lost the window's surface). post_to_ui runs a function on the UI thread
  // soon (any thread); recover_presentation (UI thread) reconnects the presenter to the window when the
  // window can be drawn again.
  std::function<void(std::function<void()>)> post_to_ui;
  std::function<void()> recover_presentation;
  // Job J3, notifications when a download ends: the window's {minimised, active} state, and restore
  // (a click on a notification: the window comes back and is asked to the front).
  std::function<std::pair<bool, bool>()> window_state;
  std::function<void()> restore;
};

struct LauncherPaths {
  std::filesystem::path game_dir;
  std::filesystem::path user_dir;
  std::filesystem::path config_path;
};

// Whether a window (the SDK's native handle: an HWND on Windows) can be drawn to now: shown, not
// minimised, some client area. True where the platform is not asked (the presenter checks the size).
bool NativeWindowDrawable(void* handle);
// {minimised, active (has the keyboard focus)} for a window (the SDK's native handle). Linux: SDL's windows.
std::pair<bool, bool> NativeWindowState(void* handle);
// Task 229: the window in front now, for the log: "no window", or its class and program file name (never
// a window title or a path). Empty off Windows.
std::string ForegroundWindowOwner();
// Restores a minimised window and asks for it to be the active window. Linux: SDL's windows.
bool RestoreNativeWindow(void* handle);

// True when the game files needed to boot exist under `game_dir`.
bool GameFilesPresent(const std::filesystem::path& game_dir);

// Loads the GPU plugin early so its cvars (resolution scale, FXAA, ...) are
// registered before the config is read and can be edited by the launcher.
void PreloadGpuPlugin();

// The native renderer's plugin was found and loaded by PreloadGpuPlugin.
bool NativeRendererAvailable();

// Selects the native plugin, normalizes legacy config, then applies startup recovery.
std::string ApplyRendererChoice();

// Creates the launcher dialog; it deletes itself when closed.
void ShowLauncher(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                  LauncherPaths paths, LauncherCallbacks callbacks);

}  // namespace kk
