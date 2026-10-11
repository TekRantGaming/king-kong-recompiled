// High frame rate fix: follow smoothing in 1/300 s steps (kk_hfr_fixes, "follow").
//
// sub_828271E8 (called by sub_82827B10, a modifier the renderer runs on some
// objects every frame) moves a point and an orientation towards a target. Its
// strength p (+8) gives a per-step factor s = 1 - (1 - p)^2, and it takes
// ceil(frame time / (1/300 s)) steps: the part left behind is s^n and the
// point moves 1 - s^n of the way. Whole steps make the rate depend on the
// frame rate: 310 steps a second on the console's 33/34 ms frames, 340 at 60
// FPS, 360 at 90 or 120, 432 at 144, 480 at 240, one per frame unlimited (1000
// at 1000 FPS). Above 30 FPS the follow is stiffer.
//
// The fix gives the call a strength whose n steps leave s^(310 x frame time),
// the console's rate, and puts the real one back afterwards. (When the object
// has flag 0x1000 at +4 the function does something else and is left alone.)

#include <algorithm>
#include <cmath>

#include <rex/hook.h>

#include "hfr.h"

namespace {

// The game's step count: f = 0; do { f += 1/300; ++n; } while (f < dt).
int FollowSteps(float dt) {
  constexpr float kStep = 1.0f / 300.0f;  // 0x820B1250
  int n = 0;
  float f = 0.0f;
  do {
    f += kStep;
    ++n;
  } while (f < dt && n < 100000);
  return n;
}

}  // namespace

REX_EXTERN(__imp__sub_828271E8);
REX_HOOK_RAW(sub_828271E8) {
  using namespace kk::hfr;
  const uint32_t self = ctx.r3.u32;
  const float dt = FrameTime(base);
  const bool other_path = (Load32(base, self + 4) & 0x1000) != 0;
  const float p = LoadF(base, self + 8);
  if (!On("follow") || other_path || !(dt > 0.0f) || !(p > 0.0f && p < 1.0f)) {
    __imp__sub_828271E8(ctx, base);
    return;
  }
  const float keep = 1.0f - (1.0f - p) * (1.0f - p);  // s, the part kept per step
  const float want = std::pow(keep, 310.0f * dt / float(FollowSteps(dt)));
  StoreF(base, self + 8, 1.0f - std::sqrt(std::max(0.0f, 1.0f - want)));
  Count("follow calls");
  __imp__sub_828271E8(ctx, base);
  StoreF(base, self + 8, p);
}
