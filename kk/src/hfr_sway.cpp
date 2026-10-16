// High frame rate fixes: floating boats and hanging objects that sway (kk_hfr_fixes, "boat" and "rope").
//
// Boats: Chaloupe_Rotation (sub_8242ECA0, every lifeboat-like object) tilts the boat on the
// water. With synthetic waves on (script variable +108) it asks sub_82623098 for the water at
// the boat's four corners, but that function keeps each corner's own height (from the boat's
// current, already tilted axes) and only adds a wave offset of +472 * sin(2 t). The new tilt is
// the plane through those corners, so each call adds the wave's slope on top of the current tilt,
// then slerps back toward the current tilt by the frame time (sub_82719648, factor dt). So every
// frame adds (1 - dt) times the wave's slope: 29 times a second at 30 FPS, 59 at 60, 119 at 120.
// The roll swings about 35 degrees each way at 30 FPS, 60-75 at 60 and 80-85 at 120 (nearly on
// its side). The boat's bobbing (+476, through a lerp by +92 * dt) is frame-rate correct.
// The fix scales the wave offset during the call by 29 dt / (1 - dt), so each frame adds 29 dt
// times the slope: the same 29 a second at any frame rate (exactly 1 at 30 FPS).
//
// Hanging objects (SA scripts: SA_ETAT_WaitAccroche, SA_ETAT_Loop, SA_ETAT_Fall; the boxes on a
// rope the player shoots down): their rope is simulated by sub_825625F8 once a call, called once
// a frame, with a fixed 1/30 s step (it stores 1/30 at +1084, moves each node by velocity / 30
// and sets the velocity to the move times 30; sub_82544E30 damps it with that step). Its own
// divider (+1080) counts calls too. So the rope's simulated time runs at the frame rate / 30:
// at 120 FPS the box swings four times as fast. The fix runs the rope on a 1/30 s grid of game
// time, as on the console: one step per 1/30 s, with the frame time it reads (the fade timer
// +1268 in sub_82544E30) at least that step. Hits between steps keep their impulse (they add to
// the node velocities, sub_8254CAA8) until the next step. The rope moves in 30 Hz steps at high
// frame rates, as it does at 30 FPS.
//
// At 30 FPS (hfr::Active() false) both call the game's code unchanged.

#include <cmath>
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

uint32_t ScriptVariables(const uint8_t* base) {
  const uint32_t ai = Load32(base, kScriptContext);
  return ai ? Load32(base, ai + 64) : 0;
}

float GameTime(const uint8_t* base) { return LoadF(base, kGameClock) - LoadF(base, kGameClockBase); }

}  // namespace
}  // namespace kk::hfr

REX_EXTERN(__imp__sub_8242ECA0);
REX_HOOK_RAW(sub_8242ECA0) {
  using namespace kk::hfr;
  const uint32_t vars = ScriptVariables(base);
  const float dt = FrameTime(base);
  if (!On("boat") || !vars || Load32(base, vars + 108) == 0 || !(dt > 0.0f && dt < 0.5f)) {
    __imp__sub_8242ECA0(ctx, base);
    return;
  }
  const uint32_t wave_bits = Load32(base, vars + 472);
  StoreF(base, vars + 472, LoadF(base, vars + 472) * ((1.0f - kStep) * 30.0f * dt / (1.0f - dt)));
  __imp__sub_8242ECA0(ctx, base);
  Store32(base, vars + 472, wave_bits);
  Count("boat tilts");
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
