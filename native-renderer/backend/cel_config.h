// Cel shading (docs/cel.md): a screen effect on the 3D scene at the end of its opaque pass: ink
// outlines from depth steps and normal creases, and the lighting in a few flat colour bands (the
// hue kept). Water, smoke, particles, fog, the sky and the HUD are drawn after it (or skipped) and
// are never outlined or banded. Off by default; the Original look keeps it off.
//
// The band function is shared with the shader (shaders/cel.hlsl, CelBandLuma) so the tests can
// check the GPU against it.
#pragma once

#include <cstdint>

namespace nr {

struct CelConfig {
  bool enabled = false;      // native_cel_shading
  float outline = 1.5f;      // native_cel_outline: line thickness in pixels of a 1080-line picture (0: no lines)
  float darkness = 0.8f;     // native_cel_darkness: 0 no lines, 1 black lines
  uint32_t bands = 3;        // native_cel_bands: 2 to 4 light levels
  float strength = 0.6f;     // native_cel_strength: 0 the original shading, 1 fully flat bands
  int debug = 0;             // native_cel_debug: 1 the lines alone on white, 2 depth lines cyan + creases magenta
  bool gpu_time = false;     // native_cel_gpu_time: timer queries logged (else the graphics panel's only)

  bool lines() const { return outline > 0.0f && darkness > 0.0f; }
  bool banding() const { return strength > 0.0f && bands >= 2; }
  bool any() const { return enabled && (lines() || banding() || debug != 0); }
};

// Tuning (docs/cel.md, "How it works"); not settings.
struct CelTuning {
  // Depth lines: a neighbour this much farther than this pixel's own surface predicts (relative view
  // depth) starts a line, and this much makes it full.
  static constexpr float kDepthStart = 0.03f, kDepthFull = 0.10f;
  // Crease lines: the angle between the normals (degrees) where a line starts and where it is full.
  static constexpr float kCreaseStartDegrees = 40.0f, kCreaseFullDegrees = 65.0f;
  // Crease lines are this much of a depth line's darkness.
  static constexpr float kCreaseWeight = 0.85f;
  // Lines fade out with distance (view units, metres in King Kong): full until kFadeStart, gone at kFadeEnd.
  static constexpr float kFadeStart = 12.0f, kFadeEnd = 60.0f;
  // Bands: log2 steps of the picture's (gamma encoded) luminance below white that the bands share.
  // Darker pixels and pixels above white keep their value.
  static constexpr float kStops = 6.0f;
  // The share of a band over which it blends into the next (soft edges: no shimmer on gradients).
  static constexpr float kSoftness = 0.12f;
  // Where in its band (in log2 steps) a band's flat level lies: 0.5 its middle, higher a little brighter.
  static constexpr float kLevel = 0.6f, kTopLevel = 0.85f;
  // The line radius in host pixels is outline * picture height / 1080, at most this.
  static constexpr float kMaxRadius = 12.0f;
};

// The band function (shaders/cel.hlsl, CelBandLuma): luminance y (gamma encoded, 0..1) into `bands`
// flat levels spread over kStops log2 steps below white, with soft edges.
float CelBandLuma(float y, uint32_t bands);
// A colour (gamma encoded rgb) with its luminance banded and the hue kept, blended by strength.
void CelBandColour(const float in[3], uint32_t bands, float strength, float out[3]);
// The line radius in host pixels for a picture `height` host pixels tall (thin lines keep radius 1
// and are drawn fainter: `opacity`).
float CelRadius(float outline, uint32_t height, float* opacity);

}  // namespace nr
