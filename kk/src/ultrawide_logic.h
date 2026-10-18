// Experimental ultrawide (task 223, the built-in Ultrawide mod; DECISIONS 18 Oct): the pure maths, no game state.
//
// kk_aspect picks the picture's shape: "16:9" (the default: nothing changes anywhere), "21:9", "32:9" or "auto"
// (the window's or screen's own shape, at least 16:9 and at most 32:9). The renderer then draws a picture of
// that shape (UpscalerOutputSize / NoUpscalerFrameSize, settings.cpp) while the game still thinks in its own
// 1280x720, and the game's 3D projections are made A = aspect / (16:9) times wider (hor+: the same height,
// more on the sides; ultrawide.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace kk::ultrawide {

constexpr double kGameAspect = 16.0 / 9.0;
constexpr double kMaxAspect = 32.0 / 9.0;

// The picture's shape for a kk_aspect value and the chosen resolution (w x h). Anything unknown is 16:9.
inline double AspectFor(std::string_view setting, int w, int h) {
  if (setting == "21:9") return 21.0 / 9.0;
  if (setting == "32:9") return kMaxAspect;
  if (setting == "auto" && w > 0 && h > 0) return std::clamp(double(w) / double(h), kGameAspect, kMaxAspect);
  return kGameAspect;
}

// How many times wider than the game's own 16:9 (1 = unchanged). Within a hair of 16:9 counts as 16:9.
inline double WidthFactor(double aspect) {
  const double a = aspect / kGameAspect;
  return a < 1.0005 ? 1.0 : std::min(a, kMaxAspect / kGameAspect);
}

// The largest picture of that shape inside w x h; rounding never goes past w or h.
inline std::pair<int, int> LargestInside(int w, int h, double aspect) {
  if (double(w) >= double(h) * aspect) return {std::min(w, int(std::lround(double(h) * aspect))), h};
  return {w, std::min(h, int(std::lround(double(w) / aspect)))};
}

// Hor+ on one projection matrix (16 floats, Direct3D row-vector layout as the game pushes it): x is divided by
// the factor (column 0), so the same height shows more on the sides. Only a perspective projection (m[11] != 0,
// m[15] == 0) is changed; returns whether it was. An orthographic one (HUD, menus, full-screen passes) stays.
inline bool WidenProjection(float* m, float factor) {
  if (factor == 1.0f || m[11] == 0.0f || m[15] != 0.0f) return false;
  for (int i : {0, 4, 8, 12}) m[i] /= factor;
  return true;
}

}  // namespace kk::ultrawide
