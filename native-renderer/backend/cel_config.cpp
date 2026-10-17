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

float CelBandAnchor(float log2_average) {
  return std::clamp(std::isfinite(log2_average) ? log2_average : CelTuning::kAnchorDefault, CelTuning::kAnchorMin,
                    CelTuning::kAnchorMax);
}

float CelBandLuma(float y, uint32_t bands, float anchor) {
  if (bands < 2) return y;
  const float n = float(std::min(bands, 8u));
  const float low = anchor - 0.5f * CelTuning::kStops;  // the darkest band's floor (log2)
  const float s = (std::log2(std::max(y, 1e-6f)) - low) / CelTuning::kStops;  // 0 to 1 over the bands
  if (s <= 0.0f || s >= 1.0f) return y;
  const float t = s * n;
  const float i = std::floor(t), f = t - i;
  auto level_of = [&](float band) { return std::exp2(low + (band + CelTuning::kLevel) * CelTuning::kStops / n); };
  // Into the next band over the last kSoftness of this one.
  const float blend = i < n - 1.0f ? Smoothstep(1.0f - CelTuning::kSoftness, 1.0f, f) : 0.0f;
  const float level = level_of(i) + (level_of(i + 1.0f) - level_of(i)) * blend;
  // The pixels darker or brighter than the bands keep their value; softly into the first and last band.
  const float edge = CelTuning::kSoftness / n;
  const float inside = Smoothstep(0.0f, edge, s) * (1.0f - Smoothstep(1.0f - edge, 1.0f, s));
  return y + (level - y) * inside;
}

void CelBandColour(const float in[3], uint32_t bands, float strength, float anchor, float out[3], float local_luma) {
  const float y = 0.2126f * in[0] + 0.7152f * in[1] + 0.0722f * in[2];
  const float yl = local_luma > 0.0f ? local_luma : y;
  float scale = 1.0f;
  if (y > 1e-5f && yl > 1e-5f) scale = CelBandLuma(yl, bands, anchor) / yl;
  float c[3] = {in[0] * scale, in[1] * scale, in[2] * scale};
  // A saturated colour scaled up may pass white in one channel: scale it back (the hue stays).
  const float top = std::max({c[0], c[1], c[2]}), was = std::max({in[0], in[1], in[2], 1.0f});
  if (top > was) for (float& v : c) v *= was / top;
  const float a = std::clamp(strength, 0.0f, 1.0f);
  for (int i = 0; i < 3; ++i) out[i] = in[i] + (c[i] - in[i]) * a;
}

float CelBandRadius(uint32_t height) {
  return std::clamp(CelTuning::kBandRadius1080 * float(height) / 1080.0f, 1.0f, CelTuning::kMaxRadius);
}

float CelRadius(float outline, uint32_t height, float* opacity) {
  const float r = std::max(outline, 0.0f) * float(height) / 1080.0f;
  if (opacity) *opacity = std::clamp(r, 0.0f, 1.0f);
  return std::clamp(r, 1.0f, CelTuning::kMaxRadius);
}

}  // namespace nr
