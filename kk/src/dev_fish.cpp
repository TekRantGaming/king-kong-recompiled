// Developer-only: what the swimming water creatures (PNJ_EatMe_ETAT_Water,
// sub_8243B2C8; the fish in Ann's chapter) do each call, to compare frame
// rates (the high frame rate fix "fish", hfr_fish.cpp).
//
// KK_DEV_FISH="file.csv[,from,to]" (seconds on the script clock, see
// dev_tools.cpp; default 0..600): one row per call of the state function:
//   t (script clock), game_s (the game's clock: 0x82CCAE1C - 0x82CCC02C),
//   dt (frame time), obj (the creature), state0/state1 (its variables +56
//   before/after), ret (the state function's result), alpha (its object +28),
//   phase0/phase1 (the swim phase, +116, before/after), speed (+112),
//   x y z (its position), fx fy fz (its facing, as sub_8270FA58 reads it),
//   wiggle_game / wiggle_used (the wiggle angle the game asked for and the
//   one used; empty when the call did not wiggle), modules (+44), then the
//   first and last body module's positions.
// The observer only reads guest memory around the original call.
//
// KK_DEV_FISH_OFF_UNTIL=<seconds>: the fix "fish" stays off until then on the script clock, so one run
// measures the game's own wiggle and then the fixed one at the same frame rate.
//
// KK_DEV_FISH_VIEW=ann-water: once the script clock reaches 2 s in Ann's
// chapter (chapter-select entry 9, the player still near its start), moves the
// player to (204, 173, 6.1) facing -Y with the game's own teleport trigger
// (sub_82538B70), close enough to the fish that they are shown and swim (far
// away they are faded out and their state function does nothing). The same
// place as task 114's synthetic view, for comparable runs.

#if defined(KK_DEV_TOOLS)

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <xmmintrin.h>

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"

namespace kk {
double DevSecondsSinceMenu();  // dev_tools.cpp
void DevFishWiggle(float game_angle, float used_angle);
bool DevFishFixAllowed();

namespace {

using kk::hfr::Load32;
using kk::hfr::LoadF;

constexpr uint32_t kScriptContext = 0x82CCBF64;  // the running script's AI instance: [+64] its variables
constexpr uint32_t kCurrentObject = 0x82CCBF6C;  // the object whose script runs
constexpr uint32_t kGameClock = 0x82CCAE1C;
constexpr uint32_t kGameClockBase = 0x82CCC02C;

bool Valid(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000; }

struct Settings {
  FILE* file = nullptr;
  double from = 0, to = 600;
};

Settings& Get() {
  static Settings s = [] {
    Settings s;
    const char* v = std::getenv("KK_DEV_FISH");
    if (!v || !*v) return s;
    std::stringstream in(v);
    std::string path, a, b;
    std::getline(in, path, ',');
    if (std::getline(in, a, ',') && !a.empty()) s.from = std::stod(a);
    if (std::getline(in, b, ',') && !b.empty()) s.to = std::stod(b);
    s.file = std::fopen(path.c_str(), "w");
    if (!s.file) {
      REXLOG_WARN("KK dev fish: cannot write {}", path);
      return s;
    }
    std::fprintf(s.file,
                 "t,game_s,dt,obj,state0,state1,ret,alpha,phase0,phase1,speed,x,y,z,fx,fy,fz,wiggle_game,"
                 "wiggle_used,modules,m0x,m0y,m0z,mnx,mny,mnz\n");
    REXLOG_INFO("KK dev fish: recording to {} from {} to {} s", path, s.from, s.to);
    return s;
  }();
  return s;
}

float g_wiggle_game = std::numeric_limits<float>::quiet_NaN();
float g_wiggle_used = std::numeric_limits<float>::quiet_NaN();

// An object's position and facing from its matrix (object +12: rows at +0/+16/+32, position at +48).
bool Pose(const uint8_t* base, uint32_t obj, float p[3], float f[3]) {
  if (!Valid(obj)) return false;
  const uint32_t m = Load32(base, obj + 12);
  if (!Valid(m)) return false;
  for (int i = 0; i < 3; ++i) {
    p[i] = LoadF(base, m + 48 + 4 * i);
    if (f) f[i] = -LoadF(base, m + 16 + 4 * i);  // sub_8270FA58 returns the second row negated
  }
  return true;
}

}  // namespace

bool DevFishFixAllowed() {
  static const double until = [] {
    const char* v = std::getenv("KK_DEV_FISH_OFF_UNTIL");
    return v && *v ? std::atof(v) : -1.0;
  }();
  return until < 0 || DevSecondsSinceMenu() >= until;
}

void DevFishWiggle(float game_angle, float used_angle) {
  g_wiggle_game = game_angle;
  g_wiggle_used = used_angle;
}

}  // namespace kk

