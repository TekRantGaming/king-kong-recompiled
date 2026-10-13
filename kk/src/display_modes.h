// Fullscreen resolution (kk_fullscreen_mode): "desktop", a borderless window at the desktop's
// resolution (the SDK's own fullscreen), or one of the monitor's display modes, for exclusive
// fullscreen at that size and refresh rate.
//
// The SDK's window goes fullscreen with SDL_SetWindowFullscreen, and SDL keeps a fullscreen mode
// per window that it uses whenever that window is fullscreen. The port sets it through the SDK
// (rex::ui::SetFullscreenDisplayMode, tools/rexglue-patches 0015) before the game starts; every
// later fullscreen switch of the SDK's (the setting, the launcher's Window mode) then uses it. The
// port can't call SDL for this itself: SDL is linked into both rexruntime.dll and king_kong.exe, and
// the port's copy sees neither the SDK's window nor the displays.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform.h"

namespace rex::ui {
class Window;
}

namespace kk {

struct DisplayMode {
  int width = 0, height = 0;  // pixels
  float refresh = 0;          // Hz (0: unknown)
  std::string Value() const;  // "2560x1440@144" (kk_fullscreen_mode)
  std::string Label() const;  // "2560 x 1440  .  144 Hz"
  std::string RateLabel() const;  // "144 Hz"
};

// "2560x1440@144" -> the mode; nullopt for "desktop" or anything it can't read.
std::optional<DisplayMode> ParseDisplayMode(const std::string& value);

// The display modes of the display the game opens on (the monitor setting: 1-based, in SDL's
// order as the SDK uses it; 0: the display `window` is on), largest first, without duplicates.
// UI thread.
std::vector<DisplayMode> ListDisplayModes(int monitor, const rex::ui::Window* window);

// The display's mode on the desktop (width 0 when unknown); `monitor` as above. UI thread.
DisplayMode DesktopDisplayMode(int monitor, const rex::ui::Window* window);

// Every display, in the order the monitor setting counts them (the SDK's; monitor = index + 1), so the Monitor
// row names the display the game opens on and whose modes Resolution lists. UI thread.
std::vector<MonitorInfo> ListDisplays();

// The size the game is shown at in fullscreen when kk_fullscreen_mode is a mode (nullopt for desktop).
std::optional<std::pair<int, int>> FullscreenModeSize();

// Sets the game window's fullscreen mode from kk_fullscreen_mode (UI thread, before the game
// starts). Logs what it chose; a mode the display doesn't have falls back to the closest one, or to
// desktop.
void ApplyFullscreenMode(rex::ui::Window* window);

}  // namespace kk
