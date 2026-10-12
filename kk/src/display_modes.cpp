#include "display_modes.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/display_mode.h>

namespace kk {

namespace {

std::string Hz(float refresh) {
  char buf[32];
  // 60, 144; 59.94, 119.88 keep their decimals.
  if (std::fabs(refresh - std::round(refresh)) < 0.05f) std::snprintf(buf, sizeof(buf), "%d", int(std::lround(refresh)));
  else std::snprintf(buf, sizeof(buf), "%.2f", refresh);
  return buf;
}

DisplayMode FromSdk(const rex::ui::DisplayMode& m) { return {m.width, m.height, m.refresh_rate}; }

int Monitor() { return std::atoi(rex::cvar::GetFlagByName("monitor").c_str()); }

}  // namespace

std::string DisplayMode::Value() const {
  return std::to_string(width) + "x" + std::to_string(height) + "@" + Hz(refresh);
}

std::string DisplayMode::Label() const {
  return std::to_string(width) + " \xC3\x97 " + std::to_string(height) + "  \xC2\xB7  " + Hz(refresh) + " Hz";
}

std::string DisplayMode::RateLabel() const { return Hz(refresh) + " Hz"; }

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

std::vector<DisplayMode> ListDisplayModes(int monitor, const rex::ui::Window* window) {
  std::vector<DisplayMode> out;
  for (const rex::ui::DisplayMode& m : rex::ui::GetFullscreenDisplayModes(monitor, window)) {
    const DisplayMode mode = FromSdk(m);
    // Two rates that print the same (59.94 and 59.95) are one choice.
    bool seen = false;
    for (const DisplayMode& o : out) seen = seen || o.Value() == mode.Value();
    if (!seen && mode.width >= 640 && mode.height >= 480) out.push_back(mode);
  }
  std::string list;
  for (const DisplayMode& m : out) list += (list.empty() ? "" : ", ") + m.Value();
  REXLOG_INFO("KK: display {}: {} fullscreen modes: {}", monitor, out.size(), list);
  return out;  // SDL's order: largest first, then the highest refresh rate
}

std::optional<std::pair<int, int>> FullscreenModeSize() {
  if (const auto m = ParseDisplayMode(rex::cvar::GetFlagByName("kk_fullscreen_mode")))
    return std::pair<int, int>{m->width, m->height};
  return std::nullopt;
}

void ApplyFullscreenMode(rex::ui::Window* window) {
  if (!window) {
    REXLOG_WARN("KK: fullscreen mode: no window");
    return;
  }
  const std::string value = rex::cvar::GetFlagByName("kk_fullscreen_mode");
  const auto want = ParseDisplayMode(value);
  if (!want) {
    rex::ui::SetFullscreenDisplayMode(window, 0, 0, 0, 0.0f);  // borderless at the desktop's resolution
    REXLOG_INFO("KK: fullscreen mode: desktop (borderless)");
    return;
  }
  rex::ui::DisplayMode chosen;
  if (!rex::ui::SetFullscreenDisplayMode(window, Monitor(), want->width, want->height, want->refresh, &chosen)) {
    REXLOG_WARN("KK: fullscreen mode {}: not available; desktop instead", value);
    return;
  }
  REXLOG_INFO("KK: fullscreen mode {} (exclusive; asked for {})", FromSdk(chosen).Value(), value);
}

}  // namespace kk
