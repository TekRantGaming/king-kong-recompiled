// Experimental ultrawide (task 223, the built-in Ultrawide mod; DECISIONS 18 Oct): hor+ for the game's 3D views.
//
// With kk_aspect wider than 16:9 the renderer draws a wider picture (settings.cpp ApplyRenderPreset) while the
// game keeps drawing for 1280x720, so each 3D view is made UltrawideFactor() times wider here: the same height,
// more on the sides, nothing stretched. The projection callback (GDI +32, sub_8278AB00; used by the scene views
// from sub_82872DB8, the first-person weapon and photo mode) builds a perspective matrix at 0x83090510 and pushes
// it on the projection stack (sub_820E0D08, which copies the 16 floats). That push is widened (column 0 divided,
// ultrawide_logic.h WidenProjection) and the scratch matrix put back afterwards, so the game never sees a change.
// Orthographic projections (HUD, menus, full-screen passes) and the menus' 3D pieces stay as they are.
//
// Edge culling (task 311): the game skips objects outside a frustum it builds from the view's angle, which
// stayed 16:9, so things at the new edges were left out. fov.cpp now widens the scene views' angle by the same
// factor (the half-angle tangent) before the view is set up, so the frustum covers the wide picture. The
// projection the callback then builds is already wider; if it also got shorter (its height comes from the same
// angle: the ratio y / x is still 16:9), the height column is put back here, so the picture stays hor+. Views
// not widened that way (the first-person weapon, which has its own angle) get column 0 divided as before.
//
// HUD (task 231, moved in task 239): the draws of the executor's HUD pass (pass 4 of sub_82796C10: HUD text and
// icons, the ammo counter, subtitles, loading text) are tagged for the native renderer (nr_set_draw_tags,
// native-renderer/backend/draw_tags.h), which puts them in a centred 16:9 box; a draw covering the whole width
// (a fade, a tint) or with a perspective matrix in c0-c3 stays as it is. At 16:9 (the default) nothing here does
// anything and no tag is ever sent.
//
// Hide HUD (task 243; DECISIONS 19 Oct): with kk_hide_hud or F10 (freecam::HideGameHud) the same pass is tagged
// "hide" (mod on or off), and the renderer leaves its draws out (the ammo counter, subtitles) except full-width
// ones (fades) and 3D ones. Not while a loading screen is up (its text stays) nor in photo mode (unchanged).

#include <array>
#include <cmath>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "freecam.h"
#include "photo_mode.h"
#include "settings.h"
#include "ultrawide_logic.h"
#include "coop_nametag.h"

namespace kk::fov {
bool InMenus();
}

namespace {

constexpr uint32_t kProjectionScratch = 0x83090510;  // sub_8278AB00's perspective matrix
thread_local int g_in_projection = 0;
thread_local float g_view_widened = 1.0f;  // fov.cpp widened this view's angle by this factor
std::atomic<bool> g_logged{false};
std::atomic<bool> g_logged_view{false};

constexpr uint32_t kTagHud = 1u;        // draw_tags.h kDrawTagHud
constexpr uint32_t kTagHideHud = 2u;    // draw_tags.h kDrawTagHideHud
constexpr uint32_t kTagLeafShift = 8;   // draw_tags.h kDrawTagLeafShift (diagnostics)
thread_local uint32_t g_tags = 0;

using SetTagsFn = void (*)(uint32_t, float);
SetTagsFn FindSetTags() {
#if defined(_WIN32)
  for (const char* module : {"rexgpu-nativerd.dll", "rexgpu-native.dll", "rexgpu-natived.dll"})
    if (HMODULE m = GetModuleHandleA(module))
      if (FARPROC f = GetProcAddress(m, "nr_set_draw_tags")) return reinterpret_cast<SetTagsFn>(f);
#else
  for (const char* module : {"librexgpu-nativerd.so", "librexgpu-native.so", "librexgpu-natived.so"}) {
    if (void* m = dlopen(module, RTLD_NOW | RTLD_NOLOAD)) {
      void* f = dlsym(m, "nr_set_draw_tags");
      dlclose(m);  // still loaded by the runtime
      if (f) return reinterpret_cast<SetTagsFn>(f);
    }
  }
#endif
  return nullptr;
}

void SetTags(uint32_t tags) {
  if (tags == g_tags) return;
  g_tags = tags;
  static const SetTagsFn set = FindSetTags();
  if (set) set(tags, kk::UltrawideFactor());
}

// KK_ULTRAWIDE_PASS_DIAG=1 (diagnostics): every pass is tagged, so KK_ULTRAWIDE_HUD_LOG shows which pass draws
// what (its "leaf" is the pass number + 1). Shadows and other passes then go wrong: never for play.
bool PassDiag() {
  static const bool on = [] {
    const char* e = std::getenv("KK_ULTRAWIDE_PASS_DIAG");
    return e && *e == '1';
  }();
  return on;
}

constexpr uint32_t kHudPass = 4;  // the last 2D pass: HUD text and icons, ammo counter, subtitles, loading text

// The game's loading screen (loading_fix.cpp): +0 its thread, +12 its state (2 starting, 1 shown, 3 stopping).
constexpr uint32_t kLoadingScreen = 0x82D64080;
uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return std::byteswap(v);
}
bool LoadingScreenUp(const uint8_t* base) {
  const uint32_t state = Load32(base, kLoadingScreen + 12);
  return Load32(base, kLoadingScreen) != 0 && (state == 1 || state == 2);
}

