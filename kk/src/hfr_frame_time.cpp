// High frame rate fix: the frame time in whole milliseconds
// (kk_hfr_fixes, "precise_dt").
//
// sub_8272CA20 sets the game's frame time once a frame from the kernel tick
// count, which counts whole milliseconds: dt = (ticks - last) / 1000, clamped
// to 0.001 .. 0.063 s, times the game's time scale (0x828D178C), stored at
// 0x82C636E0 (and unscaled at 0x82C4EDBC). At 30 FPS that is 33 or 34 ms, a 3%
// wobble. At higher rates the rounding grows: a 144 FPS frame (6.94 ms) reads
// as 6, 7 or 8 ms, so everything moves by up to 15% more or less from one
// frame to the next (uneven motion on a smooth display); above 500 FPS whole
// frames read as 0 ms, are clamped up to 1 ms, and the game runs faster than
// real time (about 1.4x at 1400 FPS, 2x at 2000).
//
// The fix replaces the frame time with the time since the last frame from the
// host's high resolution clock, with the game's clamps and time scale. The
// game's own millisecond clock (0x82CCAE1C, used for absolute times) is left
// as it is, and a world that asks for fixed steps (its byte +2444) keeps them.
//
// Dev builds: KK_DEV_FORCE_DT=0.004 (seconds) replaces the frame time with that
// value every frame instead, so each system steps as it would at 250 FPS on a
// PC that cannot reach it (the game then runs slower than real time);
// KK_DEV_FORCE_DT_AT=55 starts that 55 s after the save menu opens.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#include <rex/hook.h>

#include "hfr.h"
#include "benchmark.h"
#include "freecam.h"

namespace kk {
#if defined(KK_DEV_TOOLS)
double DevSecondsSinceMenu();
void DevFrameLogTick(const uint8_t* base);  // dev_frame_log.cpp
#endif
}  // namespace kk

REX_EXTERN(__imp__sub_8272CA20);

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint32_t kRawFrameTime = 0x82C4EDBC;
constexpr uint32_t kTimeScale = 0x828D178C;
constexpr uint32_t kUniverse = 0x828E836C;  // +12: the current world (its +2444: fixed step in 1/60 s)

#if defined(KK_DEV_TOOLS)
bool ForceFrameTime(uint8_t* base) {
  static const float forced = [] {
    const char* v = std::getenv("KK_DEV_FORCE_DT");
    return v && *v ? std::stof(v) : 0.0f;
  }();
  static const double from = [] {
    const char* v = std::getenv("KK_DEV_FORCE_DT_AT");
    return v && *v ? std::stod(v) : 0.0;
  }();
  if (forced <= 0.0f || kk::DevSecondsSinceMenu() < from) return false;
  kk::hfr::StoreF(base, kRawFrameTime, forced);
  kk::hfr::StoreF(base, kk::hfr::kFrameTime, forced * kk::hfr::LoadF(base, kTimeScale));
  return true;
}
#endif

// The game's frame time step, then the precise frame time.
void SetFrameTime(PPCContext& ctx, uint8_t* base) {
  using namespace kk::hfr;
  static Clock::time_point last{};
  const Clock::time_point now = Clock::now();
  const double real = last == Clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - last).count();
  last = now;
  __imp__sub_8272CA20(ctx, base);
#if defined(KK_DEV_TOOLS)
  if (ForceFrameTime(base)) return;
#endif
  if (!On("precise_dt") || real <= 0.0) return;
  const uint32_t world = Load32(base, kUniverse + 12);
  if (world >= 0x40000000 && world < 0xA0000000 && base[world + 2444] != 0) return;  // fixed steps
  const float dt = std::clamp(float(real), 0.001f, 0.063f);
  StoreF(base, kRawFrameTime, dt);
  StoreF(base, kFrameTime, dt * LoadF(base, kTimeScale));
}

}  // namespace

// The one hook on sub_8272CA20: the frame time as above, then the developer frame log
// (KK_DEV_DT_LOG, dev_frame_log.cpp), which reads what it ended up as.
REX_HOOK_RAW(sub_8272CA20) {
  kk::freecam::Frame(base);
  SetFrameTime(ctx, base);
  kk::benchmark::Frame(kk::hfr::LoadF(base, kRawFrameTime),
      kk::hfr::LoadF(base, kk::hfr::kFrameTime), base);
#if defined(KK_DEV_TOOLS)
  kk::DevFrameLogTick(base);
#endif
}
