// Game fix (task 224, Beta 10): a raptor that stalls on its path is put back on it.
//
// In the chapter Hayes (level 303, from checkpoint 5) the third raptor (the one that "goes around" to the
// sailor) can stop for good on a ledge at about (214.5, 73.1) in PNJ_Raptor_ETAT_VALA (sub_825E06B0, "go
// there"): its path list (vars +7028, count +6776) keeps being rebuilt between 1 and 9 waypoints whose next
// one (+7152) it never walks to, so it stands still forever, out of the player's reach, and Hayes waits for
// all three raptors to die (a CapaBit test on each). The game's own code is deterministic; whether the
// raptor takes that route depends on where Jack is when it wakes (seen in 2 of 3 runs at 60 FPS and on the
// player's Ally at 30 and 60 FPS). The path finder and the mover disagree about that link; this is the
// original game's logic, not a frame-rate effect, and the AI step is not changed.
//
// Only in level 303 (G+8, G = [0x82CC97C0]). The fix watches each raptor while it runs ETAT_VALA (vars = [[0x82CCBF64]+64], the object = [0x82CCBF6C]):
// alive (+20 > 0), a path to follow (+6776 > 0) and not moving (less than 0.3 m from where it stood) for
// 8 s of game time. Then it does what the game's own next-waypoint code does for a warp link
// (PNJ_Raptor_exec_way_next_wp_get, 0x82569494): it moves the raptor onto its next waypoint with the
// position setter sub_827117E0 and the controller callback sub_82401AA8, and the AI carries on from there.
// If the same raptor stalls a third time, it is given a lethal hit instead (life +20 = 0; ETAT_VALA sends a
// dead raptor to its death state), so the level can always go on. Each step is logged ("KK fix: raptor").
// Developer A/B: environment KK_RAPTOR_RESCUE=0 turns it off.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"

REX_EXTERN(__imp__sub_825E06B0);  // PNJ_Raptor_ETAT_VALA
REX_EXTERN(__imp__sub_8270FCE0);  // a game object's position -> vec3
REX_EXTERN(__imp__sub_827117E0);  // set an actor's position
REX_EXTERN(__imp__sub_82401AA8);  // the actor's controller follows its new position

namespace kk {
namespace {
using hfr::Load32;
using hfr::LoadF;
using hfr::StoreF;

constexpr uint32_t kAiPtr = 0x82CCBF64, kCurrentObject = 0x82CCBF6C, kGamePtr = 0x82CC97C0;
constexpr uint32_t kHayesLevel = 303;  // G+8: the level key last loaded (chapter Hayes)
constexpr float kStillSeconds = 8.0f, kStillDistance = 0.3f;

bool Ok(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && (a & 3) == 0; }

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("KK_RAPTOR_RESCUE");
    return !(v && *v == '0');
  }();
  return on;
}

struct Watch {
  float x = 0, y = 0, z = 0, still = 0;
  int rescues = 0;
  std::chrono::steady_clock::time_point seen{};
};
std::unordered_map<uint32_t, Watch> g_watch;

bool Position(uint8_t* base, uint32_t obj, float& x, float& y, float& z) {
  const uint32_t m = Load32(base, obj + 12);
  if (!Ok(m)) return false;
  x = LoadF(base, m + 48), y = LoadF(base, m + 52), z = LoadF(base, m + 56);
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

// Calls fn(r3 = a, r4 = a guest vec3 scratch) on a scratch frame below the caller's stack, restoring the
// context and the scratch bytes afterwards. Returns the vec3 (it may have been written by fn).
void CallWithVec(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t a, float v[3], bool in) {
  const PPCContext saved = ctx;
  const uint32_t start = (ctx.r1.u32 - 512) & ~15u;
  uint8_t keep[512];
  std::memcpy(keep, base + start, sizeof(keep));
  const uint32_t vec = start + 0x100;
  if (in)
    for (int i = 0; i < 3; ++i) StoreF(base, vec + i * 4, v[i]);
  ctx.r1.u64 = start;
  ctx.r3.u64 = a;
  ctx.r4.u64 = vec;
  fn(ctx, base);
  for (int i = 0; i < 3; ++i) v[i] = LoadF(base, vec + i * 4);
  std::memcpy(base + start, keep, sizeof(keep));
  ctx = saved;
}

void Check(PPCContext& ctx, uint8_t* base) {
  // Only the Hayes encounter (level 303): every other raptor fight is left exactly as it was.
  const uint32_t g = Load32(base, kGamePtr);
  if (!Ok(g) || Load32(base, g + 8) != kHayesLevel) return;
  const uint32_t obj = Load32(base, kCurrentObject), ai = Load32(base, kAiPtr);
  if (!Ok(obj) || !Ok(ai)) return;
  const uint32_t v = Load32(base, ai + 64);
  if (!Ok(v)) return;
  float x, y, z;
  if (!Position(base, obj, x, y, z)) return;
  const auto now = std::chrono::steady_clock::now();
  Watch& w = g_watch[obj];
  const bool fresh = w.seen == std::chrono::steady_clock::time_point{} || now - w.seen > std::chrono::milliseconds(500);
  w.seen = now;
  const float life = LoadF(base, v + 20);
  const int32_t path = int32_t(Load32(base, v + 6776));
  const float d = std::sqrt((x - w.x) * (x - w.x) + (y - w.y) * (y - w.y) + (z - w.z) * (z - w.z));
  if (fresh || !(life > 0) || path <= 0 || d > kStillDistance) {
    w.x = x, w.y = y, w.z = z, w.still = 0;
    return;
  }
  const float dt = hfr::FrameTime(base);
  if (std::isfinite(dt) && dt > 0 && dt < 0.25f) w.still += dt;
  if (w.still < kStillSeconds) return;
  w.still = 0;
  ++w.rescues;
  const uint32_t next = Load32(base, v + 7152);
  if (w.rescues <= 2 && Ok(next) && next != obj) {
    float p[3] = {};
    CallWithVec(ctx, base, __imp__sub_8270FCE0, next, p, false);
    if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) &&
        std::fabs(p[0] - x) + std::fabs(p[1] - y) + std::fabs(p[2] - z) < 60.0f) {
      CallWithVec(ctx, base, __imp__sub_827117E0, obj, p, true);
      CallWithVec(ctx, base, __imp__sub_82401AA8, obj, p, true);
      Position(base, obj, w.x, w.y, w.z);
      REXLOG_INFO("KK fix: raptor {:08X} stood still on its path for {:.0f} s at ({:.2f},{:.2f},{:.2f}) with {} "
                  "waypoints left; moved it onto its next waypoint {:08X} ({:.2f},{:.2f},{:.2f}) (rescue {})",
                  obj, kStillSeconds, x, y, z, path, next, p[0], p[1], p[2], w.rescues);
      return;
    }
  }
  StoreF(base, v + 20, 0.0f);
  REXLOG_INFO("KK fix: raptor {:08X} still stuck on its path at ({:.2f},{:.2f},{:.2f}) after {} moves; "
              "given a lethal hit so the level can go on", obj, x, y, z, w.rescues - 1);
}

}  // namespace
}  // namespace kk

REX_HOOK_RAW(sub_825E06B0) {
  if (kk::Enabled()) kk::Check(ctx, base);
  __imp__sub_825E06B0(ctx, base);
}
