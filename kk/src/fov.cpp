// Field of view (launcher: Display > Camera > Field of view, kk_fov).
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
// The menus keep their own angle (see g_views_since_menu); the port's pause menu does not (task 243).
//
// The Ultrawide mod (task 311): the view's angle is also made UltrawideFactor() times wider (the half-angle
// tangent) so the frustum that skips objects covers the wider picture (no pop-in at the new edges); the
// projection's height is put back in ultrawide.cpp (ViewWidened), so the picture stays hor+ (the same height).

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/system/flags.h>

#include "settings.h"
#include "freecam.h"
#include "coop_nametag.h"
#include "coop_observer.h"
#include "photo_mode.h"
#include "play_session.h"

namespace kk::ultrawide {
void SetViewWidened(float factor);  // ultrawide.cpp
}

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
// The port's pause menu (play::PauseMenuOpen, task 243) runs the same script but shows the game behind it (the
// last frame of play, kept as the pause starts and again when the field of view changes there), so while it is
// open the scene keeps the player's angle (and the Ultrawide mod its hor+): the paused picture matches play.
// Photo mode and the game's own pages from the pause (Score, Save, Options) are not the port's menu.
std::atomic<int> g_views_since_menu{0};

// Photo mode (task 311) is not the menus either: the pause's menu script runs meanwhile, but photo mode shows
// the game (with the Ultrawide mod: hor+, the HUD pass in its box).
bool MenusShow() {
  return g_views_since_menu.load(std::memory_order_relaxed) < 8 && !kk::play::PauseMenuOpen() &&
         !kk::photo::Active();
}

// The Ultrawide mod's view widening (task 311; 1 = unchanged, always with the mod off).
float WidenAngle(float radians, float factor) {
  return factor == 1.0f ? radians : 2.0f * std::atan(factor * std::tan(radians * 0.5f));
}
struct ViewWidenedScope {
  explicit ViewWidenedScope(float factor) : on(factor != 1.0f) { if (on) kk::ultrawide::SetViewWidened(factor); }
  ~ViewWidenedScope() { if (on) kk::ultrawide::SetViewWidened(1.0f); }
  bool on;
};

// How much wider than the game's own angle (1 = unchanged).
float LensFactor() {
  const int deg = REXCVAR_GET(kk_fov);
  const float want = float(deg) * 3.14159265f / 180.0f;
  if (want <= kJackFov + 0.005f) return 1.0f;
  return std::tan(std::min(want, 2.8f) * 0.5f) / std::tan(kJackFov * 0.5f);
}

}  // namespace

namespace kk::fov {
// The menus show (their 3D pieces keep the game's own projection; the Ultrawide mod, ultrawide.cpp).
bool InMenus() { return MenusShow(); }
}  // namespace kk::fov

REX_EXTERN(__imp__sub_82872DB8);
REX_HOOK_RAW(sub_82872DB8) {
  const uint32_t view = ctx.r3.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  auto draw = [&] {
    const bool capture = kk::coop::nametag::BeginView(base, view, caller);
    __imp__sub_82872DB8(ctx, base);
    if (capture) kk::coop::nametag::EndView(base);
  };
  std::array<uint8_t, 68> saved;
  float saved_fov=0;
  const bool observer=kk::coop::observer::View(base,view,caller,saved,saved_fov,LensFactor());
  const bool freecam = !observer && kk::freecam::OverrideView(base, view, uint32_t(ctx.lr), saved);
  struct RestoreView {
    uint8_t* base; uint32_t view; const std::array<uint8_t,68>& saved; bool restore,observer; float fov;
    ~RestoreView() {
      if (restore) std::memcpy(base+view,saved.data(),saved.size());
      if (observer) StoreF(base,view+172,fov);
    }
  } restore{base,view,saved,freecam || observer,observer,saved_fov};
  const float k = observer ? 1.f : LensFactor();
  const uint32_t at = ctx.r3.u32 + 172;
  const float fov = LoadF(base, at);
  const int since_menu = g_views_since_menu.load(std::memory_order_relaxed);
  if (since_menu < 1000) g_views_since_menu.store(since_menu + 1, std::memory_order_relaxed);
  const bool photo = kk::photo::Active();
  const bool menus = since_menu < 8 && !kk::play::PauseMenuOpen() && !photo;
  // The Ultrawide mod (1 with it off, and in the menus): the angle also widened for the frustum (see the top).
  static const bool old_cull = [] {  // KK_ULTRAWIDE_OLD_CULL=1 (diagnostics): the 16:9 frustum of task 223
    const char* e = std::getenv("KK_ULTRAWIDE_OLD_CULL");
    return e && *e == '1';
  }();
  const float uw = menus || old_cull ? 1.0f : kk::UltrawideFactor();
  // Photo mode: its own field of view for the scene view it flies (the pause's menu script runs meanwhile).
  if (freecam && photo && fov > 0.01f && fov < 3.0f) {
    const float wide = k == 1.0f ? fov : 2.0f * std::atan(k * std::tan(fov * 0.5f));
    const float scene = kk::photo::SceneFieldOfView(wide);
    const float shown = scene < 2.9f ? WidenAngle(scene, uw) : scene;
    ViewWidenedScope widened(shown == scene ? 1.0f : uw);
    StoreF(base, at, shown);
    draw();
    StoreF(base, at, fov);
    return;
  }
  if ((k == 1.0f && uw == 1.0f) || menus || !(fov > 0.01f && fov < 3.0f)) {
    draw();
    return;
  }
  ViewWidenedScope widened(uw);
  StoreF(base, at, WidenAngle(2.0f * std::atan(k * std::tan(fov * 0.5f)), uw));
  draw();
  StoreF(base, at, fov);
}

// IntMIG_loop: the front-end menu script, run every frame while the menus show.
REX_EXTERN(__imp__sub_824CACB0);
REX_HOOK_RAW(sub_824CACB0) {
  g_views_since_menu.store(0, std::memory_order_relaxed);
  __imp__sub_824CACB0(ctx, base);
}
