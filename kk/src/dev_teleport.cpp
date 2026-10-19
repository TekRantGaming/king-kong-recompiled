// Developer-only (task 224): move Jack with the game's own teleport trigger, to reach an encounter fast.
//
// KK_DEV_TELEPORT="t,x,y,z[,fx,fy]/t2,x,y,z..." (seconds after the chapter's level has loaded, the
// KK_DEV_SCRIPT clock): at each time, calls TrigExec_Teleport (sub_82538B70) once with a temporary
// trigger record {+4 Jack, +8 destination}, as dev_anim_view.cpp did for Ann's water (task 114). The
// destination's matrix gives the position (+48) and, negated, the forward axis (+16): (fx, fy) is
// where Jack faces (default north, +y). Jack is load32(0x82CCBF7C) (the camera's actor), checked to run
// the player track H_TRACK_tagoff (0x826718B8). Runs from the AI pass hook (dev_sample.cpp), before the
// AI step; the whole PPCContext and the guest scratch frame below r1 are restored afterwards.
// KK_DEV_TELEPORT="t,enemy[,stand-off]" approaches the nearest initialized living
// dormant allowlisted enemy (default 10 units away, range 3..30). Same-level,
// within 200 units and at most 2 units of elevation difference. It never wakes
// or schedules an enemy itself. Ground, triggers and line of sight need a run.
#if defined(KK_DEV_TOOLS)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"
#include "coop_enemies_game.h"
#include "coop_pose.h"

REX_EXTERN(__imp__sub_82538B70);

