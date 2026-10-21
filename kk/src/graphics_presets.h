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
  const char* shadows = nullptr; // native_shadow_quality; null preserves the player's choice
};
inline constexpr GraphicsPreset kGraphicsPresets[] = {
    {"Low", "performance", "performance", "fxaa", "2", false, "low", "near"},
    {"Medium", "quality", "quality", "fxaa", "3", false, "medium", "medium", "original"},
    {"High", "quality", "native", "fxaa", "5", true, "high", "far"},
    {"Ultra", "quality", "native", "fxaa", "5", true, "high", "far"},  // Full GI distance is F8 only (owner, Beta 8)
    {"Steam Deck", "quality", "native", "fxaa", "2", false, "low", "near"},
};
inline constexpr int kGraphicsPresetCount = int(std::size(kGraphicsPresets));

// Shared by the launcher, pause settings and the migration of pre-GI presets.
// The upscaler match is checked separately because its default depends on the GPU.
// Legacy migration passes false: newer effect choices must not change its preset inference.
template <typename Get>
bool MatchesGraphicsPresetSettings(const GraphicsPreset& p, Get get, bool include_effects = true) {
  const bool ao = get("ao_mode") != "0";
  return get("kk_render_quality") == p.quality && get("swap_post_effect") == p.aa &&
         get("anisotropic_override") == p.filtering && ao == p.ao &&
         (!ao || get("ao_strength") == "3") &&
         (!include_effects || (get("native_gi") == p.gi && get("native_gi_distance") == p.gi_distance &&
                              (!p.shadows || get("native_shadow_quality") == p.shadows)));
}

// High and Ultra currently share their values. Keep an explicit selection, and recognise
// an unmarked old 16x configuration as Ultra. A preference never hides custom settings.
template <typename Get, typename ExtraMatch>
int FindGraphicsPreset(Get get, ExtraMatch extra_match) {
  const auto preferred = get("kk_graphics_preset");
  for (int i = 0; i < kGraphicsPresetCount; ++i) {
    const auto& p = kGraphicsPresets[i];
    if (std::string_view(preferred) == p.label && extra_match(p) && MatchesGraphicsPresetSettings(p, get)) return i;
  }
  for (int i = kGraphicsPresetCount - 1; i >= 0; --i)
    if (extra_match(kGraphicsPresets[i]) && MatchesGraphicsPresetSettings(kGraphicsPresets[i], get)) return i;
  return -1;
}

template <typename Set>
void ApplyGraphicsPresetSettings(const GraphicsPreset& p, Set set) {
  set("kk_render_quality", p.quality);
  set("swap_post_effect", p.aa);
  set("anisotropic_override", p.filtering);
  set("ao_mode", p.ao ? "1" : "0");
  if (p.ao) {
    set("ao_strength", "3");
    set("native_ao_normal_smoothing", "2");  // High (owner 17 Oct)
  }
  set("native_gi", p.gi);
  set("kk_graphics_preset", p.label);
  set("native_gi_distance", p.gi_distance);
  if (p.shadows) set("native_shadow_quality", p.shadows);
}
}  // namespace kk
