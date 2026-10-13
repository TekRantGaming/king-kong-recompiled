// The screen-space global illumination's settings (docs/gi.md, "Settings"). The default is off:
// nothing of it is created and every frame is what it was.
#pragma once

#include <cstdint>
#include <string>

namespace nr {

// --native_gi: off, low, medium, high (the launcher's "Global illumination" row).
enum class GiLevel : uint8_t { kOff, kLow, kMedium, kHigh };
// --native_gi_quality: the working resolution. auto: the level's (low and medium half, high full up
// to a render scale of 1.5 and half above it).
enum class GiQuality : uint8_t { kAuto, kHalf, kFull };

struct GiConfig {
  GiLevel level = GiLevel::kOff;
  GiQuality quality = GiQuality::kAuto;
  // --native_gi_intensity: the bounce light times this (1: one physical bounce).
  float intensity = 1.0f;
  // --native_gi_radius: how far the bounce light is gathered from, in the game's world units
  // (metres); 0: the level's.
  float radius = 0.0f;
  // --native_gi_thickness: how deep a surface seen on the screen is taken to be (world units); 0:
  // a quarter of the radius.
  float thickness = 0.0f;
  // --native_gi_albedo: the brightest surface colour the composite assumes (the albedo estimate
  // never exceeds it); see docs/gi.md, "The composite".
  float albedo = 0.6f;
  // --native_gi_occlusion: how much of the flat ambient light the gathered visibility takes away
  // where surfaces hide the sky (0 to 1), when the ambient occlusion setting is off. With AO on,
  // the AO does that and the GI only adds light.
  float occlusion = 1.0f;
  // --native_gi_debug: 0 the picture; 1 the bounce light alone (the gathered light, before the
  // surface colour); 2 the light the composite adds (+) or takes away (-) on grey; 3 the visibility.
  int debug = 0;
  // --native_gi_temporal: blend in last frame's light where the camera saw the same surface (its
  // reprojection voted from the scene's draws); the noise then changes every frame.
  bool temporal = true;
  // --native_gi_gpu_time: the GI passes' GPU time, logged every 600 frames (ao_timer.h).
  bool gpu_time = false;

  bool enabled() const { return level != GiLevel::kOff; }
};

// The work of a level (docs/gi.md, "Levels").
struct GiLevelParams {
  uint32_t slices = 0;        // directions per pixel
  uint32_t steps = 0;         // samples per direction and side
  uint32_t denoise = 0;       // edge-aware blur passes (each twice as wide)
  bool half = true;           // working resolution: half of the scene target's
  float radius = 0.0f;        // world units
};
GiLevelParams GiParamsFor(GiLevel level);
// Add NDC jitter to a row-major clip transform, retaining its depth and homogeneous w.
// The GI camera vote then maps the actual jittered pixels between frames.
void ApplyGiJitter(float rows[16], float ndc_x, float ndc_y);

bool ParseGiLevel(const std::string& text, GiLevel& out);
bool ParseGiQuality(const std::string& text, GiQuality& out);
const char* GiLevelName(GiLevel l);
const char* GiQualityName(GiQuality q);

}  // namespace nr
