// King Kong native renderer: the render scale setting (--native_render_scale).
//
// "1" (default): the 360's size. "1.5", "2", "3", any number from 0.25 to 8: both axes by that
// factor. "1920x1080": a frame of that size (each axis by size / guest size, so a window with
// another aspect ratio stretches). Anything else is refused and the scale stays 1.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace nr {

struct RenderScale {
  float x = 1.0f, y = 1.0f;
  bool valid = true;  // false: the text was not understood (x and y are 1)
  bool unit() const { return x == 1.0f && y == 1.0f; }
};

constexpr float kMinRenderScale = 0.25f;
constexpr float kMaxRenderScale = 8.0f;

inline RenderScale ParseRenderScale(const std::string& text, uint32_t guest_width, uint32_t guest_height) {
  RenderScale out;
  auto number = [](const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(v);
  };
  auto clamp = [](double v) { return float(std::clamp(v, double(kMinRenderScale), double(kMaxRenderScale))); };
  std::string s;
  for (char c : text)
    if (c != ' ') s += char(std::tolower(static_cast<unsigned char>(c)));
  if (s.empty()) return out;
  const size_t cross = s.find('x');
  double a = 0, b = 0;
  if (cross == std::string::npos) {
    if (!number(s, a) || a <= 0) return {1.0f, 1.0f, false};
    out.x = out.y = clamp(a);
  } else {
    if (!number(s.substr(0, cross), a) || !number(s.substr(cross + 1), b) || a < 1 || b < 1 || guest_width == 0 ||
        guest_height == 0)
      return {1.0f, 1.0f, false};
    out.x = clamp(a / double(guest_width));
    out.y = clamp(b / double(guest_height));
  }
  // Values within a thousandth of 1 are 1: the 1:1 path must be taken exactly.
  if (std::fabs(out.x - 1.0f) < 1e-3f) out.x = 1.0f;
  if (std::fabs(out.y - 1.0f) < 1e-3f) out.y = 1.0f;
  return out;
}

// The size of something of `guest` pixels at scale s: the same rounding everywhere (targets,
// textures, rectangles, the frame image).
inline uint32_t ScaledSize(uint32_t guest, float s) {
  return s == 1.0f ? guest : uint32_t(std::max<long>(std::lround(double(guest) * s), 1));
}

}  // namespace nr
