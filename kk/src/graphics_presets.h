#pragma once

#include <iterator>
#include <string_view>

namespace kk {
// A graphics preset sets every quality setting of Settings > Graphics (and the F8 panel's resolutions of AO and
// GI) together; anything else shows Custom. Owner, 21 Oct (Beta 14): presets set shadows, water reflections and
// light shafts too, and every graphics setting follows the preset. Left to the player: the look's own effects
// (motion blur, screen blur, distance fog), the frame rate, the field of view, the texture pack, cel shading,
// tessellation and the F8 panel's tuning sliders (strengths, softness, ripple, mip bias, DLSS model).
struct GraphicsPreset {
  const char* label;
  const char* temporal;    // native_upscaler_quality, with the default upscaler
  const char* quality;     // kk_render_quality (with FSR 1)
  const char* aa;          // swap_post_effect
  const char* filtering;   // anisotropic_override: 2 = 2x, 3 = 4x, 4 = 8x, 5 = 16x
  bool ao;                 // ao_mode 1 at strength 3 (with kAoSettings)
  const char* gi;          // native_gi
  const char* gi_distance; // native_gi_distance
  const char* shadows;     // native_shadow_quality
  const char* water;       // native_water_reflections: original or ssr (Screen space)
  const char* shafts;      // native_volumetrics: off (Original), low or high
  const char* ao_samples;  // native_ao_samples (AO quality: 1 Low, 2 Medium, 3 High), set with AO on
};
// Shadows / water reflections / light shafts: the owner's table (21 Oct). Its "Medium" light shafts at High is
// Volumetric low, the middle of the three choices (Original, low, high).
inline constexpr GraphicsPreset kGraphicsPresets[] = {
    {"Low", "performance", "performance", "fxaa", "2", false, "low", "near", "original", "original", "off", "1"},
    {"Medium", "quality", "quality", "fxaa", "3", false, "medium", "medium", "original", "original", "off", "2"},
    {"High", "quality", "native", "fxaa", "5", true, "high", "far", "high", "ssr", "low", "2"},
    {"Ultra", "quality", "native", "fxaa", "5", true, "high", "far", "ultra", "ssr", "high", "3"},  // GI distance Full: F8 only (owner, Beta 8)
    {"Steam Deck", "quality", "native", "fxaa", "2", false, "low", "near", "original", "original", "off", "1"},
};
inline constexpr int kGraphicsPresetCount = int(std::size(kGraphicsPresets));

// The same value in every preset: the defaults (the game's own picture where they have one).
struct PresetSetting {
  const char* name;
  const char* value;
};
inline constexpr PresetSetting kPresetCommonSettings[] = {
    {"native_sharpness", "off"},          // Sharpness
    {"native_effect_buffers", "true"},    // Effect resolution: Output
    {"native_gi_quality", "auto"},        // F8 GI resolution
};
// With ambient occlusion on (High, Ultra): Strong, High normal smoothing (owner 17 Oct), the F8 AO resolution Auto.
inline constexpr PresetSetting kAoSettings[] = {
    {"ao_strength", "3"},
    {"native_ao_normal_smoothing", "2"},
    {"native_ao_quality", "auto"},
};

// Shared by the launcher, pause settings and the migration of pre-GI presets.
// The upscaler match is checked separately because its default depends on the GPU.
// Legacy migration passes false: newer effect choices must not change its preset inference.
template <typename Get>
bool MatchesGraphicsPresetSettings(const GraphicsPreset& p, Get get, bool include_effects = true) {
  const bool ao = get("ao_mode") != "0";
  if (get("kk_render_quality") != p.quality || get("swap_post_effect") != p.aa ||
      get("anisotropic_override") != p.filtering || ao != p.ao || (ao && get("ao_strength") != "3"))
    return false;
  if (!include_effects) return true;
  if (get("native_gi") != p.gi || get("native_gi_distance") != p.gi_distance ||
      get("native_shadow_quality") != p.shadows || get("native_water_reflections") != p.water ||
      get("native_volumetrics") != p.shafts)
    return false;
  for (const auto& s : kPresetCommonSettings)
    if (get(s.name) != s.value) return false;
  if (ao) {
    if (get("native_ao_samples") != p.ao_samples) return false;
    for (const auto& s : kAoSettings)
      if (get(s.name) != s.value) return false;
  }
  return true;
}

// Keep an explicit selection, and recognise an unmarked configuration by its values (the last preset that
// matches: an old unmarked 16x one is Ultra's by its older values). A preference never hides custom settings.
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
    for (const auto& s : kAoSettings) set(s.name, s.value);
    set("native_ao_samples", p.ao_samples);
  }
  set("native_gi", p.gi);
  set("kk_graphics_preset", p.label);
  set("native_gi_distance", p.gi_distance);
  set("native_shadow_quality", p.shadows);
  set("native_water_reflections", p.water);
  set("native_volumetrics", p.shafts);
  for (const auto& s : kPresetCommonSettings) set(s.name, s.value);
}
}  // namespace kk
