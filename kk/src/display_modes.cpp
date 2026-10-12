#include "display_modes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace kk {

namespace {

std::string Hz(float refresh) {
  char buf[32];
  // 60, 144; 59.94, 119.88 keep their decimals.
  if (std::fabs(refresh - std::round(refresh)) < 0.05f) std::snprintf(buf, sizeof(buf), "%d", int(std::lround(refresh)));
  else std::snprintf(buf, sizeof(buf), "%.2f", refresh);
  return buf;
}

DisplayMode FromSdl(const SDL_DisplayMode& m) {
  const float density = m.pixel_density > 0 ? m.pixel_density : 1.0f;
  return {int(std::lround(m.w * density)), int(std::lround(m.h * density)), m.refresh_rate};
}

SDL_DisplayID DisplayFor(int monitor, SDL_Window* window) {
  SDL_DisplayID id = 0;
  if (monitor > 0) {
    int count = 0;
    if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
      if (monitor <= count) id = ids[monitor - 1];
      SDL_free(ids);
    }
  }
  if (!id && window) id = SDL_GetDisplayForWindow(window);
  if (!id) id = SDL_GetPrimaryDisplay();
  return id;
}

SDL_Window* GameWindow(void* native_window) {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (!windows) return nullptr;
  SDL_Window* found = count > 0 ? windows[0] : nullptr;
#if defined(_WIN32)
  for (int i = 0; native_window && i < count; ++i) {
    if (SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) ==
        native_window) {
      found = windows[i];
      break;
    }
  }
#else
  (void)native_window;
#endif
  SDL_free(windows);
  return found;
}

}  // namespace

std::string DisplayMode::Value() const {
  return std::to_string(width) + "x" + std::to_string(height) + "@" + Hz(refresh);
}

std::string DisplayMode::Label() const {
  return std::to_string(width) + " \xC3\x97 " + std::to_string(height) + "  \xC2\xB7  " + Hz(refresh) + " Hz";
}

std::optional<DisplayMode> ParseDisplayMode(const std::string& value) {
  DisplayMode m;
  const size_t x = value.find('x'), at = value.find('@');
  if (x == std::string::npos) return std::nullopt;
  m.width = std::atoi(value.c_str());
  m.height = std::atoi(value.c_str() + x + 1);
  if (at != std::string::npos) m.refresh = float(std::atof(value.c_str() + at + 1));
  if (m.width < 320 || m.height < 200 || m.width > 16384 || m.height > 16384) return std::nullopt;
  return m;
}

std::vector<DisplayMode> ListDisplayModes(int monitor) {
  std::vector<DisplayMode> out;
  const SDL_DisplayID display = DisplayFor(monitor, GameWindow(nullptr));
  int count = 0;
  SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
  if (!modes) return out;
  for (int i = 0; i < count; ++i) {
    const DisplayMode m = FromSdl(*modes[i]);
    // SDL lists the same size and rate again for other pixel formats.
    const bool seen = std::any_of(out.begin(), out.end(), [&](const DisplayMode& o) {
      return o.width == m.width && o.height == m.height && o.Value() == m.Value();
    });
    if (!seen && m.width >= 640 && m.height >= 480) out.push_back(m);
  }
  SDL_free(modes);
  return out;  // SDL's order: largest first, then the highest refresh rate
}

std::optional<std::pair<int, int>> FullscreenModeSize() {
  if (const auto m = ParseDisplayMode(rex::cvar::GetFlagByName("kk_fullscreen_mode")))
    return std::pair<int, int>{m->width, m->height};
  return std::nullopt;
}

void ApplyFullscreenMode(void* native_window) {
  SDL_Window* window = GameWindow(native_window);
  if (!window) {
    REXLOG_WARN("KK: fullscreen mode: the game's window was not found");
    return;
  }
  const std::string value = rex::cvar::GetFlagByName("kk_fullscreen_mode");
  const auto want = ParseDisplayMode(value);
  if (!want) {
    SDL_SetWindowFullscreenMode(window, nullptr);  // borderless at the desktop's resolution
    REXLOG_INFO("KK: fullscreen mode: desktop (borderless)");
    return;
  }
  const SDL_DisplayID display = DisplayFor(std::atoi(rex::cvar::GetFlagByName("monitor").c_str()), window);
  SDL_DisplayMode mode{};
  // Sizes are in SDL's units: pixels on Windows and X11 (density 1), points where the density is higher.
  const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
  const float density = desktop && desktop->pixel_density > 0 ? desktop->pixel_density : 1.0f;
  if (!SDL_GetClosestFullscreenDisplayMode(display, int(std::lround(want->width / density)),
                                           int(std::lround(want->height / density)), want->refresh, true, &mode)) {
    SDL_SetWindowFullscreenMode(window, nullptr);
    REXLOG_WARN("KK: fullscreen mode {}: the display has no such mode ({}); desktop instead", value, SDL_GetError());
    return;
  }
  if (!SDL_SetWindowFullscreenMode(window, &mode)) {
    REXLOG_WARN("KK: fullscreen mode {}: {}; desktop instead", value, SDL_GetError());
    SDL_SetWindowFullscreenMode(window, nullptr);
    return;
  }
  REXLOG_INFO("KK: fullscreen mode {} (exclusive; asked for {}){}", FromSdl(mode).Value(), value,
              (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) ? "" : ", used when the window goes fullscreen");
}

}  // namespace kk
