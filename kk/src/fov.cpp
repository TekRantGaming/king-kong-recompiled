// Field of view (launcher: Gameplay > Field of view, kk_fov).
//
// Each frame the renderer copies the active view's horizontal field of view
// (radians, at view +172) to the camera in GDI = *(0x82CCAC30), at GDI +220.
// sub_8278AB00 builds the projection from it; sub_827E5BB8 builds the matching
// frustum. The guest render target is sized to the selected output aspect so
// the presenter scales it uniformly, keeping both 3D geometry and screen-space
// UI in proportion.
//
// Jack's camera is 1.2 radians (about 69 degrees) in play; cutscene and
// scripted cameras use their own angles (0.5 to 1.4). kk_fov is the angle
// wanted for Jack's camera, and every camera is widened by the same lens
// factor (the ratio of the half-angle tangents), so close-ups stay closer than
// wide shots. The view's own value is put back after the call, so the game
// never sees (or compounds) the wider angle. The first-person weapon is drawn
// with its own fixed angle (sub_827C6660 sets 1.0 directly) and is unchanged.
// The menu camera is widened with the selected output aspect just like the
// gameplay cameras, so ultrawide menus fill the display without stretching.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

#include "frame_stats.h"
#include "presentation.h"
#include "settings.h"

namespace {

constexpr float kJackFov = 1.2f;  // radians: the game's own angle for Jack's camera
constexpr float kGameAspect = 16.0f / 9.0f;
std::atomic<float> g_output_aspect{kGameAspect};
std::atomic<int> g_output_width{0};
std::atomic<int> g_output_height{0};
std::atomic<bool> g_logged_gameplay_fov{false};

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
std::atomic<int64_t> g_last_menu_script_ns{0};

// How much wider than the game's own angle (1 = unchanged).
float LensFactor() {
  const int deg = REXCVAR_GET(kk_fov);
  const float want = float(deg) * 3.14159265f / 180.0f;
  const float aspect = REXCVAR_QUERY(bool, present_letterbox)
                           ? kGameAspect
                           : g_output_aspect.load(std::memory_order_relaxed);
  const float target_tan = std::tan(std::min(want, 2.8f) * 0.5f) * (aspect / kGameAspect);
  return target_tan / std::tan(kJackFov * 0.5f);
}

}  // namespace

void kk::SetOutputAspectRatio(int width, int height) {
  // In fill mode, match the guest render target to the display and widen the
  // horizontal camera FOV to preserve the original vertical framing.
  g_output_width.store(width, std::memory_order_relaxed);
  g_output_height.store(height, std::memory_order_relaxed);
  if (REXCVAR_QUERY(bool, present_letterbox) || width <= 0 || height <= 0) {
    g_output_aspect.store(kGameAspect, std::memory_order_relaxed);
    REXLOG_INFO("KK: aspect check: output={}x{} ({:.4f}:1), framing=16:9", width, height,
                float(width) / float(height));
    return;
  }
  const float aspect = float(width) / float(height);
  g_output_aspect.store(aspect, std::memory_order_relaxed);
  REXLOG_INFO("KK: aspect check: output={}x{} ({:.4f}:1), framing=display", width, height, aspect);
}

int kk::GameVideoModeWidth() {
  return GameFrameWidth(g_output_width.load(std::memory_order_relaxed),
                        g_output_height.load(std::memory_order_relaxed),
                        REXCVAR_QUERY(bool, present_letterbox));
}

bool kk::IsMenuActive() {
  const int64_t last = g_last_menu_script_ns.load(std::memory_order_relaxed);
  if (last) {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    // The menu script may render many views between swaps, so a short time
    // window is more reliable than counting those views.
    return now - last < std::chrono::milliseconds(500).count() * 1'000'000LL;
  }
  // Preserve the existing short fallback for menu transitions without a recent script tick.
  return g_views_since_menu.load(std::memory_order_relaxed) < 8;
}

REX_EXTERN(__imp__sub_82872DB8);
REX_HOOK_RAW(sub_82872DB8) {
  const float k = LensFactor();
  const uint32_t at = ctx.r3.u32 + 172;
  const float fov = LoadF(base, at);
  const int since_menu = g_views_since_menu.load(std::memory_order_relaxed);
  if (since_menu < 1000) g_views_since_menu.store(since_menu + 1, std::memory_order_relaxed);
  const bool gameplay_view = kk::IsGameplayCameraView(kk::IsMenuActive(), kk::IsMoviePlayerActive(base));
  if (gameplay_view) {
    kk::NotifyGameplayCamera();
    bool not_logged = false;
    if (!REXCVAR_QUERY(bool, present_letterbox) && std::abs(fov - kJackFov) < 0.01f &&
        g_logged_gameplay_fov.compare_exchange_strong(not_logged, true, std::memory_order_relaxed)) {
      const float corrected_fov = 2.0f * std::atan(k * std::tan(fov * 0.5f));
      REXLOG_INFO(
          "KK: aspect check: gameplay horizontal FOV {:.2f} -> {:.2f} degrees at output={}x{} ({:.4f}:1), "
          "letterbox={}",
          fov * 180.0f / 3.14159265f, corrected_fov * 180.0f / 3.14159265f,
          g_output_width.load(std::memory_order_relaxed), g_output_height.load(std::memory_order_relaxed),
          g_output_aspect.load(std::memory_order_relaxed), REXCVAR_QUERY(bool, present_letterbox));
    }
  }
  if (k == 1.0f || !(fov > 0.01f && fov < 3.0f)) {
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
  g_last_menu_script_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count(),
      std::memory_order_relaxed);
  __imp__sub_824CACB0(ctx, base);
}
