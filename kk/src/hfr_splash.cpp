// High frame rate fix: footstep splashes in water (kk_hfr_fixes, "splash").
//
// A character's after-blend callback (H_callback_after_blend, sub_826BB350)
// calls sub_825845E0 for both feet (bones 60 and 80) every frame while the
// character stands in water. Each call whose foot is down at the character's
// height starts a new splash effect (a type 9 effect with its own particles).
// That is one splash per foot per frame: 30 a second on the console, 60 at
// 60 FPS, 110 at 110, so wading at high frame rates throws up several times
// the spray, and the particle effects alive at once grow with the frame rate
// (measured in the V-Rex chapter, docs/high-frame-rate.md).
//
// The fix lets each foot of each character splash at most on a 1/30 s grid of
// game time: 30 a second while the foot stays down, as on the console, and
// at once when it comes down again after a pause. A skipped call still does
// what the function does besides the splash (it stores the game time at r4).

#include <cstdint>
#include <map>
#include <utility>

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

}  // namespace
}  // namespace kk::hfr

REX_EXTERN(__imp__sub_825845E0);
REX_HOOK_RAW(sub_825845E0) {
  using namespace kk::hfr;
  if (!On("splash")) {
    __imp__sub_825845E0(ctx, base);
    return;
  }
  // Last splash time of each (character, foot); the game time restarts with each world, so a
  // time that went backwards counts as a new start.
  static std::map<std::pair<uint32_t, uint32_t>, float> last;
  const float now = GameTime(base);
  const auto key = std::make_pair(Load32(base, kCurrentObject), ctx.r3.u32 & 0xFF);
  auto it = last.find(key);
  const float since = it != last.end() ? now - it->second : -1.0f;
  if (since >= 0.0f && since < kStep - kSlack) {
    StoreF(base, ctx.r4.u32, now);  // what the game's function does first
    Count("splash skips");
    return;
  }
  // Keep the 1/30 s grid while the foot stays down; start a new one after a pause.
  const float at = since >= 0.0f && since < 2.0f * kStep ? it->second + kStep : now;
  if (last.size() > 256) last.clear();
  last[key] = at;
  Count("splash calls");
  __imp__sub_825845E0(ctx, base);
}
