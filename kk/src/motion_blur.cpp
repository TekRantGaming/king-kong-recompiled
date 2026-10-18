// Motion blur (launcher: Display > Camera > Motion blur, kk_motion_blur).
//
// The game's motion blur is one of its after effects (sub_82768DE8, a virtual
// method of the effect's class): it blends the previous frame over the new one
// with a strength from the effect's parameters (this + (index + 5) * 4, scaled
// by the frame rate into g_fMotionBlurAlpha), then keeps the new frame for next
// time. Kong's sequences and some transitions turn it up. When the strength is
// 0 the game skips the blend and drops the kept frame, so with the setting off
// the effect runs with its strength read as 0 and the game's value is put back
// afterwards (it fades the strength itself).
//
// It is a feedback blur: the shader (ProcessPixel, iMotionBlur) outputs the kept
// texture with alpha g_fMotionBlurAlpha (0x82F1CDC8) = min(1, s * (0.6 + 0.4 *
// fps / 30)), fps being the device's frames-per-second estimate (0x82D623D0),
// and the result is then resolved (sub_82116178) as the next kept frame, so
// the trail fades by that alpha per frame. High frame rate fix "blur" (hfr.h): at frames shorter
// than 1/30 s (unscaled frame time 0x82C4EDBC) the strength is changed around
// the call so that the alpha is min(1, s) ^ (30 dt) (hfr_blur_logic.h): the
// trail fades per second as at 30 FPS.

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/system/flags.h>

#include "hfr.h"
#include "hfr_blur_logic.h"
#include "photo_mode.h"
#include "settings.h"

#if defined(KK_DEV_TOOLS)
#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/logging.h>
#endif

#if defined(KK_DEV_TOOLS)
namespace kk {
double DevSecondsSinceMenu();
}
#endif

namespace {

constexpr uint32_t kAlpha = 0x82F1CDC8;          // g_fMotionBlurAlpha
constexpr uint32_t kDeviceFps = 0x82D623D0;      // the device's frames-per-second estimate
constexpr uint32_t kRawFrameTime = 0x82C4EDBC;   // seconds since the last frame, not time scaled

#if defined(KK_DEV_TOOLS)
// KK_DEV_BLUR_TRACE=<s> with KK_DEV_SAMPLE_DIR: for the first <s> seconds on the
// dev script clock writes blur.csv (t, game frame time, unscaled frame time,
// the device's fps estimate, strength the game set, strength used, alpha the
// shader got, reset flag), then logs
// "KK dev blur: done".
FILE* Trace(double* t) {
  static const double window = [] {
    const char* v = std::getenv("KK_DEV_BLUR_TRACE");
    return v && *v ? std::atof(v) : 0.0;
  }();
  if (window <= 0) return nullptr;
  *t = kk::DevSecondsSinceMenu();
  if (*t > window) {
    static bool done = false;
    if (!done) REXLOG_INFO("KK dev blur: done");
    done = true;
    return nullptr;
  }
  static FILE* f = [] {
    const char* dir = std::getenv("KK_DEV_SAMPLE_DIR");
    const std::string path = dir && *dir ? std::string(dir) + "/blur.csv" : std::string("blur.csv");
    FILE* file = std::fopen(path.c_str(), "w");
    if (file) std::fprintf(file, "t,dt,raw_dt,fps,s_game,s_used,alpha,reset\n");
    return file;
  }();
  return f;
}
#endif

}  // namespace

REX_EXTERN(__imp__sub_82768DE8);
REX_HOOK_RAW(sub_82768DE8) {
  const uint32_t strength = ctx.r3.u32 + (ctx.r4.u32 + 5) * 4;
  const uint32_t object = ctx.r3.u32;
  uint32_t saved;
  std::memcpy(&saved, base + strength, 4);
  float s = kk::hfr::LoadF(base, strength);
  float used = s;
  if (!REXCVAR_GET(kk_motion_blur) || kk::photo::Active()) {  // photo mode: no trails behind its camera
    used = 0.0f;
  } else if (s > 0.0f && kk::hfr::On("blur")) {
    const float dt = kk::hfr::LoadF(base, kRawFrameTime);
    if (dt > 0.0f && dt < 1.0f / 30.0f) {
      used = kk::hfr::StrengthForAlpha(kk::hfr::BlurAlpha30(s, dt), kk::hfr::LoadF(base, kDeviceFps));
      kk::hfr::Count("blur");
    }
  }
#if defined(KK_DEV_TOOLS)
  const uint32_t reset = kk::hfr::Load32(base, object + 16);
#endif
  if (used != s) kk::hfr::StoreF(base, strength, used);
  __imp__sub_82768DE8(ctx, base);
  if (used != s) std::memcpy(base + strength, &saved, 4);
#if defined(KK_DEV_TOOLS)
  double t = 0;
  if (FILE* f = Trace(&t)) {
    std::fprintf(f, "%.6f,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%u\n", t, kk::hfr::FrameTime(base),
                 kk::hfr::LoadF(base, kRawFrameTime), kk::hfr::LoadF(base, kDeviceFps), s, used,
                 kk::hfr::LoadF(base, kAlpha), reset);
  }
#else
  (void)object;
#endif
}
