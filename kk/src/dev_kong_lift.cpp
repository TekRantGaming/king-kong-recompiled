// Developer-only (task 325): KK_DEV_KONG_NOHIT=1 keeps Kong's grab-and-lift state
// (k_ETAT_grab_mashing, sub_8259D998) from being interrupted by hits: its call of k_exec_detect_paf
// (sub_8258A7D8, from 0x8259DF1C) reports no hit. The raptors at the start of "The Skull Islanders"
// otherwise knock Kong off the stone slab within about two seconds, so a scripted lift can be
// measured at any frame rate (hfr_kong_lift.cpp). Other callers are unchanged.
//
// KK_DEV_KONG_LIFT_ALLOW=1: the lift's completion test passes, so a lift can finish in a short
// scripted run. The test is a TrigTest_CapaBit callback (sub_82449F18, record at Kong's vars +1560)
// on the natives' controller KWC (record +4): its capability bit 1 (+40 of its extension), which
// KWC_Wait sets once enough natives are dealt with. The aid sets that bit while Kong is in the
// state (same place as above, before the test). The game's own condition, the same at any rate.
//
// KK_DEV_KONG_LIFT_TRACE=1 (with KK_DEV_SAMPLE_DIR): kong_lift.csv, one row per call of
// k_ETAT_grab_mashing after it ran: script clock, frame time, Kong's action, state (+92), start phase (+1196), lift position (+1900), speed (+1220), press boost
// (+1212), resistance (+1216), time falling (+1224), time in state (+104).
#if defined(KK_DEV_TOOLS)

#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/hook.h>

#include "hfr.h"

namespace {
constexpr uint32_t kGrabMashingReturn = 0x8259DF20;
bool Env(const char* name) {
  const char* v = std::getenv(name);
  return v && *v == '1';
}
const bool g_nohit = Env("KK_DEV_KONG_NOHIT");
const bool g_allow = Env("KK_DEV_KONG_LIFT_ALLOW");
}  // namespace

REX_EXTERN(__imp__sub_8258A7D8);
REX_HOOK_RAW(sub_8258A7D8) {
  if (g_allow && uint32_t(ctx.lr) == kGrabMashingReturn) {
    using namespace kk::hfr;
    auto ok = [](uint32_t a) { return a >= 0x40000000 && a < 0xA0000000; };
    const uint32_t ai = Load32(base, 0x82CCBF64);
    const uint32_t vars = ok(ai) ? Load32(base, ai + 64) : 0;
    const uint32_t kwc = ok(vars) ? Load32(base, vars + 1564) : 0;
    const uint32_t ext = ok(kwc) ? Load32(base, kwc + 24) : 0;
    if (ok(ext)) base[ext + 41] |= 1;  // big-endian u16 at +40: bit 1 is the low byte
  }
  if (g_nohit && uint32_t(ctx.lr) == kGrabMashingReturn) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8258A7D8(ctx, base);
}

namespace kk {
double DevSecondsSinceMenu();

void DevKongLiftTrace(uint8_t* base, uint32_t vars, uint32_t obj) {
  static const bool on = Env("KK_DEV_KONG_LIFT_TRACE");
  if (!on) return;
  using namespace kk::hfr;
  static FILE* f = [] {
    const char* d = std::getenv("KK_DEV_SAMPLE_DIR");
    FILE* out = std::fopen((std::string(d && *d ? d : ".") + "/kong_lift.csv").c_str(), "w");
    if (out) std::fprintf(out, "t,dt,action,state,starting,pos,speed,boost,resist,falling,in_state\n");
    return out;
  }();
  if (!f || !vars) return;
  auto ok = [](uint32_t a) { return a >= 0x40000000 && a < 0xA0000000; };
  const uint32_t b = ok(obj) ? Load32(base, obj + 20) : 0;
  const uint32_t a = ok(b) ? Load32(base, b) : 0;
  const uint32_t action = ok(a) ? Load16(base, a + 156) : 0xFFFF;
  std::fprintf(f, "%.4f,%.5f,%u,%d,%d,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f\n", DevSecondsSinceMenu(),
               LoadF(base, kFrameTime), action, int32_t(Load32(base, vars + 92)),
               int32_t(Load32(base, vars + 1196)), LoadF(base, vars + 1900), LoadF(base, vars + 1220),
               LoadF(base, vars + 1212), LoadF(base, vars + 1216), LoadF(base, vars + 1224),
               LoadF(base, vars + 104));
  std::fflush(f);
}
}  // namespace kk

#endif  // KK_DEV_TOOLS
