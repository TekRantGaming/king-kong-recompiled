// High frame rate fix: the camera's ground tilt at a landing, as at 30 FPS (kk_hfr_fixes, "cam_ground"; task 163).
//
// CM_Cam (sub_8246F180) has a ground branch (82471528..824715C8, while the camera manager's +228 is 0 and the
// hero has ground contact): it turns the camera's forward z toward the contact plane by
//   alpha = clamp(0.2 * speed * dt, 0, 1)
// where speed is the hero's script value +11668 (hero state = [[hero +24] +4] +64), written by the hero's AI
// (826C1478) as |previous position - position| / dt. That previous position is one frame old, so
// speed * dt is the distance the hero moved in the frame BEFORE: the camera follows the hero's motion one frame
// late. While walking that is harmless (the turn per second is the same at any rate). At a landing it is not:
// the first frame with ground contact still turns the camera by the last airborne step, and the turn stays once
// the hero stops (speed 0). Chapter starts drop the hero a few centimetres onto the floor (The Wall, Swamps,
// In the Mud, ...), so the camera keeps a small pitch. The Wall (task 163, forward z): 0.0037-0.0048 at 30 FPS,
// 0.0030 at 60, 0.0010-0.0019 at 120 (up to 3 pixels lower at 1280x720); fixed 0.0048 at 60, 0.0045-0.0049
// at 120, 0.0047 at 240, 0.0061 at 120 D3D12 (a 24 ms landing frame). In Swamps and In the Mud the first contact
// frame is the 63 ms loading frame at every rate, so they matched anyway; Venatosaurus 0.24 px -> 0.01 px.
//
// Over the frames with contact the original turns by 0.2 x (the hero's path over the last airborne frame and
// the landing step): one to two frames, 0.033-0.067 s of the drop at 30, 0.008-0.017 s at 120. The fix keeps
// the hero's path over game time (the running sum of the game's own speed * dt, each step placed at the end of
// the frame it was made in) and gives the branch speed' = (path(t - 1/30) - path(t_prev - 1/30)) / dt: the
// camera follows the hero one 30 FPS frame late, as it does at 30 (where this is the original), instead of one
// frame late. Walking turns the camera as before (the same distances, at most 1/30 s later on a turn that
// takes seconds). The landing step itself is still one frame of the drop (its share is random at 30 too).
// The hero's own speed is put back before the camera's orientation is applied (824715F8), so nothing else
// reads the changed value.

#include <algorithm>
#include <cmath>
#include <deque>

#include <rex/hook.h>

#include "hfr.h"
#include "coop_nametag.h"
#include "coop_observer.h"
#include "coop_objects_game.h"

#if defined(KK_DEV_TOOLS)
namespace kk {
void DevCameraGround(const uint8_t* base, uint32_t hero, uint32_t out, float speed_game, float speed_used);
void DevFacePartnerCamera(PPCContext& ctx, uint8_t* base);
}
#endif

