// The pause menu's kept frame (pause_hooks.cpp), task 264: when the game pauses it dims its world smoothly to
// about 0.4 times play's brightness (settled after about 1.2 s; nearly linear per channel; under its black cover,
// so it shows only with the cover left out, in photo mode too; not motion blur, the HUD's screen list, the
// renderer's GI or upscaler, nor a post pass of its own: same draws as play). A frame taken again while paused
// (a field of view change) is brought back to play's brightness with a per-channel curve measured once a pause:
// the last frame of play against the paused frame at the same field of view (the world stands still, so the
// pixels pair up; the HUD and rain are few and the medians ignore them).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kk::play {

using BrightnessLut = std::array<std::array<uint8_t, 256>, 3>;

// `play` and `paused`: RGBA, same size. For each channel and each paused value, the median play value of the
// pixels with it (every second pixel and row); values seen too rarely are filled in between their neighbours,
// above the brightest one by its ratio; the curve never goes down. False when too few values were seen.
inline bool BuildBrightnessLut(const uint8_t* play, const uint8_t* paused, int width, int height, BrightnessLut& lut) {
  if (!play || !paused || width <= 0 || height <= 0) return false;
  constexpr uint32_t kMinCount = 64;
  std::vector<uint32_t> hist(3 * 256 * 256, 0);  // [channel][paused][play]
  for (int y = 0; y < height; y += 2) {
    const size_t row = size_t(y) * size_t(width) * 4;
    for (int x = 0; x < width; x += 2) {
      const size_t i = row + size_t(x) * 4;
      for (int c = 0; c < 3; ++c) ++hist[(size_t(c) * 256 + paused[i + c]) * 256 + play[i + c]];
    }
  }
  for (int c = 0; c < 3; ++c) {
    std::array<int, 256> v;
    v.fill(-1);
    int known = 0, top = -1;
    for (int d = 0; d < 256; ++d) {
      const uint32_t* h = &hist[(size_t(c) * 256 + d) * 256];
      uint32_t n = 0;
      for (int p = 0; p < 256; ++p) n += h[p];
      if (n < kMinCount) continue;
      uint32_t half = (n + 1) / 2, seen = 0;
      int p = 0;
      for (; p < 255 && seen + h[p] < half; ++p) seen += h[p];
      v[d] = p;
      ++known, top = d;
    }
    if (known < 8) return false;
    if (v[0] < 0) v[0] = 0;
    int last = 0;
    for (int d = 1; d <= top; ++d) {
      if (v[d] < 0) continue;
      for (int k = last + 1; k < d; ++k) v[k] = v[last] + (v[d] - v[last]) * (k - last) / (d - last);
      last = d;
    }
    const double ratio = top > 0 ? double(v[top]) / top : 1.0;
    for (int d = top + 1; d < 256; ++d) v[d] = int(d * ratio + 0.5);
    int floor = 0;
    for (int d = 0; d < 256; ++d) {
      floor = std::max(floor, std::clamp(v[d], 0, 255));
      lut[size_t(c)][size_t(d)] = uint8_t(floor);
    }
  }
  return true;
}

inline void ApplyBrightnessLut(const BrightnessLut& lut, uint8_t* rgba, size_t pixels) {
  for (size_t i = 0; i < pixels; ++i, rgba += 4)
    for (int c = 0; c < 3; ++c) rgba[c] = lut[size_t(c)][rgba[c]];
}

// The curve's gain at mid grey (for the log).
inline double BrightnessLutGain(const BrightnessLut& lut) {
  return (lut[0][64] + lut[1][64] + lut[2][64]) / (3.0 * 64.0);
}

}  // namespace kk::play
