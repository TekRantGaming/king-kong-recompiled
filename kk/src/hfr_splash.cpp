// High frame rate fix: water effects started once a frame (kk_hfr_fixes,
// "splash" and "ripple").
//
// Footstep splashes: a character's after-blend callback
// (H_callback_after_blend, sub_826BB350) calls sub_825845E0 for both feet
// (bones 60 and 80) every frame while the character stands in water. Each call
// whose foot is down at the character's height starts a new splash effect (a
// type 9 effect with its own particles): one splash per foot per frame, 30 a
// second on the console, 110 at 110 FPS.
//
// Ripples: GST_MM_go (sub_82521040) calls GST_MM_exec_riple (sub_82520A50)
// every frame while its object moves in water; each call that finds water
// under a random point near the object starts a ripple effect (also type 9).
// Its own timer never holds it back (the last-ripple time it compares with,
// +332, is not written by any GST_MM script), so it runs once a frame: 21 a
// second at 30 FPS in the V-Rex chapter, 64 at 110.
//
// So wading and moving through water at high frame rates throw up several
// times the spray, and the particle effects alive at once grow with the frame
// rate (measured in the V-Rex chapter, docs/high-frame-rate.md).
//
// The fix lets each foot of each character, and each rippling object, start
// an effect at most on a 1/30 s grid of game time: 30 a second while it goes
// on, as on the console, and at once when it starts again after a pause. A
// skipped call still does what the function does besides the effect.

#include <cstdint>
#include <map>

#include <rex/hook.h>

#include "hfr.h"

namespace kk::hfr {
namespace {

constexpr uint32_t kGameClock = 0x82CCAE1C;     // the clock sub_825845E0 reads ...
constexpr uint32_t kGameClockBase = 0x82CCC02C;  // ... minus this: game time in seconds
constexpr uint32_t kCurrentObject = 0x82CCBF6C;  // the object whose callback runs
constexpr float kStep = 1.0f / 30.0f;            // the console's splash step
constexpr float kSlack = 0.0005f;                // frame time jitter allowed

float GameTime(const uint8_t* base) { return LoadF(base, kGameClock) - LoadF(base, kGameClockBase); }

// One effect source's 1/30 s grid. Returns false when the source started an effect less than 1/30 s
// ago. The game time restarts with each world, so a time that went backwards counts as a new start.
class Grid {
 public:
  bool Allow(uint64_t key, float now) {
    auto it = last_.find(key);
    const float since = it != last_.end() ? now - it->second : -1.0f;
    if (since >= 0.0f && since < kStep - kSlack) return false;
    // Keep the grid while the source goes on; start a new one after a pause.
    const float at = since >= 0.0f && since < 2.0f * kStep ? it->second + kStep : now;
    if (last_.size() > 256) last_.clear();
    last_[key] = at;
    return true;
  }

 private:
  std::map<uint64_t, float> last_;
};

}  // namespace
}  // namespace kk::hfr

REX_EXTERN(__imp__sub_825845E0);
REX_HOOK_RAW(sub_825845E0) {
  using namespace kk::hfr;
  static Grid grid;
  const float now = GameTime(base);
  const uint64_t key = uint64_t(Load32(base, kCurrentObject)) << 8 | (ctx.r3.u32 & 0xFF);  // character, foot
  if (On("splash") && !grid.Allow(key, now)) {
    StoreF(base, ctx.r4.u32, now);  // what the game's function does first
    Count("splash skips");
    return;
  }
  __imp__sub_825845E0(ctx, base);
}

REX_EXTERN(__imp__sub_82520A50);
REX_HOOK_RAW(sub_82520A50) {
  using namespace kk::hfr;
  static Grid grid;
  if (On("ripple") && !grid.Allow(Load32(base, kCurrentObject), GameTime(base))) {
    ctx.r3.u64 = 0;  // what the game's function always returns
    Count("ripple skips");
    return;
  }
  __imp__sub_82520A50(ctx, base);
}
