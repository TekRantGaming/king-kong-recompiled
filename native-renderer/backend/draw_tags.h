// Draw tags (task 231, the experimental ultrawide mod): the game marks the draws it is issuing, the draw-state
// tracker copies the current tags into each DrawCall on the game's thread (so they travel with the call to the
// render thread), and the renderer acts on them.
//
// kDrawTagHud: a draw of the game's HUD pass (executor pass 4: HUD, ammo counter, subtitles; task 239). With
// a picture wider than 16:9 the game still lays them out for 1280x720, so the renderer puts them in a centred
// 16:9 box (the clip-space X divided by HudWidthScale()) instead of stretching them over the whole width. A draw
// that covers the whole guest width (a fade, a full-screen tint) keeps covering the whole picture.
// Nothing is tagged unless the game calls nr_set_draw_tags (only with the mod on), so the default is unchanged.
#pragma once

#include <atomic>
#include <cstdint>

namespace nr {

inline constexpr uint32_t kDrawTagHud = 1u;
// Bits 8-15: the executor pass number + 1 the draw comes from (diagnostics only).
inline constexpr uint32_t kDrawTagLeafShift = 8;

inline std::atomic<uint32_t> g_draw_tags{0};
inline std::atomic<float> g_hud_width_scale{1.0f};

inline void SetDrawTags(uint32_t tags, float hud_width_scale) {
  g_hud_width_scale.store(hud_width_scale > 1.0f ? hud_width_scale : 1.0f, std::memory_order_relaxed);
  g_draw_tags.store(tags, std::memory_order_relaxed);
}
inline uint32_t CurrentDrawTags() { return g_draw_tags.load(std::memory_order_relaxed); }
inline float HudWidthScale() { return g_hud_width_scale.load(std::memory_order_relaxed); }

}  // namespace nr
