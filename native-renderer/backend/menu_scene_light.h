// The live main menu scene behind the launcher (task 330, owner 20 Oct: "make it lighter to save power"): while
// the game's start menu scene runs in the launcher's process, the effect settings read each frame are lowered for
// that process only (the player's settings, the cvars and the game's own process are never touched). The game
// sets it through the plugin's nr_set_menu_scene_light export (plugin/photo_api.cpp).
//
// Lowered: global illumination off, ambient occlusion at half resolution, volumetric light off, shadows and water
// reflections the original ones (shadow maps at the game's own size), tessellation off, and a temporal upscaler's
// mode at most Performance.
#pragma once

#include <atomic>

#include "backend/effect_settings.h"

namespace nr {

inline std::atomic<bool> g_menu_scene_light{false};

inline void SetMenuSceneLight(bool on) { g_menu_scene_light.store(on, std::memory_order_relaxed); }
inline bool MenuSceneLight() { return g_menu_scene_light.load(std::memory_order_relaxed); }

inline void LightenForMenuScene(EffectSettings& s) {
  s.gi.level = GiLevel::kOff;
  s.ao_quality = AoQuality::kHalf;
  s.lighting.volumetrics = Volumetrics::kOff;
  s.lighting.shadows = ShadowQuality::kOriginal;
  s.lighting.water = WaterReflections::kOriginal;
  s.tessellation = false;
  s.shadow_scale = 1.0f;  // shadow maps at the game's own size (play scales them with the render scale)
  if (s.upscale.quality == UpscaleQuality::kNative || s.upscale.quality == UpscaleQuality::kQuality ||
      s.upscale.quality == UpscaleQuality::kBalanced)
    s.upscale.quality = UpscaleQuality::kPerformance;
}

}  // namespace nr
