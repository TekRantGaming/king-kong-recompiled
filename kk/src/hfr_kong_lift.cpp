// High frame rate fix: Kong lifting an object out of a doorway (kk_hfr_fixes, "kong_lift").
//
// When Kong grabs a mashing object (KInteractive_Mashing_Object, e.g. the stone slab in front of
// the gate at the start of "The Skull Islanders"), his state k_ETAT_grab_mashing (sub_8259D998)
// plays the lift action (vars +1468, action 256 there) in two phases, and every frame sets
// Kong's animation (and through sub_82545108 the object's) to the fraction vars+1900 / length:
//
// 1. Start of the lift (vars +1196 set): +1900 is the animation's own frame in WHOLE 1/60 s
//    frames (sub_823F8AF0 truncates), and the animation is set back to that whole frame. It then
//    plays on for one frame before the next truncation. At 30 FPS a frame steps it about 2
//    frames (1 or 2 kept: 64 frames in about 1.4 s); at 60 about 1 (often 0 kept: 1.85 s); at
//    a steady 120 FPS half a frame, always cut to 0: the lift animation and the object stay at
//    frame 0, the mashing phase (which starts when +1900 reaches +1904, frame 64) never comes,
//    and Kong stands holding the object (the "stuck in the lifting animation, the object does not
//    move" report). Same rounding as the lifeboat rowers (hfr_rowing.cpp).
//    Fix: +1900 gets the animation's frame without the rounding, so it plays at its own speed
//    (64 frames in 1.07 s at 60, 120 and more; 30 FPS keeps the game's own 1.4 s).
// 2. Mashing (+1196 clear): +1900 += +1220 once a frame, at most +1912 (the speed +1220 follows
//    the presses at a rate per second, but the step is per frame): at 60 FPS the object rose
//    and fell twice as fast as at 30, at 120 four times. Fix: the step is scaled to 30 x frame
//    time (unscaled frame time, as at 30 FPS one step is one console frame).
// The lift only completes when the game's own test (TrigTest_CapaBit on the natives' controller
// KWC, bit 1) passes; that and the press handling run on game time and are left alone.
//
// Both are corrected just before the script reads +1900 to set the animations (its call of
// sub_823F8888, the animation's length, from 0x8259ED30), and only when +1900 is exactly what the
// game's own phase computed this frame. Off at 30 FPS (hfr::On), where the game runs unchanged.

#include <cmath>
#include <cstring>

#include <rex/hook.h>

#include "hfr.h"

#if defined(KK_DEV_TOOLS)
namespace kk {
void DevKongLiftTrace(uint8_t* base, uint32_t vars, uint32_t obj);  // dev_kong_lift.cpp
}
#endif

namespace kk::hfr {
void WithRealStepFn(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*));

namespace {

constexpr uint32_t kScriptContext = 0x82CCBF64;  // the running script's AI instance: [+64] its variables
constexpr uint32_t kRawFrameTime = 0x82C4EDBC;   // frame time before the game's time scale
constexpr uint32_t kSetAnimReturn = 0x8259ED34;  // k_ETAT_grab_mashing after its call of sub_823F8888

constexpr uint32_t kLiftPos = 1900;    // float: the lift's animation position, frames
constexpr uint32_t kLiftSpeed = 1220;  // float: frames per frame in the mashing phase
constexpr uint32_t kLiftMax = 1912;    // float: the last frame
constexpr uint32_t kStarting = 1196;   // int: the lift's start phase (whole frames)
constexpr uint32_t kLiftAction = 1468; // int: the lift action

bool Valid(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000; }

uint32_t Vars(const uint8_t* base) {
  const uint32_t ai = Load32(base, kScriptContext);
  const uint32_t vars = Valid(ai) ? Load32(base, ai + 64) : 0;
  return Valid(vars) ? vars : 0;
}

// An object's current action and its first animation layer's event list.
uint32_t Action(const uint8_t* base, uint32_t obj) {
  const uint32_t b = Valid(obj) ? Load32(base, obj + 20) : 0;
  const uint32_t anim = Valid(b) ? Load32(base, b) : 0;
  return Valid(anim) ? Load16(base, anim + 156) : 0xFFFF;
}
uint32_t Layer(const uint8_t* base, uint32_t obj) {
  const uint32_t b = Valid(obj) ? Load32(base, obj + 20) : 0;
  const uint32_t anim = Valid(b) ? Load32(base, b) : 0;
  const uint32_t layer = Valid(anim) ? Load32(base, anim + 132) : 0;
  const uint32_t events = Valid(layer) ? Load32(base, layer + 4) : 0;
  return Valid(events) ? events : 0;
}

uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

// What k_ETAT_grab_mashing's variables were when this call of it began.
struct Entry {
  bool in = false;
  uint32_t vars = 0;
  bool starting = false;
  float pos = 0;
} g_entry;

}  // namespace
}  // namespace kk::hfr

