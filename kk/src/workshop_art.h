// Task 221: Workshop card pictures made at run time from a still of the player's own copy of the game (the
// launcher's movie stills or chapter pictures), like the setup's close-ups (setup_compare.h): nothing
// game-derived is in git or in our downloads. They illustrate what a built-in mod or the HD texture pack does;
// they are not captures of the game with it on. Pure functions on art::Image (RGBA8), checked by
// tests/texture_packs_tests.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "setup_compare.h"

namespace kk::workshop_art {

using art::Image;

// The Cel shading mod's look: the picture softened a little, its light flattened into `bands` steps (hue kept)
// and dark ink outlines where the brightness changes sharply (a Sobel edge), as the mod's screen effect draws them.
inline Image CelShade(const Image& src, int bands = 4, float edge = 48.0f) {
  Image out = src;
  if (!src) return out;
  const int w = src.width, h = src.height;
  // A 3x3 box blur first, so noise and fine texture do not turn into specks of ink or colour.
  std::vector<float> soft(size_t(w) * size_t(h) * 3), lum(size_t(w) * size_t(h));
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float acc[3] = {0, 0, 0};
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          const uint8_t* c = compare::detail::At(src, std::clamp(x + dx, 0, w - 1), std::clamp(y + dy, 0, h - 1));
          for (int k = 0; k < 3; ++k) acc[k] += c[k];
        }
      float* s = &soft[(size_t(y) * w + x) * 3];
      for (int k = 0; k < 3; ++k) s[k] = acc[k] / 9.0f;
      lum[size_t(y) * w + x] = 0.2126f * s[0] + 0.7152f * s[1] + 0.0722f * s[2];
    }
  auto L = [&](int x, int y) { return lum[size_t(std::clamp(y, 0, h - 1)) * w + size_t(std::clamp(x, 0, w - 1))]; };
  const float step = 255.0f / float(bands);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float gx = L(x + 1, y - 1) + 2 * L(x + 1, y) + L(x + 1, y + 1) - L(x - 1, y - 1) - 2 * L(x - 1, y) - L(x - 1, y + 1);
      const float gy = L(x - 1, y + 1) + 2 * L(x, y + 1) + L(x + 1, y + 1) - L(x - 1, y - 1) - 2 * L(x, y - 1) - L(x + 1, y - 1);
      const float g = std::sqrt(gx * gx + gy * gy);
      const float l = L(x, y);
      // The band's light level (the middle of its step); the colour scaled to it, within limits so dark noise is
      // never blown up into strong colour.
      const float level = std::min(255.0f, (std::floor(l / step) + 0.5f) * step);
      const float scale = l > 8.0f ? std::clamp(level / l, 0.6f, 1.6f) : 0.6f;
      const float ink = std::clamp((g - edge) / edge, 0.0f, 1.0f);
      const float* s = &soft[(size_t(y) * w + x) * 3];
      uint8_t* p = &out.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
      for (int k = 0; k < 3; ++k) p[k] = compare::detail::Byte(std::min(255.0f, s[k] * scale) * (1.0f - 0.88f * ink) + 12.0f * ink);
    }
  return out;
}

// The HD texture pack: the left part as the original textures look up close (fewer pixels, scaled up plainly),
// the right part with four times the detail (sharpened), a thin light line between them.
inline Image HdSplit(const Image& still) {
  if (!still) return still;
  const Image soft = compare::FewerPixels(still, 2.5f), sharp = compare::Sharpen(still, 0.9f);
  Image out = sharp;
  const int w = still.width, h = still.height;
  for (int y = 0; y < h; ++y) {
    // A slanted divider: from 58% at the top to 42% at the bottom.
    const float split = w * (0.58f - 0.16f * float(y) / float(std::max(1, h - 1)));
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &out.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
      const float d = float(x) - split;
      if (d < -1.0f) std::copy_n(compare::detail::At(soft, x, y), 4, p);
      else if (d <= 1.0f)
        for (int k = 0; k < 3; ++k) p[k] = 235;
    }
  }
  return out;
}

}  // namespace kk::workshop_art
