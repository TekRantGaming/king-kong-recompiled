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
#include <cstdio>
#include <optional>
#include <string>
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

// Task 363 (Beta 17): ultrawide sizes in the resolution lists, so a wider picture can be chosen on any screen
// (kk_picture_size). Real 21:9 and 32:9 monitors' sizes, smallest first. 7680x2160 is the largest: no listed size
// goes past it, so the frame never grows past twice a 4K screen's pixels (the renderer's own limit is 10240x5760).
struct WideSize {
  int w, h;
  const char* shape;  // "21:9" or "32:9" (the name monitors are sold by)
};
inline constexpr WideSize kWideSizes[] = {{2560, 1080, "21:9"}, {3440, 1440, "21:9"}, {3840, 1080, "32:9"},
                                          {3840, 1600, "21:9"}, {5120, 1440, "32:9"}, {5120, 2160, "21:9"},
                                          {7680, 2160, "32:9"}};
constexpr int kMaxPictureWidth = 7680, kMaxPictureHeight = 2160;

// A size is offered on a screen at least as tall: a taller picture is only scaled down to fit it, so 5120x2160 and
// 7680x2160 need a 4K screen (and the GPU that drives one).
inline bool WideSizeOffered(int h, int screen_h) { return h <= screen_h; }

// "WxH" as a kk_picture_size: only a size wider than 16:9 and no larger than 7680x2160 counts ("" or anything
// else: the picture follows the window or screen, as before task 363).
inline std::optional<std::pair<int, int>> ParsePictureSize(std::string_view value) {
  int w = 0, h = 0;
  char tail = 0;
  if (std::sscanf(std::string(value).c_str(), "%dx%d%c", &w, &h, &tail) != 2) return std::nullopt;
  if (w < 640 || h < 270 || w > kMaxPictureWidth || h > kMaxPictureHeight) return std::nullopt;
  if (WidthFactor(double(w) / double(h)) == 1.0) return std::nullopt;
  return std::pair<int, int>{w, h};
}

// The window for a picture size in windowed mode: the picture's own size when it fits in 90% of the screen (room for
// the frame and the taskbar), else the largest of its shape that does. Even sizes.
inline std::pair<int, int> FitWindow(int pw, int ph, int screen_w, int screen_h) {
  const int room_w = screen_w * 9 / 10, room_h = screen_h * 9 / 10;
  if (pw <= room_w && ph <= room_h) return {pw, ph};
  auto [w, h] = LargestInside(room_w, room_h, double(pw) / double(ph));
  return {std::max(2, w & ~1), std::max(2, h & ~1)};
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
