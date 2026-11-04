// High frame rate fix: a scripted fall that lands as at 30 FPS (kk_hfr_fixes, "land"; The Log, 4 Nov).
//
// H_exec_read_world (sub_826A1178) keeps the player's last floor point (vars +0xB30) while he touches the ground,
// and in the air counts the fall as that point's height over him (+0x33D4): past 8 units he dies (H_ETAT_IA_mort),
// unless a scripted sequence makes him immune (+0x3CBC). In The Log (chapter select entry 20, level 901) Kong throws
// the log and Jack falls about 375 units into the ravine while immune; behind the black screen the game puts him just
// over the ravine floor, still moving down by the frame's step. At 30 and 60 FPS that step reaches the floor in the
// same frame: the contact makes the floor point his position, and the fall is 0 when the sequence ends. At higher
// rates the step is shorter: he is left 0.5 to 0.8 units up (12.06 at 120 FPS, 12.37 at 165, the floor at 11.57),
// settles a few centimetres over the floor with no contact, and when the immunity ends (about 30 s into the chapter)
// the 375 unit fall kills him. The chapter starts again from the log, every time: the soft lock. Measured on 4 Nov:
// 60 and 70 FPS land, 120 sometimes, 165, 240 and unlimited never; the same with every other fix off.
//
// The fix: while the player is immune and in the air with a fall past the game's limit, a ray down from half a unit
// over him finds a floor at most 1 unit under his feet: his floor point becomes his position, as the 30 FPS landing
// makes it. A real fall is not changed: it is far over the ground when it passes the limit, and an immune player
// only meets this close to a floor.

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"

REX_EXTERN(__imp__sub_827FFFA8);  // ray_cast

namespace kk::hfr {
namespace {

constexpr uint32_t kPlayer = 0x82CCBF7C;     // the camera's actor
constexpr uint32_t kAiObject = 0x82CCBF6C;   // the object whose AI runs now
constexpr uint32_t kWorld = 0x828E8388;      // the current world
constexpr uint32_t kFallLimit = 0x820B0E24;  // 8.0: the fall that kills the player
constexpr uint32_t kFloorPoint = 0xB30, kImmune = 0x3CBC;
constexpr float kUnder = 1.0f;  // how far under his feet the floor may be

bool Valid(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && !(a & 3); }

// The player's AI variables, when the AI that runs now is his.
uint32_t PlayerVars(const uint8_t* base) {
  const uint32_t hero = Load32(base, kPlayer);
  if (!Valid(hero) || Load32(base, kAiObject) != hero) return 0;
  const uint32_t ext = Load32(base, hero + 24);
  const uint32_t ai = Valid(ext) ? Load32(base, ext + 4) : 0;
  const uint32_t vars = Valid(ai) ? Load32(base, ai + 64) : 0;
  return Valid(vars) && Load32(base, vars) != 0 ? vars : 0;  // word 0: a player's AI
}

// The height of the first floor the game's ray_cast meets going down from (x, y, z), over every collision object
// but the player (mode 16, as dev_ledge.cpp), or NAN.
float FloorUnder(PPCContext& ctx, uint8_t* base, uint32_t hero, float x, float y, float z, float len) {
  const uint32_t world = Load32(base, kWorld);
  if (!Valid(world)) return NAN;
  const PPCContext saved = ctx;
  constexpr uint32_t kFrame = 1024;
  const uint32_t frame = (ctx.r1.u32 - kFrame) & ~15u;
  static uint8_t keep[kFrame];
  std::memcpy(keep, base + frame, kFrame);
  std::memset(base + frame, 0, kFrame);
  const uint32_t start = frame + 0x200, dir = frame + 0x210;
  StoreF(base, start, x), StoreF(base, start + 4, y), StoreF(base, start + 8, z);
  StoreF(base, dir, 0), StoreF(base, dir + 4, 0), StoreF(base, dir + 8, -1);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = world;
  ctx.r4.u64 = hero;
  ctx.r5.u64 = start;
  ctx.r6.u64 = dir;
  ctx.f1.f64 = len;
  ctx.r8.u64 = 0;
  ctx.r9.u64 = 0;
  ctx.r10.u64 = 16;
  __imp__sub_827FFFA8(ctx, base);
  const float floor = ctx.r3.u32 != 0 ? LoadF(base, world + 0x16274) : NAN;
  std::memcpy(base + frame, keep, kFrame);
  ctx = saved;
  return floor;
}

}  // namespace

void LandBeforeGround(PPCContext& ctx, uint8_t* base) {
  if (!On("land")) return;
  const uint32_t vars = PlayerVars(base);
  if (!vars || Load32(base, vars + kImmune) == 0) return;
  const uint32_t hero = Load32(base, kPlayer);
  const uint32_t matrix = Load32(base, hero + 12);
  if (!Valid(matrix)) return;
  const float x = LoadF(base, matrix + 48), y = LoadF(base, matrix + 52), z = LoadF(base, matrix + 56);
  const float fall = LoadF(base, vars + kFloorPoint + 8) - z;
  if (!(fall > LoadF(base, kFallLimit))) return;
  const float floor = FloorUnder(ctx, base, hero, x, y, z + 0.5f, 0.5f + kUnder);
  if (!std::isfinite(floor) || floor < z - kUnder) return;
  StoreF(base, vars + kFloorPoint, x);
  StoreF(base, vars + kFloorPoint + 4, y);
  StoreF(base, vars + kFloorPoint + 8, z);
  Count("land");
  REXLOG_INFO("KK hfr: land: a {:.1f} unit fall counted as landed, {:.2f} units over the floor ({:.2f}, {:.2f}, {:.2f})",
              fall, z - floor, x, y, z);
}

}  // namespace kk::hfr
