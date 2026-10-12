// High frame rate fix: the lifeboat crew's rowing (kk_hfr_fixes, "rowing").
//
// In the Venture opening the lifeboat script's camera callback
// (Chaloupe_cb_aftercam, sub_824269C8) keeps one rower in step with the lead
// one every frame: it takes the lead rower's animation time, turns it into
// whole 1/60 s frames, adds 10 and sets the rower's time to that frame (modulo
// the animation's length in frames) with sub_8271AE08. The rower's own
// animation then plays on from there for one frame.
//
// An action that plays once and hands over to the next (the "start rowing"
// action 275, then the rowing loop 256) only hands over when the animation's
// own step crosses its last frame. Set to whole frames, the time never gets
// past length - 1 frames by itself, so the crossing needs a step of at least
// one 1/60 s frame: at 30 FPS (two frames a step) it always happens, at 60
// usually, above that never. The rower and the two that move with it then
// replay the short start-rowing action over and over instead of rowing (the
// "rowing skips" players saw above 30 FPS).
//
// The fix gives that call the lead rower's time plus 10 frames without the
// rounding (still modulo the length), so the rower follows smoothly and its
// own step crosses the end of an action at any frame rate. Measured with
// KK_DEV_SAMPLE / KK_DEV_ACTION_TRACE: see docs/high-frame-rate.md.

#include <cmath>

#include <rex/hook.h>

#include "hfr.h"

namespace kk::hfr {
namespace {

constexpr uint32_t kSyncReturn = 0x82427900;     // after Chaloupe_cb_aftercam's call of sub_8271AE08
constexpr uint32_t kScriptContext = 0x82CCBF64;  // the running script's AI instance: [+64] its variables

// An object's first animation layer's event list: [[[[obj + 20] + 0] + 132] + 4].
uint32_t Layer(const uint8_t* base, uint32_t obj) {
  auto valid = [](uint32_t a) { return a >= 0x40000000 && a < 0xA0000000; };
  if (!valid(obj)) return 0;
  const uint32_t b = Load32(base, obj + 20);
  const uint32_t anim = valid(b) ? Load32(base, b) : 0;
  const uint32_t layer = valid(anim) ? Load32(base, anim + 132) : 0;
  const uint32_t events = valid(layer) ? Load32(base, layer + 4) : 0;
  return valid(events) ? events : 0;
}

}  // namespace
}  // namespace kk::hfr

REX_EXTERN(__imp__sub_8271AF48);  // (events, track, &time, &length): an animation's time and length

REX_EXTERN(__imp__sub_8271AE08);
REX_HOOK_RAW(sub_8271AE08) {
  using namespace kk::hfr;
  if (uint32_t(ctx.lr) != kSyncReturn || !On("rowing")) {
    __imp__sub_8271AE08(ctx, base);
    return;
  }
  // The lead rower is the first of the script's four boat objects (its variables +68), the one set
  // here the second (+72), as in the game's code.
  const uint32_t ai = Load32(base, kScriptContext);
  const uint32_t vars = ai ? Load32(base, ai + 64) : 0;
  const uint32_t lead = vars ? Layer(base, Load32(base, vars + 68)) : 0;
  const uint32_t self = ctx.r3.u32;
  if (!lead || !self) {
    __imp__sub_8271AE08(ctx, base);
    return;
  }
  // Ask the game for both times, with scratch space well below the caller's stack.
  const PPCContext saved = ctx;
  const uint32_t out = saved.r1.u32 - 0x400;
  auto time_and_length = [&](uint32_t events, float* time, float* length) {
    ctx = saved;
    ctx.r1.u64 = saved.r1.u32 - 0x800;
    ctx.r3.u64 = events;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = out;
    ctx.r6.u64 = out + 4;
    __imp__sub_8271AF48(ctx, base);
    *time = LoadF(base, out);
    *length = LoadF(base, out + 4);
  };
  float lead_time = 0, lead_length = 0, own_time = 0, own_length = 0;
  time_and_length(lead, &lead_time, &lead_length);
  time_and_length(self, &own_time, &own_length);
  ctx = saved;
  const float frames = std::floor(own_length * 60.0f);  // the game's frame count
  if (frames >= 1.0f && lead_time >= 0.0f) {
    const float frame = std::fmod(lead_time * 60.0f + 10.0f, frames);
    ctx.f1.f64 = double(frame / 60.0f);
    Count("rowing syncs");
  }
  __imp__sub_8271AE08(ctx, base);
}
