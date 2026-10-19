#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace kk::coop::nametag {
using Matrix = std::array<float, 16>;  // Guest Direct3D row-vector layout.
struct Point { float x = 0, y = 0, z = 0; };
struct Rect { float x = 0, y = 0, w = 0, h = 0; };
struct Camera {
  Matrix view{}, projection{};
  uint32_t level = 0;
  double time = 0;
};
struct Projection {
  bool on_screen = false, behind = false;
  float x = 0, y = 0;  // Screen point, or a clamped edge point.
  float dx = 0, dy = 1;  // Unit screen direction for the edge arrow.
};
inline constexpr float kAboveHead = .3f;
// Human rigs are already validated by the body service. Select their highest
// central joint, rather than assigning anatomical meaning to an unproved tag 20.
// This remains a geometric head estimate, bounded to reject arms/held objects.
inline void ConsiderHead(Point root, Point bone, std::optional<Point>& highest) {
  for (float f : {root.x,root.y,root.z,bone.x,bone.y,bone.z}) if (!std::isfinite(f)) return;
  const float height = bone.z-root.z;
  if (std::hypot(bone.x-root.x,bone.y-root.y) > .35f || height < 1.2f || height > 2.6f) return;
  if (!highest || bone.z > highest->z) highest = bone;
}
inline Point AboveHead(Point root, const std::optional<Point>& head) {
  const auto p = head.value_or(Point{root.x,root.y,root.z+1.8f});
  return {p.x,p.y,p.z+kAboveHead};
}
// Measurement and projection must both be in ImGui logical coordinates. The
// caller measures at the very same font size passed to AddText; DPI is applied
// once by ImGui's renderer. Only picture-edge clipping may move the centre.
inline Rect TextBounds(const Projection& p, float width, float height, Rect picture, float inset) {
  const float left = picture.x+inset;
  const float right = std::max(left,picture.x+picture.w-width-inset);
  return {std::clamp(p.x-width*.5f,left,right),p.y-height,width,height};
}
inline bool Valid(const Camera& c) {
  for (const auto& m : {c.view, c.projection})
    for (float f : m) if (!std::isfinite(f)) return false;
  return c.view[15] == 1 && c.projection[15] == 0 && c.projection[11] > 0 &&
      std::abs(c.projection[0]) > .001f && std::abs(c.projection[5]) > .001f;
}
// Same fit as present_letterbox, in UI display coordinates, never render-resolution pixels.
inline Rect Picture(float w, float h, float aspect, bool letterbox) {
  if (!(w > 0 && h > 0 && aspect > 0)) return {};
  if (!letterbox) return {0, 0, w, h};
  const float pw = std::min(w, h*aspect), ph = std::min(h, w/aspect);
  return {(w-pw)*.5f, (h-ph)*.5f, pw, ph};
}
inline std::array<float, 4> Transform(const Matrix& m, const std::array<float, 4>& p) {
  std::array<float, 4> out{};
  for (unsigned col = 0; col < 4; ++col)
    for (unsigned row = 0; row < 4; ++row) out[col] += p[row]*m[row*4+col];
  return out;
}
inline std::optional<Projection> Project(const Camera& camera, Point p, Rect r, float inset) {
  if (!Valid(camera) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
      !std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) || !std::isfinite(r.h) ||
      !std::isfinite(inset) || inset < 0 || r.w <= 2*inset || r.h <= 2*inset) return {};
  const auto clip = Transform(camera.projection, Transform(camera.view, {p.x, p.y, p.z, 1}));
  for (float f : clip) if (!std::isfinite(f)) return {};
  Projection out;
  out.behind = clip[3] <= .001f;
  const float cx = r.x+r.w*.5f, cy = r.y+r.h*.5f;
  // For a behind-camera target use the undivided clip XY. Dividing by negative W
  // would mirror the arrow and point the player away from a target to their right.
  const float w = std::max(std::abs(clip[3]), .001f);
  float dx = clip[0]/w*r.w*.5f, dy = -clip[1]/w*r.h*.5f;
  if (!std::isfinite(dx) || !std::isfinite(dy)) return {};
  out.x = cx+dx; out.y = cy+dy;
  out.on_screen = !out.behind && out.x >= r.x+inset && out.x <= r.x+r.w-inset &&
      out.y >= r.y+inset && out.y <= r.y+r.h-inset;
  const float n = std::hypot(dx, dy);
  if (n < .001f) { dx = 0; dy = 1; }  // Exactly behind: stable bottom arrow.
  else { dx /= n; dy /= n; }
  out.dx = dx; out.dy = dy;
  if (!out.on_screen) {
    const float tx = std::abs(dx) > .00001f ? (r.w*.5f-inset)/std::abs(dx) : 1e30f;
    const float ty = std::abs(dy) > .00001f ? (r.h*.5f-inset)/std::abs(dy) : 1e30f;
    const float t = std::min(tx, ty);
    out.x = cx+dx*t; out.y = cy+dy*t;
  }
  return out;
}
inline float Opacity(float distance) {
  if (!std::isfinite(distance)) return 0;
  return .65f * std::clamp((distance-.7f)/1.3f, 0.f, 1.f) * std::clamp((60.f-distance)/15.f, 0.f, 1.f);
}
inline float FontScale(float distance) { return std::clamp(1.f-distance/110.f, .65f, 1.f); }

// Called only by the existing guest hooks. All reads are synchronous; the UI
// receives a mutex-protected value copy with no guest addresses.
void NormalCamera(const uint8_t* base, uint32_t actor, uint32_t caller);
bool BeginView(const uint8_t* base, uint32_t view, uint32_t caller);
void ProjectionPush(const uint8_t* base, uint32_t matrix);
void EndView(const uint8_t* base);
std::optional<Camera> Snapshot();
const char* Status();  // Static diagnostic label, never a persona or guest pointer.
}  // namespace kk::coop::nametag
