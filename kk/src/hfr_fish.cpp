// High frame rate fix: the swimming wiggle of the small water creatures
// (kk_hfr_fixes, "fish").
//
// The fish in the water of Ann's chapter (and the other PNJ_EatMe creatures
// while they swim) run PNJ_EatMe_ETAT_Water (sub_8243B2C8) once a frame. It
// turns the fish's forward direction part of the way to where it is going
// (6 x frame time of the angle, 8243BF3C / 8243BFEC), then turns it further,
// about the up axis, by
//     sin(swim phase) x min(forward speed, 0.25)  radians   (8243C10C)
// with sub_827184C0, and sets that as the fish's facing (8243C144); its
// velocity follows the facing. The swim phase (variables +116) advances by
// pi x frame time, so the wiggle has a 2 s period at any frame rate, but its
// angle is added once per call with no frame time: on the console's 30 frames
// a second the heading swings about +-65 degrees around its course; at 60 FPS
// the same angle is added twice as often (+-128 degrees), at 120 FPS four
// times (+-255 degrees, the fish turn in circles), at 240 eight times.
//
// The fix scales that one angle by 30 x frame time, so the heading turns by
// the same amount per second of game time as on the console. Measured with
// KK_DEV_FISH (dev_fish.cpp): see docs/high-frame-rate.md. At 30 FPS the fix
// is off and the game runs its own code.

#include <cmath>

#include <rex/hook.h>

#include "hfr.h"
#include "hfr_fish_logic.h"

namespace kk {
#if defined(KK_DEV_TOOLS)
void DevFishWiggle(float game_angle, float used_angle);  // dev_fish.cpp
bool DevFishFixAllowed();  // dev_fish.cpp: KK_DEV_FISH_OFF_UNTIL
#endif
}  // namespace kk

namespace {
constexpr uint32_t kWiggleReturn = 0x8243C114;  // PNJ_EatMe_ETAT_Water, after its call of sub_827184C0
}  // namespace

// sub_827184C0(r3 vector, r4 axis, f1 angle, r6 out): r3 turned about r4 by f1 radians.
REX_EXTERN(__imp__sub_827184C0);
REX_HOOK_RAW(sub_827184C0) {
  using namespace kk::hfr;
  if (uint32_t(ctx.lr) != kWiggleReturn) {
    __imp__sub_827184C0(ctx, base);
    return;
  }
  const float angle = float(ctx.f1.f64);
  const float dt = FrameTime(base);
  bool allowed = true;
#if defined(KK_DEV_TOOLS)
  allowed = kk::DevFishFixAllowed();
#endif
  if (allowed && On("fish") && kk::hfr::FishWiggleApplies(dt)) {
    ctx.f1.f64 = double(kk::hfr::FishWiggle(angle, dt));
    Count("fish wiggles");
  }
#if defined(KK_DEV_TOOLS)
  kk::DevFishWiggle(angle, float(ctx.f1.f64));
#endif
  __imp__sub_827184C0(ctx, base);
}
