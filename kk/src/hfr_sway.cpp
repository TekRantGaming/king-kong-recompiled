// High frame rate fixes: floating boats and hanging objects that sway (kk_hfr_fixes, "boat" and "rope").
//
// Boats: Chaloupe_Rotation (sub_8242ECA0, every lifeboat-like object) tilts the boat on the
// water. It samples the water under the boat (sub_82623098) at its centre and three corners;
// with synthetic waves on (script variable +108) a corner keeps its own height (from the boat's
// current, already tilted axes) plus a wave offset of +472 * sin(2 t), and one corner also gets a
// start-up offset +612 * sin(3 t) (+612 fades to 0 at 0.5/s by frame time). The new up axis is
// the normal of those points, so it is the current tilt plus the points' slope; then it is
// slerped back toward the "current" up axis by t = +88 * dt (sub_82719648; +88 is 1 except on a
// one-off frame the game sets it). So every frame adds (1 - dt) times the slope: 29 times a
// second at 30 FPS, 59 at 60, 119 at 120. The roll swings about 35 degrees each way at 30 FPS,
// 60-75 at 60 and 80-85 at 120, and the start-up lean grows with the frame rate too.
// When the boat follows a leader (+504), that "current" axis was first pulled toward the
// leader's up axis by +468 * dt, so the pull that survives is t * 468 * dt: it shrinks with the
// frame rate (the lean then fades more slowly).
// The fix sets +88 for the call so that the slerp keeps 29 dt of the change (1 - t = 29 dt, the
// same 29 a second at any frame rate; exactly the game's 1 - dt at 30 FPS), which scales every
// part of the slope (waves, start-up offset) alike, and sets +468 so that t * 468 * dt is dt times
// the 30 FPS pull per second. Traced per call (task 151): the roll change is 1.10 rad/s times
// sin(2 t) at 30 and at 60 FPS alike. The tilt is an integral of the wave, so its middle depends
// on the wave's phase when the boat starts; the game clock is already 1.99 s at the first call at
// 30 FPS but 1.61 s at 60, which changes the start-up lean (it fades in about 20 s).
//
// Hanging objects (SA scripts: SA_ETAT_WaitAccroche, SA_ETAT_Loop, SA_ETAT_Fall; the boxes on a
// rope the player shoots down): their rope is simulated by sub_825625F8 once a call, called once
// a frame, with a fixed 1/30 s step (it stores 1/30 at +1084, moves each node by velocity / 30
// and sets the velocity to the move times 30; sub_82544E30 damps it with that step). Its own
// divider (+1080) counts calls too. So the rope's simulated time runs at the frame rate / 30:
// at 120 FPS the box swings four times as fast. The fix runs the rope on a 1/30 s grid of game
// time, as on the console: one step per 1/30 s, with the frame time it reads (the fade timer
// +1268 in sub_82544E30) at least that step. The rope moves in 30 Hz steps at high frame rates,
// as it does at 30 FPS.
// SA_ETAT_WaitAccroche (sub_8268A450) also pushes the end node (the box) with a slowly turning
// wind every call: velocity(+1180 + 12 * node count) += 0.02 * (-sin(pi gt), cos(pi gt), 0).
// Called every frame but integrated once per step, that is fps/30 times the push. The fix takes
// (1 - 30 dt) of that push off before the call, so each frame pushes 30 dt times it: the same push
// per second as at 30 FPS. Hits (sub_82808310, the messages read by sub_8254CAA8 and
// sub_8256EA00) are one-off impulses: they keep their full size and wait for the next step.
//
// At 30 FPS (hfr::Active() false) these call the game's code unchanged.

#include <cmath>
#if defined(KK_DEV_TOOLS)
#include <cstdio>
#include <cstdlib>
#endif
#include <map>

#include <rex/hook.h>

#include "hfr.h"

namespace kk::hfr {
namespace {

constexpr uint32_t kScriptContext = 0x82CCBF64;  // the running script's AI instance: [+64] its variables
constexpr uint32_t kGameClock = 0x82CCAE1C;      // game time = this minus the next one, in seconds
constexpr uint32_t kGameClockBase = 0x82CCC02C;
constexpr float kStep = 1.0f / 30.0f;            // the console's frame
constexpr float kSlack = 0.0005f;                // frame time jitter allowed on the grid
constexpr uint32_t kOne = 0x3F800000;            // 1.0f
constexpr float kPi = 3.14159274f;               // 820B0ED4
constexpr float kWind = 0.02f;                   // 820B0D00

uint32_t ScriptVariables(const uint8_t* base) {
  const uint32_t ai = Load32(base, kScriptContext);
  return ai ? Load32(base, ai + 64) : 0;
}

float GameTime(const uint8_t* base) { return LoadF(base, kGameClock) - LoadF(base, kGameClockBase); }

}  // namespace
}  // namespace kk::hfr