REX_EXTERN(__imp__sub_8243B2C8);
REX_HOOK_RAW(sub_8243B2C8) {
  using namespace kk;
  Settings& s = Get();
  const double t = s.file ? DevSecondsSinceMenu() : -1;
  if (!s.file || t < s.from || t > s.to) {
    __imp__sub_8243B2C8(ctx, base);
    return;
  }
  const uint32_t obj = Load32(base, kCurrentObject);
  const uint32_t ai = Load32(base, kScriptContext);
  const uint32_t vars = Valid(ai) ? Load32(base, ai + 64) : 0;
  if (!Valid(obj) || !Valid(vars)) {
    __imp__sub_8243B2C8(ctx, base);
    return;
  }
  const int32_t state0 = int32_t(Load32(base, vars + 56));
  const float phase0 = LoadF(base, vars + 116);
  g_wiggle_game = g_wiggle_used = std::numeric_limits<float>::quiet_NaN();
  __imp__sub_8243B2C8(ctx, base);
  const int32_t ret = int32_t(ctx.r3.u32);
  float p[3] = {}, f[3] = {}, m0[3] = {}, mn[3] = {};
  Pose(base, obj, p, f);
  const int32_t modules = int32_t(Load32(base, vars + 44));
  if (modules > 0 && modules < 64) {
    Pose(base, Load32(base, vars + 148), m0, nullptr);
    Pose(base, Load32(base, vars + 148 + 4 * uint32_t(modules - 1)), mn, nullptr);
  }
  auto num = [](float v) { return std::isnan(v) ? std::string() : std::to_string(v); };
  std::fprintf(s.file, "%.4f,%.6f,%.7f,%08X,%d,%d,%d,%u,%.7f,%.7f,%.6f,%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%s,%s,%d,"
               "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n",
               t, LoadF(base, kGameClock) - LoadF(base, kGameClockBase), kk::hfr::FrameTime(base), obj, state0,
               int32_t(Load32(base, vars + 56)), ret, unsigned(base[obj + 28]), phase0, LoadF(base, vars + 116),
               LoadF(base, vars + 112), p[0], p[1], p[2], f[0], f[1], f[2], num(g_wiggle_game).c_str(),
               num(g_wiggle_used).c_str(), modules, m0[0], m0[1], m0[2], mn[0], mn[1], mn[2]);
  static int rows = 0;
  if (++rows % 256 == 0) std::fflush(s.file);
}

// The engine's per-world call; the view preset runs after it, between frames' AI.
REX_EXTERN(__imp__sub_82538B70);  // the game's teleport trigger: (trigger) -> [+4] object to [+8] destination
REX_EXTERN(__imp__sub_82881B70);
REX_HOOK_RAW(sub_82881B70) {
  __imp__sub_82881B70(ctx, base);
  using namespace kk;
  static const bool wanted = [] {
    const char* v = std::getenv("KK_DEV_FISH_VIEW");
    return v && std::strcmp(v, "ann-water") == 0;
  }();
  static bool done = false;
  if (!wanted || done || DevSecondsSinceMenu() < 2.0) return;
  done = true;
  const uint32_t actor = Load32(base, 0x82CCBF7C);  // the player
  const uint32_t matrix = Valid(actor) ? Load32(base, actor + 12) : 0;
  const uint32_t sp = ctx.r1.u32;
  // Only the plain transform path (no Gao indirection, no hierarchy), and only at Ann's start.
  if (!Valid(actor) || (Load32(base, actor) & 0x00400001u) || !Valid(matrix) || (sp & 15) || sp < 0x40010000 ||
      std::fabs(LoadF(base, matrix + 48) - 214.3f) > 3 || std::fabs(LoadF(base, matrix + 52) - 190.2f) > 3) {
    REXLOG_INFO("KK dev fish view: skipped (player not at Ann's start)");
    return;
  }
  const unsigned fp = _mm_getcsr();
  REXLOG_INFO("KK dev fish view: SYNTHETIC player {:08X} from ({},{},{}) to (204,173,6.1) facing -Y", actor,
              LoadF(base, matrix + 48), LoadF(base, matrix + 52), LoadF(base, matrix + 56));
  {
    // Scratch frame below the caller's stack: the trigger, destination object and its matrix.
    const PPCContext saved = ctx;
    const uint32_t start = sp - 1024;
    std::array<uint8_t, 1024> stack{};
    std::memcpy(stack.data(), base + start, stack.size());
    std::memset(base + start, 0, 512);
    const uint32_t trigger = start + 0x100, destination = start + 0x120, transform = start + 0x140;
    kk::hfr::Store32(base, trigger + 4, actor);
    kk::hfr::Store32(base, trigger + 8, destination);
    kk::hfr::Store32(base, destination + 12, transform);
    for (uint32_t i : {0u, 20u, 40u, 60u}) kk::hfr::StoreF(base, transform + i, 1);
    kk::hfr::StoreF(base, transform + 48, 204);
    kk::hfr::StoreF(base, transform + 52, 173);
    kk::hfr::StoreF(base, transform + 56, 6.1f);
    ctx.r1.u64 = start;
    ctx.r3.u64 = trigger;
    __imp__sub_82538B70(ctx, base);
    std::memcpy(base + start, stack.data(), stack.size());
    ctx = saved;
  }
  _mm_setcsr(fp);
  const uint32_t after = Load32(base, actor + 12);
  if (Valid(after))
    REXLOG_INFO("KK dev fish view: player now at ({},{},{})", LoadF(base, after + 48), LoadF(base, after + 52),
                LoadF(base, after + 56));
}

#endif  // KK_DEV_TOOLS
