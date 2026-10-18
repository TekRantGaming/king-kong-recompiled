// Performance metrics overlay (owner, 18 Oct: "Show FPS becomes Performance metrics"): the parts that need
// no window, so tests can pin them (kk/tests/perf_overlay_tests.cpp). Drawn by overlay.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace kk::perf {

// kk_performance_metrics: off, fps (frame rate and frame time), full (the whole box).
enum class Level { kOff, kFps, kFull };

inline Level ParseLevel(std::string_view v) { return v == "full" ? Level::kFull : v == "fps" ? Level::kFps : Level::kOff; }
inline const char* LevelValue(Level l) { return l == Level::kFull ? "full" : l == Level::kFps ? "fps" : "off"; }
// F2 while playing: Off, FPS only, Full, Off...
inline Level NextLevel(Level l) { return l == Level::kOff ? Level::kFps : l == Level::kFps ? Level::kFull : Level::kOff; }

// The frame time graph's top (ms): the next of 20, 35, 50, 100, 200 above the slowest frame shown, so a
// steady 60 or 30 FPS line sits in the lower part and a hitch reaches the top without rescaling every frame.
inline float GraphTop(float slowest_ms) {
  for (float top : {20.0f, 35.0f, 50.0f, 100.0f, 200.0f})
    if (slowest_ms <= top) return top;
  return 200.0f;
}

// "1.6" for 1638 MiB in GiB with one decimal (no unit), "-" when unknown.
inline std::string GiB(double mib) {
  if (!(mib >= 0)) return "-";
  char b[16];
  std::snprintf(b, sizeof(b), "%.1f", mib / 1024.0);
  return b;
}
// A whole number ("45"), "-" when unknown.
inline std::string Whole(double v) {
  if (!(v >= 0) || !std::isfinite(v)) return "-";
  char b[16];
  std::snprintf(b, sizeof(b), "%.0f", v);
  return b;
}

// The renderer's upscaler line ("DLSS 310.9.1 Quality preset M (Vulkan)") without the API in brackets,
// which the box shows on its own row; "off" stays as it is.
inline std::string UpscalerShort(std::string_view name) {
  std::string s(name);
  if (const auto at = s.rfind(" ("); at != std::string::npos && !s.empty() && s.back() == ')') s.erase(at);
  return s;
}

// The frame rate's colour band: 0 at or above 55 FPS (green), 1 from 28 (amber), 2 below (red).
inline int FpsBand(double fps) { return fps >= 55 ? 0 : fps >= 28 ? 1 : 2; }

}  // namespace kk::perf
