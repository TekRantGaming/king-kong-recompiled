// High frame rate fix: walking up ledges as at 30 FPS (kk_hfr_fixes, "step_up"; issue 49).
//
// The game has no step up logic. The collision (obj_collide, contact_resolve) tests a sphere at the end of the
// frame's move, radius 0.26 for Jack. A floor contact lifts it, a wall contact pushes it back. At a step whose top
// is higher than the sphere's middle the wall's face is met first, and the sphere only gets the top surface in reach
// by sinking into the wall in one frame. That takes a set depth:
//   depth = r - sqrt(r^2 - h^2), h = the height of the top over the sphere's middle
// A frame's move is 0.1 units at 30 FPS and 0.025 at 120, so there is a band of steps that a player runs up at 30 FPS
// and that stops him dead above it: the wall pushes him back each frame before he is deep enough, however long he
// holds the stick. The Wall, the bank out of the water (level 204, 106.9,66.9): depth 0.07, the step fails at 60 FPS
// and above, and now and then a long frame at 120 gets over it. An earlier frame rate fix is not the cause: it is
// the same with KK_DEV_HFR_OFF=all, only the frame time matters.
//
// The fix gives the sphere the 30 FPS depth without changing how anything moves. While the player (the camera's
// actor) pressed into a wall in the frame before, then once per 1/30 s of game time its move into that wall is made
// as long as one 1/30 s frame's, before the collision runs. Nothing but the part along the wall's normal grows, so
// sliding along a wall keeps its speed. With no top surface in reach the wall pushes the sphere straight back in the
// same collision step, so the extra depth is never seen and the player stands where he stood. Otherwise the floor
// contact lifts him as at 30 FPS. The sweep's old position is the body's own and stays as it was.

#include <cmath>
#include <cstdint>

#include <rex/hook.h>

#include "hfr.h"

#if defined(KK_DEV_TOOLS)
namespace kk {
void DevContactRow(uint8_t* base, uint32_t a, uint32_t n, uint32_t push, uint32_t flags, uint32_t result);
}
#endif

namespace kk::hfr {
namespace {

constexpr uint32_t kSweep = 0x828E89D0;  // the sweep record: its word 8 is the moving object
constexpr uint32_t kPlayer = 0x82CCBF7C;  // the camera's actor
constexpr uint32_t kContactWall = 0x80, kContactDropped = 0x80001000;
constexpr float kStep = 1.0f / 30.0f;
constexpr float kSlack = 0.0005f;       // frame time jitter allowed
constexpr float kWallNormalZ = 0.7f;    // a steeper normal is a floor or a ceiling
constexpr float kMaxSpeed = 15.0f;      // units a second: a faster move is a teleport or a respawn, not a walk

bool Valid(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && !(a & 3); }

struct State {
  float move_x = 0, move_y = 0;  // the player's last move
  float wall_x = 0, wall_y = 0;  // the flat normal of the wall he pressed into most
  float wall_into = 0;           // how much the last move went into it (negative)
  bool wall = false;
  float since = 0;  // game time since the last long move
} g_state;

// From the wall contacts of the last collision step: the wall his move went into most.
void NoteContact(const uint8_t* base, uint32_t normal, uint32_t flags, uint32_t result) {
  if (result == 0 || (Load32(base, flags) & (kContactWall | kContactDropped)) != kContactWall) return;
  if (Load32(base, kSweep + 8) != Load32(base, kPlayer)) return;
  const float nz = LoadF(base, normal + 8);
  if (std::fabs(nz) >= kWallNormalZ) return;
  const float nx = LoadF(base, normal), ny = LoadF(base, normal + 4);
  const float len = std::hypot(nx, ny);
  if (len < 0.01f) return;
  const float fx = nx / len, fy = ny / len;
  const float into = fx * g_state.move_x + fy * g_state.move_y;
  if (into < 0 && into < g_state.wall_into) {
    g_state.wall_x = fx;
    g_state.wall_y = fy;
    g_state.wall_into = into;
    g_state.wall = true;
  }
}

void LongMove(uint8_t* base) {
  const float dt = FrameTime(base);
  const bool wall = g_state.wall;
  const float wall_x = g_state.wall_x, wall_y = g_state.wall_y;
  g_state.wall = false;
  g_state.wall_into = 0;
  g_state.since += dt;
  const uint32_t obj = Load32(base, kPlayer);
  if (!Valid(obj)) return;
  const uint32_t matrix = Load32(base, obj + 12);
  const uint32_t ref = Load32(base, obj + 24);
  if (!Valid(matrix) || !Valid(ref)) return;
  const uint32_t rec = Load32(base, ref);
  const uint32_t set = Valid(rec) ? Load32(base, rec + 4) : 0;
  const uint32_t old_matrix = Valid(set) ? Load32(base, set + 0x38) : 0;
  if (!Valid(old_matrix)) return;
  const float x = LoadF(base, matrix + 48), y = LoadF(base, matrix + 52);
  const float move_x = x - LoadF(base, old_matrix + 48), move_y = y - LoadF(base, old_matrix + 52);
  g_state.move_x = move_x;
  g_state.move_y = move_y;
  if (g_state.since < kStep - kSlack) return;
  g_state.since = 0;
  const float into = move_x * wall_x + move_y * wall_y;
  if (!wall || into >= 0 || dt <= 0 || dt >= kStep || -into > kMaxSpeed * dt) return;
  const float grow = into * (kStep / dt - 1.0f);
  StoreF(base, matrix + 48, x + wall_x * grow);
  StoreF(base, matrix + 52, y + wall_y * grow);
  Count("step_up");
}

}  // namespace
}  // namespace kk::hfr

REX_EXTERN(__imp__sub_828092D8);
REX_HOOK_RAW(sub_828092D8) {  // contact_resolve
  const uint32_t a = ctx.r5.u32, normal = ctx.r6.u32, push = ctx.r7.u32, flags = ctx.r8.u32;
  __imp__sub_828092D8(ctx, base);
  if (kk::hfr::On("step_up")) kk::hfr::NoteContact(base, normal, flags, ctx.r3.u32);
#if defined(KK_DEV_TOOLS)
  kk::DevContactRow(base, a, normal, push, flags, ctx.r3.u32);
#else
  (void)a;
  (void)push;
#endif
}

REX_EXTERN(__imp__sub_8284C6F8);
REX_HOOK_RAW(sub_8284C6F8) {  // world_collide
  if (kk::hfr::On("step_up")) kk::hfr::LongMove(base);
  __imp__sub_8284C6F8(ctx, base);
}
