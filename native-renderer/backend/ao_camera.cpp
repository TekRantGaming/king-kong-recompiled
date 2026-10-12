// The camera for the ambient occlusion: see ao_camera.h.

#include "backend/ao_camera.h"

#include <algorithm>
#include <cmath>

#include "backend/draw_state.h"
#include "backend/guest_memory.h"

namespace nr {

namespace {
float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool Near(float x, float y, float relative, float absolute) {
  return std::fabs(x - y) <= std::max(absolute, relative * std::max(std::fabs(x), std::fabs(y)));
}
}  // namespace

bool ExtractAoProjection(const float m[16], AoProjection& out) {
  if (!LooksLikePerspectiveRows(m)) return false;
  const float* x = m;
  const float* y = m + 4;
  const float* z = m + 8;
  const float* w = m + 12;
  const float ww = Dot3(w, w);
  AoProjection p;
  p.cx = Dot3(x, w) / ww;
  p.cy = Dot3(y, w) / ww;
  const float px2 = Dot3(x, x) / ww - p.cx * p.cx, py2 = Dot3(y, y) / ww - p.cy * p.cy;
  if (!(px2 > 0.0f) || !(py2 > 0.0f)) return false;
  p.px = std::sqrt(px2);
  p.py = std::sqrt(py2);
  p.a = Dot3(z, w) / ww;
  p.b = z[3] - p.a * w[3];
  if (!std::isfinite(p.px) || !std::isfinite(p.py) || !std::isfinite(p.a) || !std::isfinite(p.b) || !p.valid())
    return false;
  out = p;
  return true;
}

int FindAoPerspectiveRegister(const uint8_t* be, float rows[16]) {
  if (!be) return -1;
  for (uint32_t r = 0; r + 4 <= 256; ++r) {
    float m[16];
    for (uint32_t i = 0; i < 16; ++i) m[i] = LoadBEFloat(be + (r * 4 + i) * 4);
    if (LooksLikePerspectiveRows(m)) {
      for (uint32_t i = 0; i < 16; ++i) rows[i] = m[i];
      return int(r);
    }
  }
  return -1;
}

AoViewMapping ComputeAoViewMapping(const AoProjection& p, const AoScreen& s, int32_t origin_x, int32_t origin_y,
                                   uint32_t padded_w, uint32_t padded_h, uint32_t step) {
  // Working pixel i is host pixel origin + step * i, whose centre is at host coordinate
  // origin + step * i + 0.5 = origin + 0.5 - step / 2 + step * W * uv. In guest pixels that is
  // divided by the render scale, and the viewport transform gives NDC:
  //   guest = ndc * scale + offset + (window and half-pixel offsets).
  // View x = (ndc - cx) / px * view z, view y likewise (y up, as XeGTAO has it).
  AoViewMapping m;
  const float k = float(step);
  auto axis = [&](float host_scale, float vp_scale, float vp_offset, float extra, float projection, float centre,
                  int32_t origin, uint32_t padded, float& mul, float& add) {
    const float hs = host_scale > 0.0f ? host_scale : 1.0f;
    const float ss = vp_scale != 0.0f ? vp_scale : 1.0f;
    mul = k * float(padded) / (hs * ss * projection);
    add = (((float(origin) + 0.5f - 0.5f * k) / hs - vp_offset - extra) / ss - centre) / projection;
  };
  axis(s.host_scale_x, s.x_scale, s.x_offset, s.offset_x(), p.px, p.cx, origin_x, padded_w, m.mul[0], m.add[0]);
  axis(s.host_scale_y, s.y_scale, s.y_offset, s.offset_y(), p.py, p.cy, origin_y, padded_h, m.mul[1], m.add[1]);
  // Stored depth = z_scale * (a + b / view z) + z_offset.
  m.depth_mul = -p.b * s.z_scale;
  m.depth_add = s.z_scale * p.a + s.z_offset;
  return m;
}

bool AoCameraFinder::Same(const Candidate& c, const AoProjection& p, const AoScreen& s) {
  return Near(c.p.px, p.px, 0.005f, 1e-6f) && Near(c.p.py, p.py, 0.005f, 1e-6f) && Near(c.p.cx, p.cx, 0.0f, 0.005f) &&
         Near(c.p.cy, p.cy, 0.0f, 0.005f) && Near(c.p.a, p.a, 0.005f, 1e-6f) && Near(c.p.b, p.b, 0.005f, 1e-6f) &&
         c.s.x_scale == s.x_scale && c.s.x_offset == s.x_offset && c.s.y_scale == s.y_scale &&
         c.s.y_offset == s.y_offset && c.s.z_scale == s.z_scale && c.s.z_offset == s.z_offset &&
         c.s.window_x == s.window_x && c.s.window_y == s.window_y && c.s.half_pixel == s.half_pixel &&
         c.s.host_scale_x == s.host_scale_x &&
         c.s.host_scale_y == s.host_scale_y;
}

void AoCameraFinder::Offer(uint64_t frame, const AoProjection& p, const AoScreen& s) {
  if (frame != frame_) {
    // Keep the last frame's winner for a frame without candidates.
    if (used_) {
      const Candidate* best = &candidates_[0];
      for (uint32_t i = 1; i < used_; ++i)
        if (candidates_[i].count > best->count) best = &candidates_[i];
      previous_ = *best;
      have_previous_ = true;
    }
    frame_ = frame;
    used_ = offers_ = 0;
  }
  ++offers_;
  for (uint32_t i = 0; i < used_; ++i) {
    if (Same(candidates_[i], p, s)) {
      ++candidates_[i].count;
      return;
    }
  }
  if (used_ < candidates_.size()) candidates_[used_++] = {p, s, 1};
}

bool AoCameraFinder::Get(uint64_t frame, AoProjection& p, AoScreen& s) const {
  if (frame == frame_ && used_) {
    const Candidate* best = &candidates_[0];
    for (uint32_t i = 1; i < used_; ++i)
      if (candidates_[i].count > best->count) best = &candidates_[i];
    p = best->p;
    s = best->s;
    return true;
  }
  if (frame != frame_ && used_) {
    // The frame changed and nothing was offered yet: the last frame's winner is the current set.
    const Candidate* best = &candidates_[0];
    for (uint32_t i = 1; i < used_; ++i)
      if (candidates_[i].count > best->count) best = &candidates_[i];
    p = best->p;
    s = best->s;
    return true;
  }
  if (have_previous_) {
    p = previous_.p;
    s = previous_.s;
    return true;
  }
  return false;
}

}  // namespace nr
