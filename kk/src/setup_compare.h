// The setup graphics pages' comparison pictures (owner, 18 Oct: nothing game-derived in git or in our downloads,
// so no screenshots ship any more): made at run time from a still of the player's own copy of the game (the
// launcher's movie stills or chapter pictures, launcher_art.h) for the options whose effect can be shown on a
// still picture: drawn with fewer pixels, sharpened, jagged or smooth edges, and a floor with plain or 16x
// texture filtering. They are illustrations of what each option does, not captures of the game with it on.
// Pure functions on art::Image (RGBA8), checked by tests/hdr_tests.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "art.h"

namespace kk::compare {

using art::Image;

namespace detail {

inline Image Blank(int w, int h) {
  Image im;
  im.width = std::max(1, w);
  im.height = std::max(1, h);
  im.rgba.assign(size_t(im.width) * size_t(im.height) * 4, 255);
  return im;
}

inline const uint8_t* At(const Image& im, int x, int y) {
  return &im.rgba[(size_t(y) * size_t(im.width) + size_t(x)) * 4];
}

// Bilinear sample at texel coordinates (texel centres at .5), clamped or wrapped at the edges.
inline void Bilinear(const Image& im, float x, float y, bool wrap, float out[4]) {
  x -= 0.5f;
  y -= 0.5f;
  const float fx = std::floor(x), fy = std::floor(y);
  const float tx = x - fx, ty = y - fy;
  auto cx = [&](int v) {
    if (!wrap) return std::clamp(v, 0, im.width - 1);
    v %= im.width;
    return v < 0 ? v + im.width : v;
  };
  auto cy = [&](int v) {
    if (!wrap) return std::clamp(v, 0, im.height - 1);
    v %= im.height;
    return v < 0 ? v + im.height : v;
  };
  const int x0 = cx(int(fx)), x1 = cx(int(fx) + 1), y0 = cy(int(fy)), y1 = cy(int(fy) + 1);
  const uint8_t *a = At(im, x0, y0), *b = At(im, x1, y0), *c = At(im, x0, y1), *d = At(im, x1, y1);
  for (int k = 0; k < 4; ++k)
    out[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
}

inline uint8_t Byte(float v) { return uint8_t(std::clamp(int(std::lround(v)), 0, 255)); }

}  // namespace detail

// Resized to w x h: averaged over each pixel's area when made smaller, bilinear when made larger.
inline Image Resize(const Image& src, int w, int h) {
  Image out = detail::Blank(w, h);
  if (!src) return out;
  const float sx = float(src.width) / float(out.width), sy = float(src.height) / float(out.height);
  for (int y = 0; y < out.height; ++y)
    for (int x = 0; x < out.width; ++x) {
      float acc[4] = {0, 0, 0, 0};
      if (sx <= 1.0f && sy <= 1.0f) {
        detail::Bilinear(src, (x + 0.5f) * sx, (y + 0.5f) * sy, false, acc);
      } else {
        const int x0 = int(x * sx), x1 = std::max(x0 + 1, std::min(src.width, int(std::ceil((x + 1) * sx))));
        const int y0 = int(y * sy), y1 = std::max(y0 + 1, std::min(src.height, int(std::ceil((y + 1) * sy))));
        for (int v = y0; v < y1; ++v)
          for (int u = x0; u < x1; ++u)
            for (int k = 0; k < 4; ++k) acc[k] += detail::At(src, u, v)[k];
        const float n = float((x1 - x0) * (y1 - y0));
        for (float& a : acc) a /= n;
      }
      uint8_t* p = &out.rgba[(size_t(y) * size_t(out.width) + size_t(x)) * 4];
      for (int k = 0; k < 4; ++k) p[k] = detail::Byte(acc[k]);
    }
  return out;
}

// The middle of the picture with the w:h shape, resized to w x h.
inline Image Fit(const Image& src, int w, int h) {
  if (!src) return detail::Blank(w, h);
  int cw = src.width, ch = int(std::lround(float(src.width) * float(h) / float(w)));
  if (ch > src.height) {
    ch = src.height;
    cw = int(std::lround(float(src.height) * float(w) / float(h)));
  }
  Image crop = detail::Blank(cw, ch);
  const int ox = (src.width - cw) / 2, oy = (src.height - ch) / 2;
  for (int y = 0; y < ch; ++y)
    std::copy_n(detail::At(src, ox, oy + y), size_t(cw) * 4, &crop.rgba[size_t(y) * size_t(cw) * 4]);
  return Resize(crop, w, h);
}

// Drawn with fewer pixels (1/factor across) and scaled up plainly, as the game looks without an upscaler at a
// lower render size.
inline Image FewerPixels(const Image& src, float factor) {
  const int w = std::max(1, int(std::lround(src.width / factor))), h = std::max(1, int(std::lround(src.height / factor)));
  return Resize(Resize(src, w, h), src.width, src.height);
}

// Sharpened: each pixel pushed away from the average of its four neighbours (an unsharp mask).
inline Image Sharpen(const Image& src, float amount) {
  Image out = src;
  for (int y = 0; y < src.height; ++y)
    for (int x = 0; x < src.width; ++x) {
      const uint8_t* c = detail::At(src, x, y);
      const uint8_t* l = detail::At(src, std::max(0, x - 1), y);
      const uint8_t* r = detail::At(src, std::min(src.width - 1, x + 1), y);
      const uint8_t* u = detail::At(src, x, std::max(0, y - 1));
      const uint8_t* d = detail::At(src, x, std::min(src.height - 1, y + 1));
      uint8_t* p = &out.rgba[(size_t(y) * size_t(src.width) + size_t(x)) * 4];
      for (int k = 0; k < 3; ++k) p[k] = detail::Byte(c[k] + amount * (c[k] - (l[k] + r[k] + u[k] + d[k]) * 0.25f));
    }
  return out;
}

// Edges up close: the picture drawn with big pixels (`pixel` screen pixels each, from `step` source pixels),
// each from one sample (jagged, as without anti-aliasing) or the average of its area (smooth edges).
inline Image Edges(const Image& src, int w, int h, int pixel, int step, bool smooth) {
  Image out = detail::Blank(w, h);
  if (!src) return out;
  const int cols = (w + pixel - 1) / pixel, rows = (h + pixel - 1) / pixel;
  const int ox = std::max(0, (src.width - cols * step) / 2), oy = std::max(0, (src.height - rows * step) / 2);
  for (int by = 0; by < rows; ++by)
    for (int bx = 0; bx < cols; ++bx) {
      float acc[4] = {0, 0, 0, 0};
      const int sx = std::min(src.width - 1, ox + bx * step), sy = std::min(src.height - 1, oy + by * step);
      if (smooth) {
        int n = 0;
        for (int v = 0; v < step; ++v)
          for (int u = 0; u < step; ++u, ++n)
            for (int k = 0; k < 4; ++k)
              acc[k] += detail::At(src, std::min(src.width - 1, sx + u), std::min(src.height - 1, sy + v))[k];
        for (float& a : acc) a /= float(n);
      } else {
        const uint8_t* c = detail::At(src, std::min(src.width - 1, sx + step / 2), std::min(src.height - 1, sy + step / 2));
        for (int k = 0; k < 4; ++k) acc[k] = c[k];
      }
      for (int y = by * pixel; y < std::min(h, (by + 1) * pixel); ++y)
        for (int x = bx * pixel; x < std::min(w, (bx + 1) * pixel); ++x) {
          uint8_t* p = &out.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
          for (int k = 0; k < 4; ++k) p[k] = detail::Byte(acc[k]);
        }
    }
  return out;
}

// A floor covered with the picture (repeated), going into the distance, as a GPU draws it: with plain trilinear
// filtering (the far floor blurs) or with 16x anisotropic filtering (it stays sharp at the steep angle).
inline Image Ground(const Image& tex, int w, int h, bool anisotropic) {
  Image out = detail::Blank(w, h);
  if (!tex) return out;
  std::vector<Image> mips{tex};
  while (mips.back().width > 1 || mips.back().height > 1)
    mips.push_back(Resize(mips.back(), std::max(1, mips.back().width / 2), std::max(1, mips.back().height / 2)));
  const Image& top = mips.back();
  const float horizon = h * 0.22f, f = w * 0.8f, height = 1.0f, repeat = 3.0f;  // floor units per picture
  auto trilinear = [&](float u, float v, float lod, float out_px[4]) {
    lod = std::clamp(lod, 0.0f, float(mips.size() - 1));
    const int l0 = int(lod), l1 = std::min(l0 + 1, int(mips.size()) - 1);
    const float t = lod - float(l0);
    float a[4], b[4];
    detail::Bilinear(mips[size_t(l0)], u * mips[size_t(l0)].width, v * mips[size_t(l0)].height, true, a);
    detail::Bilinear(mips[size_t(l1)], u * mips[size_t(l1)].width, v * mips[size_t(l1)].height, true, b);
    for (int k = 0; k < 4; ++k) out_px[k] = a[k] * (1 - t) + b[k] * t;
  };
  for (int y = 0; y < h; ++y) {
    const float dy = float(y) + 0.5f - horizon;
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &out.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
      if (dy < 1.0f) {  // the sky: the picture's average colour, darker towards the top
        const float k = 0.35f + 0.4f * float(y) / std::max(1.0f, horizon);
        for (int c = 0; c < 3; ++c) p[c] = detail::Byte(top.rgba[size_t(c)] * k);
        p[3] = 255;
        continue;
      }
      const float dist = height * f / dy;
      const float u = (float(x) + 0.5f - w * 0.5f) / f * dist / repeat, v = dist / repeat;
      // How far one screen pixel moves on the picture, across (u) and down the screen (v), in texels.
      const float du = dist / f / repeat * float(tex.width), dv = height * f / (dy * dy) / repeat * float(tex.height);
      const float major = std::max(du, dv), minor = std::max(1e-6f, std::min(du, dv));
      float px[4] = {0, 0, 0, 0};
      if (!anisotropic) {
        trilinear(u, v, std::log2(std::max(1.0f, major)), px);
      } else {
        const float ratio = std::min(16.0f, major / minor);
        const int taps = std::max(1, int(std::ceil(ratio)));
        const float lod = std::log2(std::max(1.0f, major / ratio));
        const bool along_v = dv >= du;
        for (int i = 0; i < taps; ++i) {
          const float o = (float(i) + 0.5f) / float(taps) - 0.5f;  // across the pixel's footprint
          float s[4];
          trilinear(along_v ? u : u + o * du / float(tex.width), along_v ? v + o * dv / float(tex.height) : v, lod, s);
          for (int k = 0; k < 4; ++k) px[k] += s[k] / float(taps);
        }
      }
      for (int c = 0; c < 3; ++c) p[c] = detail::Byte(px[c]);
      p[3] = 255;
    }
  }
  return out;
}

}  // namespace kk::compare
