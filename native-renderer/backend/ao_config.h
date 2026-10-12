// The ambient occlusion's settings beyond the launcher's ao_mode and ao_strength
// (docs/ao.md, "Settings"). The defaults are today's AO, unchanged.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "backend/ao_classifier.h"

namespace nr {

// --native_ao_method: legacy (the occlusion ported from the Xenos plugin, ambient_occlusion.h) or
// gtao (XeGTAO, gtao.h).
enum class AoMethod : uint8_t { kLegacy, kGtao };
// --native_ao_apply: when the occlusion reaches the picture. resolve: at the scene copy after the
// full-screen depth resolve (today's moment); opaque: at the end of the opaque pass, before the
// first transparent or effect draw; ambient: inside the lighting pixel shaders the shader list
// names (their ambient term times the occlusion), with opaque as the fallback. The legacy method
// always uses resolve.
enum class AoApply : uint8_t { kResolve, kOpaque, kAmbient };
// --native_ao_quality: GTAO at half or full resolution of the scene target; auto: full up to a render scale
// of 1.5 per axis, half above it (at scale 2 that is the same number of pixels as full at scale 1).
enum class AoQuality : uint8_t { kHalf, kFull, kAuto };

struct AoConfig {
  AoMethod method = AoMethod::kLegacy;
  AoApply apply = AoApply::kResolve;
  AoQuality quality = AoQuality::kFull;
  // --native_ao_radius: the occlusion radius in the game's world units; 0 picks it from the camera:
  // the legacy method's radius for the strength (6, 8 or 12 times the near plane distance).
  float radius = 0.0f;
  // --native_ao_radius_scale: the radius from the camera times this (tuning; 1 keeps it).
  float radius_scale = 1.0f;
  // --native_ao_falloff: XeGTAO's falloff range, the part of the radius over which an occluder's
  // weight fades out (0 keeps XeGTAO's 0.615).
  float falloff = 0.0f;
  // --native_ao_power: XeGTAO's final power (0: from ao_strength, Gtao::FinalPower).
  float power = 0.0f;
  // --native_ao_fade: the occlusion fades out linearly to this many near-plane distances (0: none;
  // the legacy method uses 2000).
  float fade = 0.0f;
  // --native_ao_dump_every: every this many frames, the classifier's view of each draw in the log
  // (as --native_dump_frame does for one frame); 0 off.
  uint32_t dump_every = 0;
  // --native_ao_gpu_time: the AO passes' GPU time, logged every 600 frames (ao_timer.h).
  bool gpu_time = false;
  // --native_ao_rules: classifier rules placed before the built-in ones (ao_classifier.h).
  std::vector<AoRule> rules;
  // --native_ao_shaders: the lighting pixel shaders of the ambient-only mode and the term of each
  // that the occlusion multiplies (the text of the list, kkshaders/ambient.h's format).
  std::string ambient_shaders;

  bool gtao() const { return method == AoMethod::kGtao; }
  bool ambient() const { return gtao() && apply == AoApply::kAmbient; }
};

bool ParseAoMethod(const std::string& text, AoMethod& out);
bool ParseAoApply(const std::string& text, AoApply& out);
bool ParseAoQuality(const std::string& text, AoQuality& out);
const char* AoMethodName(AoMethod m);
const char* AoApplyName(AoApply a);
const char* AoQualityName(AoQuality q);

}  // namespace nr