namespace {
using kk::hfr::Load32;
using kk::hfr::LoadF;
using kk::hfr::StoreF;

bool Valid(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && !(a & 3); }

uint32_t HeroState(const uint8_t* base, uint32_t hero) {
  if (!Valid(hero)) return 0;
  const uint32_t ext = Load32(base, hero + 24);
  const uint32_t ai = Valid(ext) ? Load32(base, ext + 4) : 0;
  const uint32_t state = Valid(ai) ? Load32(base, ai + 64) : 0;
  return Valid(state) ? state : 0;
}

double GameTime(const uint8_t* base) { return double(LoadF(base, 0x82CCAE1C)) - LoadF(base, 0x82CCC02C); }

struct Sample {
  double t, path;
};

struct State {
  std::deque<Sample> history;  // (game time, running sum of speed * dt) per CM_Cam frame
  uint32_t hero = 0;
  double path = 0, last_t = 0, delayed = 0;
  float speed_used = 0;
  bool ready = false;
  uint32_t saved_addr = 0;  // the hero's speed while the branch sees speed'
  uint32_t saved_bits = 0;
} g;

double PathAt(double t) {
  const auto& h = g.history;
  if (h.empty()) return g.path;
  if (t <= h.front().t) return h.front().path;
  for (size_t i = 1; i < h.size(); ++i) {
    if (t <= h[i].t) {
      const double span = h[i].t - h[i - 1].t;
      const double f = span > 0 ? (t - h[i - 1].t) / span : 1.0;
      return h[i - 1].path + f * (h[i].path - h[i - 1].path);
    }
  }
  return h.back().path;
}

void Restore(uint8_t* base) {
  if (g.saved_addr) kk::hfr::Store32(base, g.saved_addr, g.saved_bits);
  g.saved_addr = 0;
}

// Once per CM_Cam call: record this frame's distance and work out the delayed one.
void Frame(uint8_t* base) {
  const uint32_t script = Load32(base, 0x82CCBF64);
  const uint32_t cm = Valid(script) ? Load32(base, script + 64) : 0;
  const uint32_t hero = Valid(cm) ? Load32(base, cm) : 0;
  const uint32_t state = HeroState(base, hero);
  const double t = GameTime(base);
  const float dt = kk::hfr::FrameTime(base);
  if (!state || !std::isfinite(t) || !std::isfinite(dt) || dt < 0) {
    g.ready = false;
    return;
  }
  const float speed = LoadF(base, state + 11668);
  if (!g.ready || hero != g.hero || t < g.last_t) {
    g.history.clear();
    g.hero = hero;
    g.path = 0;
    g.history.push_back({t - std::max(0.0f, dt), 0});
    g.last_t = t;
    g.delayed = 0;
    g.ready = true;
  } else if (t > g.last_t) {
    // This frame's speed * dt is the hero's step in the previous frame, which ended at last_t.
    const double step = std::isfinite(speed) ? std::max(0.0, double(speed) * dt) : 0.0;
    g.path += step;
    g.history.push_back({g.last_t, g.path});
    g.last_t = t;
    while (g.history.size() > 2 && g.history[1].t < t - 0.25) g.history.pop_front();
  }
  // The original reads the step of the frame before (at 30: the hero's path from t - 2/30 to t - 1/30).
  // Read the path one 30 FPS frame late instead: from t_prev - 1/30 to t - 1/30.
  const double now = std::max(g.delayed, PathAt(t - 1.0 / 30.0));
  const double moved = now - g.delayed;
  g.delayed = now;
  g.speed_used = dt > 0 ? float(moved / dt) : speed;
}
}  // namespace

REX_EXTERN(__imp__sub_8246F180);
REX_HOOK_RAW(sub_8246F180) {
  const bool on = kk::hfr::On("cam_ground");
  if (on) Frame(base);
  else g.ready = false;
  __imp__sub_8246F180(ctx, base);
  Restore(base);
}

// The ground branch's contact query; the branch reads the hero's speed right after it (824715A0).
REX_EXTERN(__imp__sub_824008D0);
REX_HOOK_RAW(sub_824008D0) {
  const uint32_t hero = ctx.r3.u32, out = ctx.r5.u32;
  const bool camera = uint32_t(ctx.lr) == 0x82471560;
  __imp__sub_824008D0(ctx, base);
  if (!camera) return;
  const uint32_t state = HeroState(base, hero);
  const float game = state ? LoadF(base, state + 11668) : 0.0f;
  float used = game;
  if (state && g.ready && hero == g.hero && kk::hfr::On("cam_ground") && !g.saved_addr) {
    g.saved_addr = state + 11668;
    g.saved_bits = Load32(base, g.saved_addr);
    used = g.speed_used;
    StoreF(base, g.saved_addr, used);
    kk::hfr::Count("cam_ground");
  }
#if defined(KK_DEV_TOOLS)
  kk::DevCameraGround(base, hero, out, game, used);
#else
  (void)out;
#endif
}

// First call after the ground branch (824715F8): orient the camera. The branch has used speed' by now.
REX_EXTERN(__imp__sub_82711328);
REX_HOOK_RAW(sub_82711328) {
  const uint32_t actor = ctx.r3.u32, caller = uint32_t(ctx.lr);
  if (g.saved_addr && uint32_t(ctx.lr) == 0x824715FC) Restore(base);
#if defined(KK_DEV_TOOLS)
  kk::DevFacePartnerCamera(ctx, base);
#endif
  __imp__sub_82711328(ctx, base);
  kk::coop::nametag::NormalCamera(base, actor, caller);
  kk::coop::observer::NormalCamera(base, actor, caller);
#if defined(KK_DEV_TOOLS)
  kk::coop::ObjectsDevCamera(ctx,base,actor,caller);
#endif
}