// Dev builds: KK_DEV_BOAT_TRACE=1 writes boat.csv in KK_DEV_SAMPLE_DIR, a row before and after
// every Chaloupe_Rotation call (game time, frame time, the boat's axes, its leader's up axis and
// the script values the tilt uses).
static void BoatTrace(const uint8_t* base, uint32_t vars, const char* when) {
#if defined(KK_DEV_TOOLS)
  using namespace kk::hfr;
  static FILE* f = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    const char* d = std::getenv("KK_DEV_SAMPLE_DIR");
    if (std::getenv("KK_DEV_BOAT_TRACE") && d) {
      char path[512];
      std::snprintf(path, sizeof path, "%s/boat.csv", d);
      f = std::fopen(path, "w");
      if (f) std::fprintf(f, "when,gt,dt,obj,vars,lead,u0,u1,u2,a0,a1,a2,b0,b1,b2,lu0,lu1,lu2,p612,p472,p468,p464,p88,n96,w108\n");
    }
  }
  if (!f || !vars) return;
  const uint32_t obj = Load32(base, 0x82CCBF6C);
  const uint32_t m = obj ? Load32(base, obj + 12) : 0;
  const uint32_t lead = Load32(base, vars + 504);
  const uint32_t lm = lead ? Load32(base, lead + 12) : 0;
  auto M = [&](uint32_t mm, int i) { return mm ? LoadF(base, mm + 4 * i) : 0.0f; };
  std::fprintf(f, "%s,%.4f,%.5f,%08X,%08X,%08X,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d\n",
               when, GameTime(base), FrameTime(base), obj, vars, lead, M(m, 8), M(m, 9), M(m, 10), M(m, 0), M(m, 1),
               M(m, 2), M(m, 4), M(m, 5), M(m, 6), M(lm, 8), M(lm, 9), M(lm, 10), LoadF(base, vars + 612),
               LoadF(base, vars + 472), LoadF(base, vars + 468), LoadF(base, vars + 464), LoadF(base, vars + 88),
               int(Load32(base, vars + 96)), int(Load32(base, vars + 108)));
#else
  (void)base, (void)vars, (void)when;
#endif
}

REX_EXTERN(__imp__sub_8242ECA0);
REX_HOOK_RAW(sub_8242ECA0) {
  using namespace kk::hfr;
  const uint32_t vars = ScriptVariables(base);
  const float dt = FrameTime(base);
  BoatTrace(base, vars, "pre");
  if (!On("boat") || !vars || Load32(base, vars + 108) == 0 || !(dt > 0.0f && dt < kStep) ||
      Load32(base, vars + 88) != kOne) {
    __imp__sub_8242ECA0(ctx, base);
    BoatTrace(base, vars, "post");
    return;
  }
  // Keep 29 dt of the change: blend t with 1 - t = 29 dt (the game's 1 - dt at 1/30 s).
  const float t = 1.0f - (1.0f - kStep) * 30.0f * dt;
  StoreF(base, vars + 88, t / dt);
  const uint32_t blend_bits = Load32(base, vars + 88);
  // The pull toward the leader's up axis (+468 * dt) only counts times t: keep it at the 30 FPS
  // amount per second, min(1, +468 / 30) of the gap, so per frame dt * that = t * h.
  const uint32_t follow_bits = Load32(base, vars + 468);
  const float follow = LoadF(base, vars + 468);
  if (follow > 0.0f) StoreF(base, vars + 468, std::fmin(1.0f, follow * kStep) / t);
  __imp__sub_8242ECA0(ctx, base);
  Store32(base, vars + 468, follow_bits);
  if (Load32(base, vars + 88) == blend_bits) Store32(base, vars + 88, kOne);  // call left early
  BoatTrace(base, vars, "post");
  Count("boat tilts");
}

REX_EXTERN(__imp__sub_8268A450);
REX_HOOK_RAW(sub_8268A450) {
  using namespace kk::hfr;
  const uint32_t vars = ScriptVariables(base);
  const float dt = FrameTime(base);
  const uint32_t nodes = vars ? Load32(base, vars + 1072) : 0;
  if (!On("rope") || nodes < 1 || nodes > 19 || !(dt > 0.0f && dt < kStep)) {
    __imp__sub_8268A450(ctx, base);
    return;
  }
  // The game adds kWind * (-sin, cos, 0)(pi gt) below; take (1 - 30 dt) of it off first.
  const float angle = GameTime(base) * kPi;
  const float off = kWind * (1.0f - 30.0f * dt);
  const uint32_t v = vars + 1180 + 12 * nodes;
  StoreF(base, v + 0, LoadF(base, v + 0) + off * std::sin(angle));
  StoreF(base, v + 4, LoadF(base, v + 4) - off * std::cos(angle));
  __imp__sub_8268A450(ctx, base);
  Count("rope winds");
}

REX_EXTERN(__imp__sub_825625F8);
REX_HOOK_RAW(sub_825625F8) {
  using namespace kk::hfr;
  const uint32_t vars = ScriptVariables(base);
  if (!On("rope") || !vars) {
    __imp__sub_825625F8(ctx, base);
    return;
  }
  // Each rope's last step on game time. The game time restarts with each world, so a time that
  // went backwards starts a new grid; so does a pause of two steps or more.
  static std::map<uint32_t, float> last;
  const float now = GameTime(base);
  auto it = last.find(vars);
  const float since = it != last.end() ? now - it->second : -1.0f;
  if (since >= 0.0f && since < kStep - kSlack) {
    Count("rope skips");
    return;
  }
  const float at = since >= 0.0f && since < 2.0f * kStep ? it->second + kStep : now;
  if (last.size() > 256) last.clear();
  last[vars] = at;
  const uint32_t dt_bits = Load32(base, kFrameTime);
  if (FrameTime(base) < kStep) StoreF(base, kFrameTime, kStep);
  __imp__sub_825625F8(ctx, base);
  Store32(base, kFrameTime, dt_bits);
  Count("rope steps");
}
