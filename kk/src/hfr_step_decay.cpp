// High frame rate fix: "decay per 0.016 s" factors (kk_hfr_fixes, "step_decay").
//
// Several scripts slow a velocity down with a factor computed like this
// (inlined in each, and as a helper, sub_8261AD78):
//     f = k; t = frame time; while (t > 0.016) { t -= 0.016; f *= k; }
//     velocity *= f;
// The first factor is always applied, so the decay is k once a frame plus once
// per 0.016 s beyond the first: on the console's 33/34 ms frames k^3 a frame,
// 90 times a second; at 60 FPS about 100, at 120 FPS 120, at 240 FPS 240. Above
// 30 FPS knock-back slides and swings die out sooner (a third shorter at 120
// FPS, two thirds at 240).
//
// The helper returns k ^ (90 x frame time). For the inlined copies the script's
// velocity is read before the call; if afterwards it is exactly that value
// times the game's factor (nothing else changed it this frame), it is set to
// that value times k ^ (90 x frame time) instead. The frame where the game
// used its own factor moves the object by a hair less; the velocity it keeps
// is the console's.
//
// Sites (the script's variables are [[0x82CCBF64] + 64] while it runs):
//   BC_TAG_On (sub_8263C930)              +7024, k 0.9 or 0.95
//   KBC_TAG_On (sub_82512FA8)             +7520, k 0.9 or 0.95
//   KBC_ETAT_Fight_BB_Poursuit (sub_8252E1E8) +1244, k 0.9
//   k_ETAT_grab_ANN (sub_8259F698)        +3332, k 0.95
//   CM_Kong_Mode_Swing_Horiz (sub_826245B8) +312, k 0.98 (Kong's swing camera)
//   k_ETAT_grab (sub_825CA350)            +928 (a scalar), k 0.9 or 0.95
// k_ETAT_grab also grows a vector (+980) by 1.005 per 1/60 s in whole steps
// rounded up (ceil(frame time x 60) times a frame: 70 a second on the
// console, 110 at 110 FPS); it is set to 1.005 ^ (70 x frame time) the same
// way. Its hook also applies the dt_floor fix (hfr_dt_floor.cpp).

#include <cmath>

#include <rex/hook.h>

#include "hfr.h"

namespace kk::hfr {
namespace {

constexpr uint32_t kScriptContext = 0x82CCBF64;  // the running script's AI instance: [+64] its variables

// The factor the game computes for this frame (the loop above, in float).
float GameFactor(float k, float dt) {
  constexpr float kStep = 0.016f;  // 0x820B0D1C
  float f = k;
  for (float t = dt; t > kStep; t -= kStep) f *= k;
  return f;
}

struct Site {
  uint32_t offset;
  float k[2];  // 0 = unused
};

template <typename Fn>
void Correct(const Site& site, PPCContext& ctx, uint8_t* base, Fn original) {
  const float dt = FrameTime(base);
  const uint32_t ai = Load32(base, kScriptContext);
  const uint32_t vars = ai >= 0x40000000 && ai < 0xA0000000 ? Load32(base, ai + 64) : 0;
  if (!On("step_decay") || !(dt > 0.0f) || !(vars >= 0x40000000 && vars < 0xA0000000)) {
    original(ctx, base);
    return;
  }
  const uint32_t at = vars + site.offset;
  float before[3];
  for (int i = 0; i < 3; ++i) before[i] = LoadF(base, at + i * 4);
  original(ctx, base);
  if (before[0] == 0.0f && before[1] == 0.0f && before[2] == 0.0f) return;
  for (const float k : site.k) {
    if (k == 0.0f) break;
    const float f = GameFactor(k, dt);
    bool ours = true;
    for (int i = 0; i < 3 && ours; ++i) ours = LoadF(base, at + i * 4) == before[i] * f;
    if (!ours) continue;
    const float want = std::pow(k, 90.0f * dt);
    for (int i = 0; i < 3; ++i) StoreF(base, at + i * 4, before[i] * want);
    Count("step_decay corrections");
    return;
  }
}

constexpr Site kBcTag = {7024, {0.9f, 0.95f}};
constexpr Site kKbcTag = {7520, {0.9f, 0.95f}};
constexpr Site kKbcPoursuit = {1244, {0.9f, 0.0f}};
constexpr Site kGrabAnn = {3332, {0.95f, 0.0f}};
constexpr Site kKongSwing = {312, {0.98f, 0.0f}};

// k_ETAT_grab's growth: f = 1; do { t -= 1/60; f *= 1.005; } while (t > 0).
float GrowthFactor(float dt) {
  constexpr float kStep = 0.01666f, kGrow = 1.005f;  // 0x820B24F0, 0x820B24EC
  float f = 1.0f;
  int n = 0;
  for (float t = dt; t > 0.0f && n < 10000; ++n) {
    t -= kStep;
    f *= kGrow;
  }
  return f;
}

uint32_t Vars(const uint8_t* base) {
  const uint32_t ai = Load32(base, kScriptContext);
  const uint32_t vars = ai >= 0x40000000 && ai < 0xA0000000 ? Load32(base, ai + 64) : 0;
  return vars >= 0x40000000 && vars < 0xA0000000 ? vars : 0;
}

}  // namespace

void WithRealStepFn(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*));

