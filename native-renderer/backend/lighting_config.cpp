// The lighting options: see lighting_config.h.

#include "backend/lighting_config.h"

namespace nr {

float ShadowScaleForQuality(ShadowQuality q) {
  switch (q) {
    case ShadowQuality::kHigh: return 2.0f;
    case ShadowQuality::kUltra: return 1.5f;  // Beta 14, task 348a (2 before): PCSS blurs the finer texels away
    default: return 0.0f;
  }
}

bool ParseShadowQuality(const std::string& t, ShadowQuality& out) {
  if (t == "original" || t.empty()) {
    out = ShadowQuality::kOriginal;
    return true;
  }
  if (t == "high") {
    out = ShadowQuality::kHigh;
    return true;
  }
  if (t == "ultra") {
    out = ShadowQuality::kUltra;
    return true;
  }
  return false;
}

bool ParseWaterReflections(const std::string& t, WaterReflections& out) {
  if (t == "original" || t.empty()) {
    out = WaterReflections::kOriginal;
    return true;
  }
  if (t == "ssr") {
    out = WaterReflections::kSsr;
    return true;
  }
  return false;
}

bool ParseVolumetrics(const std::string& t, Volumetrics& out) {
  if (t == "off" || t == "original" || t.empty()) {
    out = Volumetrics::kOff;
    return true;
  }
  if (t == "low") {
    out = Volumetrics::kLow;
    return true;
  }
  if (t == "high") {
    out = Volumetrics::kHigh;
    return true;
  }
  return false;
}

const char* ShadowQualityName(ShadowQuality q) {
  return q == ShadowQuality::kHigh ? "high" : (q == ShadowQuality::kUltra ? "ultra" : "original");
}
const char* WaterReflectionsName(WaterReflections w) { return w == WaterReflections::kSsr ? "ssr" : "original"; }
const char* VolumetricsName(Volumetrics v) {
  return v == Volumetrics::kLow ? "low" : (v == Volumetrics::kHigh ? "high" : "off");
}

}  // namespace nr
