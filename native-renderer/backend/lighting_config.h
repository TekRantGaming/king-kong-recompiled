// The modern lighting options of the native renderer (docs/lighting.md): softer, sharper shadows,
// screen-space reflections on water and volumetric light shafts. Every default is the game's own
// picture, unchanged: with the defaults none of the lighting code runs.
#pragma once

#include <cstdint>
#include <string>

namespace nr {

// --native_shadow_quality: original (the game's 832x832 shadow maps, one hard comparison per pixel),
// high (shadow maps at twice the resolution, a wide rotated PCF kernel), ultra (the same
// resolution, PCSS: the penumbra widens with the distance to the blocker).
enum class ShadowQuality : uint8_t { kOriginal, kHigh, kUltra };
// --native_water_reflections: original (the game's planar reflection and environment colour) or ssr
// (screen-space reflections from the scene at the fog pass, falling back to the game's).
enum class WaterReflections : uint8_t { kOriginal, kSsr };
// --native_volumetrics: off (the game's light shaft sprites only), low or high (screen-space
// shafts from bright sky regions, added in proportion to the game's fog).
enum class Volumetrics : uint8_t { kOff, kLow, kHigh };

struct LightingConfig {
  ShadowQuality shadows = ShadowQuality::kOriginal;
  WaterReflections water = WaterReflections::kOriginal;
  Volumetrics volumetrics = Volumetrics::kOff;
  // Tuning (1 keeps the tuned values): the PCF / PCSS kernel's size, the reflections' and the
  // scattering's strength.
  float shadow_softness = 1.0f;
  float ssr_strength = 1.0f;
  float volumetric_strength = 1.0f;
  // --native_lighting_census: every this many frames, the lighting-related draws of the frame (shadow
  // maps and the screen-space shadow, water, light shafts, fog) with their targets, textures and
  // constants in the log (0 off). A tool for finding the passes, independent of the options.
  uint32_t census_every = 0;
  // --native_lighting_gpu_time: GPU time of each feature's own passes, logged every 600 frames.
  bool gpu_time = false;

  bool shadows_on() const { return shadows != ShadowQuality::kOriginal; }
  bool water_on() const { return water != WaterReflections::kOriginal; }
  bool volumetrics_on() const { return volumetrics != Volumetrics::kOff; }
  bool any() const { return shadows_on() || water_on() || volumetrics_on(); }
  // What the renderer has to follow per frame (any feature, or the census).
  bool active() const { return any() || census_every != 0 || gpu_time; }
};

// The shadow map scale a quality asks for (0: the quality leaves it to --native_shadow_scale).
float ShadowScaleForQuality(ShadowQuality q);

bool ParseShadowQuality(const std::string& text, ShadowQuality& out);
bool ParseWaterReflections(const std::string& text, WaterReflections& out);
bool ParseVolumetrics(const std::string& text, Volumetrics& out);
const char* ShadowQualityName(ShadowQuality q);
const char* WaterReflectionsName(WaterReflections w);
const char* VolumetricsName(Volumetrics v);

}  // namespace nr
