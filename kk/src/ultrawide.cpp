// Experimental ultrawide (task 223, the built-in Ultrawide mod; DECISIONS 18 Oct): hor+ for the game's 3D views.
//
// With kk_aspect wider than 16:9 the renderer draws a wider picture (settings.cpp ApplyRenderPreset) while the
// game keeps drawing for 1280x720, so each 3D view is made UltrawideFactor() times wider here: the same height,
// more on the sides, nothing stretched. The projection callback (GDI +32, sub_8278AB00; used by the scene views
// from sub_82872DB8, the first-person weapon and photo mode) builds a perspective matrix at 0x83090510 and pushes
// it on the projection stack (sub_820E0D08, which copies the 16 floats). That push is widened (column 0 divided,
// ultrawide_logic.h WidenProjection) and the scratch matrix put back afterwards, so the game never sees a change.
// Orthographic projections (HUD, menus, full-screen passes) and the menus' 3D pieces stay as they are (phase 2:
// the HUD in a centred 16:9 box). At 16:9 (the default) nothing here does anything.

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>

#include "settings.h"
#include "ultrawide_logic.h"

namespace kk::fov {
bool InMenus();
}

namespace {

constexpr uint32_t kProjectionScratch = 0x83090510;  // sub_8278AB00's perspective matrix
thread_local int g_in_projection = 0;
std::atomic<bool> g_logged{false};

}  // namespace

REX_EXTERN(__imp__sub_8278AB00);
REX_HOOK_RAW(sub_8278AB00) {
  ++g_in_projection;
  __imp__sub_8278AB00(ctx, base);
  --g_in_projection;
}

REX_EXTERN(__imp__sub_820E0D08);
REX_HOOK_RAW(sub_820E0D08) {
  const float factor = kk::UltrawideFactor();
  if (factor == 1.0f || g_in_projection == 0 || ctx.r4.u32 != kProjectionScratch || kk::fov::InMenus()) {
    __imp__sub_820E0D08(ctx, base);
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
  if (!kk::ultrawide::WidenProjection(m.data(), factor)) {
    __imp__sub_820E0D08(ctx, base);
    return;
  }
  for (size_t i = 0; i < m.size(); ++i) {
    const uint32_t v = std::byteswap(std::bit_cast<uint32_t>(m[i]));
    std::memcpy(base + kProjectionScratch + i * 4, &v, 4);
  }
  if (!g_logged.exchange(true))
    REXLOG_INFO("KK: ultrawide: first 3D view widened {:.3f} times (hor+)", factor);
  __imp__sub_820E0D08(ctx, base);
  std::memcpy(base + kProjectionScratch, saved.data(), saved.size());
}