void KongGrab(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*)) {
  const float dt = FrameTime(base);
  const uint32_t vars = Vars(base);
  if (!On("step_decay") || !(dt > 0.0f) || !vars) {
    WithRealStepFn(ctx, base, original);
    return;
  }
  const float scalar = LoadF(base, vars + 928);
  float vec[3];
  for (int i = 0; i < 3; ++i) vec[i] = LoadF(base, vars + 980 + i * 4);
  WithRealStepFn(ctx, base, original);
  if (scalar != 0.0f) {
    for (const float k : {0.9f, 0.95f}) {
      if (LoadF(base, vars + 928) != scalar * GameFactor(k, dt)) continue;
      const float v = scalar * std::pow(k, 90.0f * dt);
      StoreF(base, vars + 928, v);
      Count("step_decay corrections");
      break;
    }
  }
  if (vec[0] != 0.0f || vec[1] != 0.0f || vec[2] != 0.0f) {
    const float g = GrowthFactor(dt);
    bool ours = true;
    for (int i = 0; i < 3 && ours; ++i) ours = LoadF(base, vars + 980 + i * 4) == vec[i] * g;
    if (ours) {
      const float want = std::pow(1.005f, 70.0f * dt);
      for (int i = 0; i < 3; ++i) StoreF(base, vars + 980 + i * 4, vec[i] * want);
      Count("step_decay corrections");
    }
  }
}

}  // namespace kk::hfr

REX_EXTERN(__imp__sub_825CA350);
REX_HOOK_RAW(sub_825CA350) { kk::hfr::KongGrab(ctx, base, __imp__sub_825CA350); }  // k_ETAT_grab

#define KK_STEP_DECAY_HOOK(addr, site)                                                         \
  REX_EXTERN(__imp__sub_##addr);                                                               \
  REX_HOOK_RAW(sub_##addr) { kk::hfr::Correct(kk::hfr::site, ctx, base, __imp__sub_##addr); }

KK_STEP_DECAY_HOOK(8263C930, kBcTag)        // BC_TAG_On
KK_STEP_DECAY_HOOK(82512FA8, kKbcTag)       // KBC_TAG_On
KK_STEP_DECAY_HOOK(8252E1E8, kKbcPoursuit)  // KBC_ETAT_Fight_BB_Poursuit
KK_STEP_DECAY_HOOK(8259F698, kGrabAnn)      // k_ETAT_grab_ANN
KK_STEP_DECAY_HOOK(826245B8, kKongSwing)    // CM_Kong_Mode_Swing_Horiz

// The helper: f1 = k in, k^(1 + steps) out.
REX_EXTERN(__imp__sub_8261AD78);
REX_HOOK_RAW(sub_8261AD78) {
  const float k = float(ctx.f1.f64);
  const float dt = kk::hfr::FrameTime(base);
  if (!kk::hfr::On("step_decay") || !(dt > 0.0f) || !(k > 0.0f)) {
    __imp__sub_8261AD78(ctx, base);
    return;
  }
  ctx.f1.f64 = double(std::pow(k, 90.0f * dt));
  kk::hfr::Count("step_decay helper");
}
