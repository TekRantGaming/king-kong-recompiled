// Camera shake (the V-Rex's footsteps, explosions, Kong's blows).
//
// The camera's effects script, CM_Sfx (sub_8245EC90), runs every frame. Its
// state is at [[0x82CCBF64] + 64]: a swaying shake with +32 on, +36 and +52 its
// two amplitudes, +40 and +56 their speeds, +44 how fast the amplitudes fall
// (per second), +48 what that rate is multiplied by, +60 the time; and a
// jitter with +160 on and +164 its size. Each frame the sway moves the camera,
// lowers both amplitudes by rate x frame time and multiplies the rate by +48;
// it ends when both amplitudes reach 0. SFX_ShakeCam_Capa's SCC_SFX
// (sub_82553F20) does the same with its state at +60 (on), +64, +80, +72, +76.
//
// The multiply happens once a frame, not once per 1/30 s. Above 30 fps the
// rate falls away faster while each frame lowers the amplitudes by less, so a
// strong shake can stop shrinking before it reaches 0 and then never ends
// (issue #39: in the V-Rex chapter the screen shook until the player died).
// After each call the rate is set to the old one times the multiplier ^ (frame
// time x 30), the Xbox 360's falloff at any frame rate.
//
// kk_camera_shake and kk_camera_shake_strength scale the amplitudes (and the
// jitter) for the call only, so a shake still lasts as long as the game meant.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "settings.h"

namespace kk {
namespace {

constexpr uint32_t kScriptContext = 0x82CCBF64;  // the running script function's object: [+64] its state
constexpr uint32_t kFrameTime = 0x82C636E0;      // seconds since the last frame

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

float LoadF(const uint8_t* base, uint32_t addr) {
  const uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void StoreF(uint8_t* base, uint32_t addr, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Store32(base, addr, v);
}

// Where one shake keeps its state (offsets from the state object).
struct Sway {
  const char* name;
  uint32_t on, a, b, rate, mult;
  uint32_t jitter_on, jitter;  // 0: none
};
constexpr Sway kCmSfx = {"CM_Sfx", 32, 36, 52, 44, 48, 160, 164};
constexpr Sway kScc = {"SCC_SFX", 60, 64, 80, 72, 76, 0, 0};

float Strength() {
  return REXCVAR_GET(kk_camera_shake) ? std::clamp(REXCVAR_GET(kk_camera_shake_strength), 0, 200) / 100.0f : 0.0f;
}

#if defined(KK_DEV_TOOLS)
// Developer aids: KK_DEV_SHAKE_TRACE=1 logs a shake twice a second while it is
// on; KK_DEV_SHAKE_ORIGINAL=1 leaves it as the game does it.
bool Tracing() {
  static const bool on = std::getenv("KK_DEV_SHAKE_TRACE") != nullptr;
  return on;
}
bool Original() {
  static const bool on = std::getenv("KK_DEV_SHAKE_ORIGINAL") != nullptr;
  return on;
}
void Trace(const Sway& w, const char* when, const uint8_t* base, uint32_t state, float dt) {
  static float since[2] = {1.0f, 1.0f};
  float& t = since[&w == &kScc];
  t += dt;
  if (!Tracing() || (t < 0.5f && std::strcmp(when, "on") == 0)) return;
  t = 0;
  REXLOG_INFO("KK dev shake {} {}: on {} amp {:.4f} / {:.4f}, rate {:.4f} x {:.4f}, jitter {} {:.4f}, dt {:.4f}", w.name,
              when, Load32(base, state + w.on), LoadF(base, state + w.a), LoadF(base, state + w.b),
              LoadF(base, state + w.rate), LoadF(base, state + w.mult), w.jitter_on ? Load32(base, state + w.jitter_on) : 0,
              w.jitter ? LoadF(base, state + w.jitter) : 0.0f, dt);
}
#endif

// Runs the game's function with the shake scaled, then redoes its bookkeeping
// on the real amplitudes, per second.
template <typename Fn>
void Shake(const Sway& w, PPCContext& ctx, uint8_t* base, Fn original) {
  const uint32_t context = Load32(base, kScriptContext);
  const uint32_t state = context ? Load32(base, context + 64) : 0;
  const bool sway = state && Load32(base, state + w.on);
  const bool jitter = state && w.jitter_on && Load32(base, state + w.jitter_on);
  if (!sway && !jitter) {
    original(ctx, base);
    return;
  }
  const float dt = LoadF(base, kFrameTime);
#if defined(KK_DEV_TOOLS)
  Trace(w, "on", base, state, dt);
  if (Original()) {
    original(ctx, base);
    if (sway && !Load32(base, state + w.on)) Trace(w, "end", base, state, dt);
    return;
  }
#endif
  const float s = Strength();
  const float a = LoadF(base, state + w.a), b = LoadF(base, state + w.b);
  const float rate = LoadF(base, state + w.rate), mult = LoadF(base, state + w.mult);
  const float j = jitter ? LoadF(base, state + w.jitter) : 0.0f;
  if (sway) {
    StoreF(base, state + w.a, a * s);
    StoreF(base, state + w.b, b * s);
  }
  if (jitter) StoreF(base, state + w.jitter, j * s);
  original(ctx, base);

  if (jitter && LoadF(base, state + w.jitter) == j * s) StoreF(base, state + w.jitter, j);
  if (!sway) return;
  // What the game's own step left (unless something started a new shake meanwhile).
  const float fall = rate * dt;
  const float sa = std::max(0.0f, a * s - fall), sb = std::max(0.0f, b * s - fall);
  const bool ours = LoadF(base, state + w.rate) == rate * mult && LoadF(base, state + w.a) == sa &&
                    LoadF(base, state + w.b) == sb;
  if (!ours) return;
  const float na = std::max(0.0f, a - fall), nb = std::max(0.0f, b - fall);
  StoreF(base, state + w.a, na);
  StoreF(base, state + w.b, nb);
  StoreF(base, state + w.rate, dt > 0 ? rate * std::pow(mult, dt * 30.0f) : rate);
  const bool over = na <= 0 && nb <= 0;
  Store32(base, state + w.on, over ? 0 : 1);
#if defined(KK_DEV_TOOLS)
  if (over) Trace(w, "end", base, state, dt);
#endif
}

}  // namespace
}  // namespace kk

REX_EXTERN(__imp__sub_8245EC90);
REX_HOOK_RAW(sub_8245EC90) { kk::Shake(kk::kCmSfx, ctx, base, __imp__sub_8245EC90); }

REX_EXTERN(__imp__sub_82553F20);
REX_HOOK_RAW(sub_82553F20) { kk::Shake(kk::kScc, ctx, base, __imp__sub_82553F20); }
