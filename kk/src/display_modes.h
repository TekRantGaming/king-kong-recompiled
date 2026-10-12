// Fullscreen resolution (kk_fullscreen_mode): "desktop", a borderless window at the desktop's
// resolution (the SDK's own fullscreen), or one of the monitor's display modes, for exclusive
// fullscreen at that size and refresh rate.
//
// The SDK's window (rex::ui::WindowSDL) goes fullscreen with SDL_SetWindowFullscreen and never
// sets a display mode, so it gets SDL3's borderless desktop fullscreen. SDL keeps a fullscreen
// mode per window and uses it whenever that window is fullscreen, so the port sets it on the
// game's window (SDL_SetWindowFullscreenMode) before the game starts; every later fullscreen
// switch of the SDK's (the setting, the launcher's Window mode) then uses it. No SDK change.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kk {

// NOT READY, off for the first beta (native-renderer/docs/beta.md, "3. Display > Resolution"). SDL is
// linked statically into both rexruntime.dll and king_kong.exe, so the port's SDL copy sees neither the
// SDK's window nor (its video never initialised) the displays: the list is empty and the mode can't be
// set. It needs a small SDK addition (the window setting its own fullscreen mode). While this is
// false the Display page has no Resolution row and nothing here runs.
constexpr bool kFullscreenResolution = false;

struct DisplayMode {
  int width = 0, height = 0;  // pixels
  float refresh = 0;          // Hz (0: unknown)
  std::string Value() const;  // "2560x1440@144" (kk_fullscreen_mode)
  std::string Label() const;  // "2560 x 1440  .  144 Hz"
};

// "2560x1440@144" -> the mode; nullopt for "desktop" or anything it can't read.
std::optional<DisplayMode> ParseDisplayMode(const std::string& value);

// The display modes of the display the game opens on (the monitor setting: 1-based, in SDL's
// order as the SDK uses it; 0: the display the window is on), largest first, without duplicates.
std::vector<DisplayMode> ListDisplayModes(int monitor);

// The size the game is shown at in fullscreen when kk_fullscreen_mode is a mode (nullopt for desktop).
std::optional<std::pair<int, int>> FullscreenModeSize();

// Sets the game window's fullscreen mode from kk_fullscreen_mode (call on the UI thread before the
// game starts). `native_window`: the window's HWND on Windows (null elsewhere: SDL's first window).
// Logs what it chose; a mode the display doesn't have falls back to the closest one, or to desktop.
void ApplyFullscreenMode(void* native_window);

}  // namespace kk
