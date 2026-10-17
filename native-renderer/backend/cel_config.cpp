// Cel shading's settings and its band function: see cel_config.h. Keep CelBandLuma and
// CelBandColour equal to shaders/cel.hlsl.

#include "backend/cel_config.h"

#include <algorithm>
#include <cmath>

namespace nr {

namespace {

float Smoothstep(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

float CelBandLuma(float y, uint32_t bands) {
  if (bands < 2) return y;
  const float n = float(std::min(bands, 8u));
  const float s = std::log2(std::max(y, 1e-6f)) / CelTuning::kStops + 1.0f;  // 0 at the darkest band's floor, 1 white
  if (s <= 0.0f || s >= 1.0f) return y;
  const float t = s * n;
  const float i = std::floor(t), f = t - i;
  // Each band's flat level (the top band's nearer white, so highlights stay bright).
  auto level_of = [&](float band) {
    const float at = band >= n - 1.0f ? CelTuning::kTopLevel : CelTuning::kLevel;
    return std::exp2(((band + at) / n - 1.0f) * CelTuning::kStops);
  };
  // Into the next band over the last kSoftness of this one (the top band stays flat up to white).
  const float blend = i < n - 1.0f ? Smoothstep(1.0f - CelTuning::kSoftness, 1.0f, f) : 0.0f;
  const float level = level_of(i) + (level_of(i + 1.0f) - level_of(i)) * blend;
  // From the darkest pixels (kept as they are) into the first band, softly as well.
  const float from_dark = Smoothstep(0.0f, CelTuning::kSoftness / n, s);
  return y + (level - y) * from_dark;
}

void CelBandColour(const float in[3], uint32_t bands, float strength, float out[3]) {
  const float y = 0.2126f * in[0] + 0.7152f * in[1] + 0.0722f * in[2];
  float scale = 1.0f;
  if (y > 1e-5f) scale = CelBandLuma(y, bands) / y;
  float c[3] = {in[0] * scale, in[1] * scale, in[2] * scale};
  // A saturated colour scaled up may pass white in one channel: scale it back (the hue stays).
  const float top = std::max({c[0], c[1], c[2]}), was = std::max({in[0], in[1], in[2], 1.0f});
  if (top > was) for (float& v : c) v *= was / top;
  const float a = std::clamp(strength, 0.0f, 1.0f);
  for (int i = 0; i < 3; ++i) out[i] = in[i] + (c[i] - in[i]) * a;
}

float CelRadius(float outline, uint32_t height, float* opacity) {
  const float r = std::max(outline, 0.0f) * float(height) / 1080.0f;
  if (opacity) *opacity = std::clamp(r, 0.0f, 1.0f);
  return std::clamp(r, 1.0f, CelTuning::kMaxRadius);
}

}  // namespace nr