// Hide HUD / F10 leaves out the HUD pass's draws (see the top).
bool HideHudPass(const uint8_t* base) {
  return kk::freecam::HideGameHud() && !kk::photo::Active() && !LoadingScreenUp(base);
}

}  // namespace

namespace kk::ultrawide {
void SetViewWidened(float factor) { g_view_widened = factor; }
}  // namespace kk::ultrawide

// The render executor's pass runner (r4 = pass number; called from the frame's pass sequence sub_82797460,
// pass 4 at 0x827977A8). The game records its draws into passes while it updates and draws them here, on
// the drawing thread, so the HUD tag is set around the pass itself (task 239; the display-list hooks of task 231
// run on the recording thread and only tagged whatever the drawing thread drew at that moment).
REX_EXTERN(__imp__sub_82796C10);
REX_HOOK_RAW(sub_82796C10) {
  const uint32_t pass = ctx.r4.u32;
  const bool box = kk::UltrawideFactor() != 1.0f && !kk::fov::InMenus() && (pass == kHudPass || PassDiag());
  const bool hide = pass == kHudPass && HideHudPass(base);
  if (!box && !hide) {
    __imp__sub_82796C10(ctx, base);
    return;
  }
  const uint32_t old = g_tags;
  SetTags((box ? kTagHud : 0u) | (hide ? kTagHideHud : 0u) | (((pass + 1) & 0xFF) << kTagLeafShift));
  __imp__sub_82796C10(ctx, base);
  SetTags(old);
}

REX_EXTERN(__imp__sub_8278AB00);
REX_HOOK_RAW(sub_8278AB00) {
  ++g_in_projection;
  __imp__sub_8278AB00(ctx, base);
  --g_in_projection;
}

REX_EXTERN(__imp__sub_820E0D08);
REX_HOOK_RAW(sub_820E0D08) {
  // Capture precisely the matrix copied to the projection stack, including all
  // FOV/hor+ adjustments. Every early return below must go through this call.
  const uint32_t matrix = ctx.r4.u32;
  auto push = [&] {
    if (g_in_projection && matrix == kProjectionScratch)
      kk::coop::nametag::ProjectionPush(base, matrix);
    __imp__sub_820E0D08(ctx, base);
  };
  const float factor = kk::UltrawideFactor();
  if (factor == 1.0f || g_in_projection == 0 || ctx.r4.u32 != kProjectionScratch || kk::fov::InMenus()) {
    push();
    return;
  }
  std::array<uint8_t, 64> saved;
  std::memcpy(saved.data(), base + kProjectionScratch, saved.size());
  std::array<float, 16> m;
  for (size_t i = 0; i < m.size(); ++i) {
    uint32_t v;
    std::memcpy(&v, saved.data() + i * 4, 4);
    m[i] = std::bit_cast<float>(std::byteswap(v));
  }
  if (g_view_widened != 1.0f) {
    // The angle was widened (fov.cpp): x is already right. Put the height back when it shrank with it.
    if (m[11] == 0.0f || m[15] != 0.0f || m[0] == 0.0f) {
      push();
      return;
    }
    const float ratio = std::fabs(m[5] / m[0]);
    const float game = float(kk::ultrawide::kGameAspect);
    const bool shorter = std::fabs(ratio - game) < 0.03f * game;
    if (!g_logged_view.exchange(true))
      REXLOG_INFO("KK: ultrawide: view angle widened {:.3f} times for culling; y/x {:.3f}, height {}", g_view_widened,
                  ratio, shorter ? "put back" : "kept");
    if (!shorter) {
      push();
      return;
    }
    for (int i : {1, 5, 9, 13}) m[i] *= g_view_widened;
  } else if (!kk::ultrawide::WidenProjection(m.data(), factor)) {
    push();
    return;
  }
  for (size_t i = 0; i < m.size(); ++i) {
    const uint32_t v = std::byteswap(std::bit_cast<uint32_t>(m[i]));
    std::memcpy(base + kProjectionScratch + i * 4, &v, 4);
  }
  if (!g_logged.exchange(true))
    REXLOG_INFO("KK: ultrawide: first 3D view widened {:.3f} times (hor+)", factor);
  push();
  std::memcpy(base + kProjectionScratch, saved.data(), saved.size());
}
