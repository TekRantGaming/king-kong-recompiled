#pragma once
#include <algorithm>
#include <cmath>
namespace kk::freecam {
struct Vec {
  float x = 0, y = 0, z = 0;
};
inline Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec operator*(Vec a, float b) { return {a.x * b, a.y * b, a.z * b}; }
inline float Length(Vec a) {
  return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
struct Camera {
  Vec position;
  float yaw = 0, pitch = 0, speed = 10;
  Vec Forward() const {
    return {std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch),
            std::sin(pitch)};
  }
  Vec Right() const { return {std::sin(yaw), -std::cos(yaw), 0}; }
  Vec Up() const {
    const Vec r = Right(), f = Forward();
    return {r.y * f.z, -r.x * f.z, r.x * f.y - r.y * f.x};
  }
  void Look(float dx, float dy) {
    yaw = std::remainder(yaw - dx, 6.28318530718f);
    pitch = std::clamp(pitch - dy, -1.553343f, 1.553343f);
  }
  void Wheel(float detents) {
    speed =
        std::clamp(speed * std::pow(1.25f, std::clamp(detents, -20.f, 20.f)),
                   0.1f, 1000.f);
  }
  void Move(float forward, float side, float vertical, bool fast, bool slow,
            float dt) {
    Vec d = Forward() * forward + Right() * side + Vec{0, 0, vertical};
    const float n = Length(d);
    if (n > 1)
      d = d * (1 / n);
    position = position + d * (speed * (fast ? 4.f : 1.f) *
                               (slow ? .25f : 1.f) * std::clamp(dt, 0.f, .1f));
  }
};
} // namespace kk::freecam
