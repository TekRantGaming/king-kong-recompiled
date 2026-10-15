#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
namespace kk::freecam {
struct Vec {
  float x = 0, y = 0, z = 0;
};
inline Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec operator*(Vec a, float b) { return {a.x * b, a.y * b, a.z * b}; }
inline float Length(Vec a) {
  return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
struct PadInput {
  float forward = 0, side = 0, vertical = 0, look_x = 0, look_y = 0;
  bool fast = false, slow = false;
};
// Physical XINPUT_STATE, before game button remapping/toggle aim. Consume every
// control, including Start/Back, so none can move or act on the player.
inline PadInput ConsumePad(uint8_t *state, float deadzone, bool invert_x,
                           bool invert_y) {
  auto u16 = [&](int offset) {
    return uint16_t(uint16_t(state[offset]) << 8 | state[offset + 1]);
  };
  auto stick = [&](int offset, float &x, float &y) {
    x = std::clamp(float(int16_t(u16(offset))) / 32767.f, -1.f, 1.f);
    y = std::clamp(float(int16_t(u16(offset + 2))) / 32767.f, -1.f, 1.f);
    const float n = std::sqrt(x * x + y * y);
    deadzone = std::clamp(deadzone, 0.f, .5f);
    const float scale = n > deadzone
                            ? std::min(1.f, (n - deadzone) / (1 - deadzone)) / n
                            : 0;
    x *= scale;
    y *= scale;
  };
  PadInput input;
  stick(8, input.side, input.forward);
  stick(12, input.look_x, input.look_y);
  if (invert_x) input.look_x = -input.look_x;
  // Visible forward is -guest row1, so reducing guest pitch looks upward.
  // Normal XInput up must therefore pass positive Y to Camera::Look.
  input.look_y *= invert_y ? -1.f : 1.f;
  const uint16_t buttons = u16(4);
  input.vertical = float(bool(buttons & 0x1000)) - bool(buttons & 0x2000);
  input.fast = state[7] > 30;
  input.slow = state[6] > 30;
  std::memset(state + 4, 0, 12);
  return input;
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
    // These are guest view axes, not flight directions: sub_827E64C8
    // negates row 0 and swaps/negates row 1 into render +Z (positive depth).
    // The visible forward and screen right therefore oppose both guest rows.
    Vec d = Forward() * -forward + Right() * -side + Vec{0, 0, vertical};
    const float n = Length(d);
    if (n > 1)
      d = d * (1 / n);
    position = position + d * (speed * (fast ? 4.f : 1.f) *
                               (slow ? .25f : 1.f) * std::clamp(dt, 0.f, .1f));
  }
};
} // namespace kk::freecam