namespace kk {
double DevSecondsSinceMenu();

namespace {
using hfr::Load16;
using hfr::Load32;
using hfr::LoadF;
using hfr::Store32;
using hfr::StoreF;

struct Hop {
  double t;
  float x, y, z, fx, fy;
  bool done;
  bool enemy = false;
  float stand_off = 10;
  double retry = 0;
};

std::vector<Hop> Parse() {
  std::vector<Hop> out;
  const char* v = std::getenv("KK_DEV_TELEPORT");
  if (!v || !*v) return out;
  std::stringstream all(v);
  for (std::string s; std::getline(all, s, '/');) {
    Hop h{0, 0, 0, 0, 0, 1, false};
    const auto comma = s.find(',');
    if (comma != std::string::npos && s.compare(comma+1, 5, "enemy") == 0) {
      char tail = 0;
      const auto suffix = s.substr(comma+1);
      const bool valid_suffix = suffix == "enemy" ||
          std::sscanf(suffix.c_str(), "enemy,%f%c", &h.stand_off, &tail) == 1;
      if (valid_suffix && std::sscanf(s.substr(0, comma).c_str(), "%lf%c", &h.t, &tail) == 1 &&
          std::isfinite(h.t) && h.t >= 0 && std::isfinite(h.stand_off) && h.stand_off >= 3 && h.stand_off <= 30) {
        h.enemy = true; out.push_back(h);
      } else REXLOG_WARN("KK dev teleport: invalid enemy placement '{}'", s);
    } else if (std::sscanf(s.c_str(), "%lf,%f,%f,%f,%f,%f", &h.t, &h.x, &h.y, &h.z, &h.fx, &h.fy) >= 4) out.push_back(h);
  }
  return out;
}

bool Ok(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && (a & 3) == 0; }

}  // namespace

// KK_DEV_KILL="t,obj/t2,obj2" (same clock; obj = guest address of a raptor from sample.csv; the heap
// layout repeats between runs of the same start): sets its AI life (vars +20, float) to 0, as a lethal
// hit leaves it; the raptor's own states then see it (PNJ_Raptor states test +20 == 0).
void DevKillTick(uint8_t* base) {
  struct Kill { double t; uint32_t obj; bool done; };
  static std::vector<Kill> kills = [] {
    std::vector<Kill> out;
    const char* v = std::getenv("KK_DEV_KILL");
    if (!v || !*v) return out;
    std::stringstream all(v);
    for (std::string s; std::getline(all, s, '/');) {
      Kill k{0, 0, false};
      if (std::sscanf(s.c_str(), "%lf,%x", &k.t, &k.obj) == 2) out.push_back(k);
    }
    return out;
  }();
  if (kills.empty()) return;
  const double t = DevSecondsSinceMenu();
  for (Kill& k : kills) {
    if (k.done || t < k.t || !Ok(k.obj)) continue;
    k.done = true;
    const uint32_t ext = Load32(base, k.obj + 24), ai = Ok(ext) ? Load32(base, ext + 4) : 0;
    const uint32_t v = Ok(ai) ? Load32(base, ai + 64) : 0;
    if (!Ok(v)) continue;
    REXLOG_INFO("KK dev kill: at {:.2f} s object {:08X} life {} -> 0", t, k.obj, LoadF(base, v + 20));
    StoreF(base, v + 20, 0.0f);
  }
}

void DevTeleportTick(PPCContext* ctx, uint8_t* base, uint32_t world) {
  DevKillTick(base);
  static std::vector<Hop> hops = Parse();
  if (hops.empty() || !ctx) return;
  const double t = DevSecondsSinceMenu();
  if (t < 0) return;
  for (Hop& h : hops) {
    if (h.done || t < h.t) continue;
    if (h.enemy) {
      if (uint32_t(ctx->lr) != 0x82881CF0) continue;
      if (t > h.t+10) {
        h.done = true; REXLOG_WARN("KK dev teleport: enemy placement expired, no eligible placement"); continue;
      }
      if (t < h.retry) continue;
      h.retry = t+1;
    }
    const uint32_t actor = Load32(base, 0x82CCBF7C);
    if (!Ok(actor) || (Load32(base, actor) & 0x00400001u)) return;
    const uint32_t ext = Load32(base, actor + 24);
    if (!Ok(ext)) return;
    const uint32_t ai = Load32(base, ext + 4);
    if (!Ok(ai)) return;
    const uint32_t first = Load16(base, ai + 80);
    if (first > Load16(base, ai + 82) || first > 4096) return;
    const uint32_t block = Load32(base, ai + (first + 6) * 4);
    if (!Ok(block) || Load32(base, block + 20) != 0x826718B8) return;  // H_TRACK_tagoff: Jack
    const uint32_t m = Load32(base, actor + 12);
    if (!Ok(m)) return;
    if (h.enemy) {
      const auto jack = coop::ReadJackPose([&](uint32_t p) { return Load32(base, p); },
          [&](uint32_t p) { return Load16(base, p); }, [&](uint32_t p) { return LoadF(base, p); }, t);
      if (!jack.pose) continue;
      const auto& p = *jack.pose;
      const auto target = coop::EnemyDormantTarget(base, world, {p.x, p.y, p.z});
      if (!target) continue;
      const float dx = target->x-p.x, dy = target->y-p.y, distance = std::hypot(dx, dy);
      if (distance <= h.stand_off) {
        h.done = true; REXLOG_INFO("KK dev teleport: dormant enemy already within stand-off, no move"); continue;
      }
      h.fx = dx/distance; h.fy = dy/distance;
      h.x = target->x-h.fx*h.stand_off; h.y = target->y-h.fy*h.stand_off; h.z = target->z;
      REXLOG_INFO("KK dev teleport: synthetic enemy approach target=({:.3f},{:.3f},{:.3f}) stand-off={:.1f}",
          target->x, target->y, target->z, h.stand_off);
    }
    const uint32_t sp = ctx->r1.u32;
    if ((sp & 15) || sp < 0x40010200) return;
    h.done = true;
    const float bx = LoadF(base, m + 48), by = LoadF(base, m + 52), bz = LoadF(base, m + 56);
    const PPCContext saved = *ctx;
    const uint32_t start = sp - 512;
    uint8_t stack[512];
    std::memcpy(stack, base + start, sizeof(stack));
    std::memset(base + start, 0, sizeof(stack));
    const uint32_t trigger = start + 0x100, dest = start + 0x120, mat = start + 0x140;
    Store32(base, trigger + 4, actor);
    Store32(base, trigger + 8, dest);
    Store32(base, dest + 12, mat);
    const float len = std::sqrt(h.fx * h.fx + h.fy * h.fy);
    const float fx = len > 0 ? h.fx / len : 0, fy = len > 0 ? h.fy / len : 1;
    // Rows: x axis = right of forward, y axis = -forward (the getter negates it), z up.
    StoreF(base, mat + 0, fy);
    StoreF(base, mat + 4, -fx);
    StoreF(base, mat + 16, -fx);
    StoreF(base, mat + 20, -fy);
    StoreF(base, mat + 40, 1);
    StoreF(base, mat + 60, 1);
    StoreF(base, mat + 48, h.x);
    StoreF(base, mat + 52, h.y);
    StoreF(base, mat + 56, h.z);
    ctx->r1.u64 = start;
    ctx->r3.u64 = trigger;
    __imp__sub_82538B70(*ctx, base);
    std::memcpy(base + start, stack, sizeof(stack));
    *ctx = saved;
    const uint32_t m2 = Load32(base, actor + 12);
    REXLOG_INFO("KK dev teleport: at {:.2f} s Jack {:08X} ({:.2f},{:.2f},{:.2f}) -> ({:.2f},{:.2f},{:.2f}), now ({:.2f},{:.2f},{:.2f})",
                t, actor, bx, by, bz, h.x, h.y, h.z, Ok(m2) ? LoadF(base, m2 + 48) : 0.f,
                Ok(m2) ? LoadF(base, m2 + 52) : 0.f, Ok(m2) ? LoadF(base, m2 + 56) : 0.f);
  }
}

}  // namespace kk

#endif
