// The high frame rate fix "blur" (motion_blur.cpp): the motion blur trail's
// length in seconds as at 30 FPS. Pure math, unit tested (tests/hfr_blur_tests.cpp).
//
// The game's motion blur (sub_82768DE8) is a feedback blur: each frame it draws
// the kept previous output over the new frame with alpha
//   a = min(1, s * (0.6 + 0.4 * fps / 30))
// (s = the effect's strength, fps = the device's frames-per-second estimate,
// 0x82D623D0) and then resolves the result as the next kept frame. An image
// therefore fades by a factor a per frame: at 30 FPS by a30 = min(1, s) per
// 1/30 s. The game's own fps term raises the alpha with the frame rate, but
// linearly, and it reaches 1 (a frozen trail) at s >= 1 / (0.6 + 0.4 fps / 30):
// s 0.45 at 120 FPS. For the same fade per second at a frame time dt the alpha
// must be a30 ^ (30 dt). Frames of 1/30 s or longer keep the game's own value.

#pragma once

#include <algorithm>
#include <cmath>

namespace kk::hfr {

// The game's alpha for strength s at the device's fps estimate.
inline float GameBlurAlpha(float s, float fps) { return std::min(1.0f, s * (0.6f + 0.4f * fps / 30.0f)); }

// The alpha that fades the trail per second as at 30 FPS, for a frame of dt
// seconds (dt below 1/30 s; callers keep the game's alpha otherwise).
inline float BlurAlpha30(float s, float dt) {
  const float a30 = std::min(1.0f, s);
  if (a30 <= 0.0f || a30 >= 1.0f) return a30;
  return float(std::pow(double(a30), 30.0 * double(dt)));
}

// The strength to give the game so that its own formula yields alpha a at the
// fps estimate (the hook changes the strength around the call and puts it back).
inline float StrengthForAlpha(float a, float fps) { return a / (0.6f + 0.4f * std::max(fps, 0.0f) / 30.0f); }

}  // namespace kk::hfr
