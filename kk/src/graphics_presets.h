#pragma once

#include <iterator>
#include <string_view>

namespace kk {
struct GraphicsPreset {
  const char* label;
  const char* temporal;   // native_upscaler_quality, with the default upscaler
  const char* quality;    // kk_render_quality (with FSR 1)
  const char* aa;         // swap_post_effect
  const char* filtering;  // anisotropic_override: 2 = 2x, 3 = 4x, 4 = 8x, 5 = 16x
  bool ao;                // ao_mode 1 at strength 3
  const char* gi;         // native_gi
  const char* gi_distance; // native_gi_distance
};
inline constexpr GraphicsPreset kGraphicsPresets[] = {
    {"Low", "performance", "performance", "fxaa", "2", false, "low", "near"},
    {"Medium", "quality", "quality", "fxaa", "3", false, "medium", "medium"},
    {"High", "quality", "native", "fxaa", "4", true, "high", "far"},
    {"Ultra", "quality", "native", "fxaa", "5", true, "high", "full"},
    {"Steam Deck", "quality", "native", "fxaa", "2", false, "low", "near"},
};
inline constexpr int kGraphicsPresetCount = int(std::size(kGraphicsPresets));

// Shared by the launcher, pause settings and the migration of pre-GI presets.
// The upscaler match is checked separately because its default depends on the GPU.
template <typename Get>
bool MatchesGraphicsPresetSettings(const GraphicsPreset& p, Get get, bool include_gi = true) {
  const bool ao = get("ao_mode") != "0";
  return get("kk_render_quality") == p.quality && get("swap_post_effect") == p.aa &&
         get("anisotropic_override") == p.filtering && ao == p.ao &&
         (!ao || get("ao_strength") == "3") &&
         (!include_gi || (get("native_gi") == p.gi && get("native_gi_distance") == p.gi_distance));
}

template <typename Set>
void ApplyGraphicsPresetSettings(const GraphicsPreset& p, Set set) {
  set("kk_render_quality", p.quality);
  set("swap_post_effect", p.aa);
  set("anisotropic_override", p.filtering);
  set("ao_mode", p.ao ? "1" : "0");
  if (p.ao) set("ao_strength", "3");
  set("native_gi", p.gi);
  set("native_gi_distance", p.gi_distance);
}
}  // namespace kk
