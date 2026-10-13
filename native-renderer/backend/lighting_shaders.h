// The game's pixel shaders the lighting options care about (docs/lighting.md): XXH3-64 hashes of
// the microcode of every shader of these families in the game's database (kkshaders ao-list
// --family <name>), and what each one does. Facts about the game's shaders, like the AO
// classifier's rules; no game data.
#pragma once

#include <cstdint>

namespace nr {

enum class LightFamily : uint8_t {
  kNone,
  kShadow,           // psshadow: the shadow map (light-space z / w into a k_32_FLOAT target)
  kShadowQuad,       // psshadowquad: the screen-space shadow (the shadow maps against the view depth)
  kCompositeShadow,  // pscompositeshadow: min of two shadow masks
  kBlurShadow,       // psblurshadow: the screen-space shadow's blur
  kWater,            // pswater (26)
  kOcean,            // psocean
  kLightShaft,       // pslightshaft (12): render, blur, apply
  kReflection,       // psreflection: the planar water reflection
  kFog,              // the AfterEffects fog pass (two of psaftereffects)
  kAfterEffects,     // the other post effects
  kRain,
  kHeatShimmer,
  kSprite,
  kFur,
};
const char* LightFamilyName(LightFamily f);
LightFamily LightFamilyOf(uint64_t ps_hash);

// pslightshaft's modes (pslightshaft.hlsl's ProcessPixel): render (the cookie volumes into the
// shaft target), blur, and apply / resample (one fetch, no constants).
enum class LightShaftMode : uint8_t { kNone, kRender, kRenderShadowed, kBlur, kApply };
LightShaftMode LightShaftModeOf(uint64_t ps_hash);

// psshadowquad: how many lights (shadow maps at fetch constants 2, 3, 4) and where its constants
// are: g_mCamToLightMatrix at c0 (four rows per light), g_vShadowParams (z: the depth bias) and
// g_fShadowColor.
struct ShadowQuadLayout {
  uint32_t lights = 0;          // 0: not a psshadowquad
  uint32_t shadow_params = 0;   // g_vShadowParams[0]
  uint32_t shadow_color = 0;    // g_fShadowColor
};
ShadowQuadLayout ShadowQuadLayoutOf(uint64_t ps_hash);

// pswater: which of the reflection fetches a shader has (fetch 1: the dynamic planar reflection,
// fetch 2: the environment cube), from its translation.
struct WaterLayout {
  bool water = false;
  bool dynamic_reflection = false;  // fetch constant 1
  bool environment = false;         // fetch constant 2 (a cube)
};
WaterLayout WaterLayoutOf(uint64_t ps_hash);

}  // namespace nr