REX_EXTERN(__imp__sub_8271AF48);  // (events, track, &time, &length): an animation's time and length

// k_ETAT_grab_mashing: also has the dt_floor fix (hfr_dt_floor.cpp).
REX_EXTERN(__imp__sub_8259D998);
REX_HOOK_RAW(sub_8259D998) {
  using namespace kk::hfr;
  g_entry = {};
  if (On("kong_lift")) {
    if (const uint32_t vars = Vars(base)) {
      g_entry.in = true;
      g_entry.vars = vars;
      g_entry.starting = Load32(base, vars + kStarting) != 0;
      g_entry.pos = LoadF(base, vars + kLiftPos);
    }
  }
#if defined(KK_DEV_TOOLS)
  const uint32_t vars = Vars(base), obj = Load32(base, 0x82CCBF6C);  // the running script's object: Kong
#endif
  WithRealStepFn(ctx, base, __imp__sub_8259D998);
  g_entry = {};
#if defined(KK_DEV_TOOLS)
  kk::DevKongLiftTrace(base, vars, obj);
#endif
}

REX_EXTERN(__imp__sub_823F8888);
REX_HOOK_RAW(sub_823F8888) {
  using namespace kk::hfr;
  const bool here = uint32_t(ctx.lr) == kSetAnimReturn && g_entry.in;
  // The caller's callee-saved registers: r29 Kong, r31 his variables.
  const uint32_t obj = ctx.r29.u32, vars = ctx.r31.u32;
  __imp__sub_823F8888(ctx, base);
  if (!here || vars != g_entry.vars || Action(base, obj) != Load32(base, vars + kLiftAction)) return;
  const float pos = LoadF(base, vars + kLiftPos);
  if (g_entry.starting) {
    // Phase 1: the game put the whole frame here (trunc(time x 60)); use the frame as it is.
    const uint32_t events = Layer(base, obj);
    if (!events) return;
    const PPCContext saved = ctx;
    const uint32_t out = saved.r1.u32 - 0x400;
    ctx.r1.u64 = saved.r1.u32 - 0x800;
    ctx.r3.u64 = events;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = out;
    ctx.r6.u64 = out + 4;
    __imp__sub_8271AF48(ctx, base);
    const float time = LoadF(base, out);
    ctx = saved;
    const float frame = time * 60.0f;
    if (!(frame >= 0.0f) || std::trunc(frame) != pos || frame == pos) return;
    StoreF(base, vars + kLiftPos, frame);
    Count("kong_lift start frames");
    return;
  }
  // Phase 2: the game did pos = min(entry + speed, max) once this frame; make it speed x 30 dt.
  const float speed = LoadF(base, vars + kLiftSpeed), top = LoadF(base, vars + kLiftMax);
  const float sum = g_entry.pos + speed;
  const float game = sum - top >= 0.0f ? top : sum;
  if (Bits(game) != Bits(pos)) return;
  const float dt = LoadF(base, kRawFrameTime);
  if (!(dt > 0.0f && dt < 0.25f)) return;
  const float step = g_entry.pos + speed * (30.0f * dt);
  StoreF(base, vars + kLiftPos, step - top >= 0.0f ? top : step);
  Count("kong_lift mashing steps");
}
