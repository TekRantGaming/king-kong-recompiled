// Field of view (launcher: Gameplay > Field of view, kk_fov).
//
// Each frame the renderer copies the active view's camera into its own camera
// (GDI = *(0x82CCAC30), camera at GDI +208) in sub_82872DB8: the view's field
// of view (radians, horizontal) is at view +172 and goes to GDI +220. The same
// call then builds the projection (sub_8278AB00, which uses tan(fov / 2)) and
// the frustum used to skip objects outside the view (sub_827E5BB8), so a wider
// angle passed in here widens both and nothing pops in at the screen edges.
//
// Jack's camera is 1.2 radians (about 69 degrees) in play; cutscene and
// scripted cameras use their own angles (0.5 to 1.4). kk_fov is the angle
// wanted for Jack's camera, and every camera is widened by the same lens
// factor (the ratio of the half-angle tangents), so close-ups stay closer than
// wide shots. The view's own value is put back after the call, so the game
// never sees (or compounds) the wider angle. The first-person weapon is drawn
// with its own fixed angle (sub_827C6660 sets 1.0 directly) and is unchanged.
// The menus keep their own angle (see g_views_since_menu).

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/system/flags.h>

#include "settings.h"

namespace {

constexpr float kJackFov = 1.2f;  // radians: the game's own angle for Jack's camera

float LoadF(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return std::bit_cast<float>(std::byteswap(v));
}

void StoreF(uint8_t* base, uint32_t a, float f) {
  const uint32_t v = std::byteswap(std::bit_cast<uint32_t>(f));
  std::memcpy(base + a, &v, 4);
}

// Views drawn since the menu script (IntMIG_loop) last ran. It runs every
// frame while the menus show (the front end, and the pause menu, whose black
// screen hides the game): the front end's logo, chapter strip and other 3D
// pieces sit in front of the menu camera, so the menus keep their own angle.
// A frame draws up to three views, so a few views without it means play.
std::atomic<int> g_views_since_menu{0};

// How much wider than the game's own angle (1 = unchanged).
float LensFactor() {
  const int deg = REXCVAR_GET(kk_fov);
  const float want = float(deg) * 3.14159265f / 180.0f;
  if (want <= kJackFov + 0.005f) return 1.0f;
  return std::tan(std::min(want, 2.8f) * 0.5f) / std::tan(kJackFov * 0.5f);
}

}  // namespace

REX_EXTERN(__imp__sub_82872DB8);
REX_HOOK_RAW(sub_82872DB8) {
  const float k = LensFactor();
  const uint32_t at = ctx.r3.u32 + 172;
  const float fov = LoadF(base, at);
  const int since_menu = g_views_since_menu.load(std::memory_order_relaxed);
  if (since_menu < 1000) g_views_since_menu.store(since_menu + 1, std::memory_order_relaxed);
  const bool menus = since_menu < 8;
  if (k == 1.0f || menus || !(fov > 0.01f && fov < 3.0f)) {
    __imp__sub_82872DB8(ctx, base);
    return;
  }
  StoreF(base, at, 2.0f * std::atan(k * std::tan(fov * 0.5f)));
  __imp__sub_82872DB8(ctx, base);
  StoreF(base, at, fov);
}

// IntMIG_loop: the front-end menu script, run every frame while the menus show.
REX_EXTERN(__imp__sub_824CACB0);
REX_HOOK_RAW(sub_824CACB0) {
  g_views_since_menu.store(0, std::memory_order_relaxed);
  __imp__sub_824CACB0(ctx, base);
}
