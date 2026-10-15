// The settings the game renderer applies while the game runs (the F8 graphics panel, the launcher's
// in-game pages): read once a frame, at the frame's first clear, draw or resolve (GameRenderer::
// UpdateEffects, game_effects.cpp). Everything else in GameSettings stays as it was at the start.
#pragma once

#include <cstdint>
#include <string>

#include "backend/ao_config.h"
#include "backend/gi_config.h"
#include "backend/lighting_config.h"
#include "backend/upscale_config.h"

namespace nr {

struct EffectSettings {
  bool texture_pack = false;
  std::string texture_pack_folder;
  bool tessellation = false;
  float tessellation_smoothness = 0.75f;
  uint32_t tessellation_detail = 4;
  AoQuality ao_quality = AoQuality::kAuto;
  GiConfig gi;
  LightingConfig lighting;
  float shadow_scale = 0;
  // The temporal upscaler as asked for (kind, mode, DLSS preset, sharpness, mip bias, reactive mask).
  // A change of kind, mode or preset makes the upscaler again at a frame boundary; the others apply
  // to the next frame.
  UpscaleConfig upscale;
  uint32_t fxaa = 0;                  // swap_post_effect: 0 off, 1 fxaa, 2 fxaa_extreme, 3 SMAA 1x
  bool effect_buffers = true;
  uint32_t sharpness = 0;
  int32_t anisotropic_override = -1;  // anisotropic_override: -1 the game's, 0 off, 2 to 5 (2x to 16x)
};

}  // namespace nr
