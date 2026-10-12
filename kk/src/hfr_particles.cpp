// High frame rate fix: particle drag (kk_hfr_fixes, "particles").
//
// sub_82862460 moves every particle of one generator (rain splashes, sparks,
// spray, smoke): it takes steps of 1/60 s while the frame time left is at
// least 1/30 s, then one step for the rest, and each step multiplies the
// particle's velocity by the generator's drag (+48). So the drag is applied
// once or twice a frame, not per second: on the console's 33/33/34 ms frames
// that is 40 times a second, at 60 FPS 60, at 120 FPS 120, unlimited many
// hundreds, and particles stop short and fall straighter at high frame rates.
//
// The fix gives the call a drag that, applied as many times as the game will
// this frame, multiplies the velocity by drag ^ (40 x frame time): the
// console's rate at any frame rate. It is put back after the call.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"

namespace {

// How many times the game applies the drag this frame (sub_82862460's steps).
int DragSteps(float dt) {
  constexpr float kThirtieth = 1.0f / 30.0f, kSixtieth = 1.0f / 60.0f;  // its constants (0x820B0F78, 0x820B0C68)
  int n = 1;
  for (float left = dt; left >= kThirtieth && n < 64; left -= kSixtieth) ++n;
  return n;
}

}  // namespace

REX_EXTERN(__imp__sub_82862460);
REX_HOOK_RAW(sub_82862460) {
  using namespace kk::hfr;
  const uint32_t at = ctx.r3.u32 + 48;
  const float drag = LoadF(base, at);
  const float dt = FrameTime(base);
#if defined(KK_DEV_TOOLS)
  // KK_DEV_HFR_TRACE=1: once a second, how many generators ran and their drags.
  if (static const bool trace = std::getenv("KK_DEV_HFR_TRACE") != nullptr; trace) {
    static auto start = std::chrono::steady_clock::now();
    static float lo = 1, hi = 0;
    static int calls = 0, damped = 0;
    static double speed_sum = 0, vz_sum = 0;
    static int particles = 0;
    static std::unordered_set<uint32_t> distinct;
    distinct.insert(ctx.r3.u32);
    ++calls;
    if (drag > 0.0f && drag < 1.0f) {
      ++damped, lo = std::min(lo, drag), hi = std::max(hi, drag);
      // Particles: [+112] array of 36-byte entries ([+104] of them), velocity at +16.
      const uint32_t list = Load32(base, ctx.r3.u32 + 112), count = Load32(base, ctx.r3.u32 + 104);
      for (uint32_t i = 0; list && i < count && i < 4096; ++i) {
        const uint32_t p = list + i * 36;
        const float vx = LoadF(base, p + 16), vy = LoadF(base, p + 20), vz = LoadF(base, p + 24);
        speed_sum += std::sqrt(vx * vx + vy * vy + vz * vz), vz_sum += vz, ++particles;
      }
    }
    if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(1)) {
      REXLOG_INFO(
          "KK dev hfr particles: {} generator updates of {} generators, {} with drag {:.3f}..{:.3f}, dt {:.4f}, "
          "steps {}, {} particles mean speed {:.3f} mean vz {:.3f}",
          calls, distinct.size(), damped, lo, hi, dt, DragSteps(dt), particles,
          particles ? speed_sum / particles : 0.0, particles ? vz_sum / particles : 0.0);
      distinct.clear();
      start = std::chrono::steady_clock::now(), lo = 1, hi = 0, calls = damped = 0;
      speed_sum = vz_sum = 0, particles = 0;
    }
  }
#endif
  if (!On("particles") || !(drag > 0.0f && drag < 1.0f) || !(dt > 0.0f)) {
    __imp__sub_82862460(ctx, base);
    return;
  }
  StoreF(base, at, std::pow(drag, 40.0f * dt / float(DragSteps(dt))));
  __imp__sub_82862460(ctx, base);
  StoreF(base, at, drag);
}
