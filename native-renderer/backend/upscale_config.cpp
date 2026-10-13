#include "backend/upscale_config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace nr {

namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  // "ultra-performance" and "ultra performance" as "ultra_performance".
  std::replace(s.begin(), s.end(), '-', '_');
  std::replace(s.begin(), s.end(), ' ', '_');
  return s;
}

}  // namespace

const char* UpscalerKindName(UpscalerKind k) {
  switch (k) {
    case UpscalerKind::kOff: return "off";
    case UpscalerKind::kAuto: return "auto";
    case UpscalerKind::kFsr3: return "fsr3";
    case UpscalerKind::kFsr4: return "fsr4";
    case UpscalerKind::kDlss: return "dlss";
    case UpscalerKind::kXess: return "xess";
    case UpscalerKind::kBilinear: return "bilinear";
  }
  return "?";
}

bool ParseUpscalerKind(const std::string& text, UpscalerKind& out) {
  const std::string s = Lower(text);
  if (s.empty() || s == "off" || s == "none" || s == "0" || s == "false") out = UpscalerKind::kOff;
  else if (s == "auto") out = UpscalerKind::kAuto;
  else if (s == "fsr3" || s == "fsr" || s == "fsr3.1" || s == "fsr31") out = UpscalerKind::kFsr3;
  else if (s == "fsr4") out = UpscalerKind::kFsr4;
  else if (s == "dlss" || s == "dlaa") out = UpscalerKind::kDlss;
  else if (s == "xess") out = UpscalerKind::kXess;
  else if (s == "bilinear") out = UpscalerKind::kBilinear;
  else return false;
  return true;
}

const char* UpscaleQualityName(UpscaleQuality q) {
  switch (q) {
    case UpscaleQuality::kNative: return "native";
    case UpscaleQuality::kQuality: return "quality";
    case UpscaleQuality::kBalanced: return "balanced";
    case UpscaleQuality::kPerformance: return "performance";
    case UpscaleQuality::kUltraPerformance: return "ultra_performance";
  }
  return "?";
}

bool ParseUpscaleQuality(const std::string& text, UpscaleQuality& out) {
  const std::string s = Lower(text);
  if (s == "native" || s == "native_aa" || s == "nativeaa" || s == "aa" || s == "dlaa") out = UpscaleQuality::kNative;
  else if (s.empty() || s == "quality") out = UpscaleQuality::kQuality;
  else if (s == "balanced") out = UpscaleQuality::kBalanced;
  else if (s == "performance") out = UpscaleQuality::kPerformance;
  else if (s == "ultra_performance" || s == "ultraperformance" || s == "ultra") out = UpscaleQuality::kUltraPerformance;
  else return false;
  return true;
}

float DefaultUpscaleRatio(UpscaleQuality q) {
  switch (q) {
    case UpscaleQuality::kNative: return 1.0f;
    case UpscaleQuality::kQuality: return 1.5f;
    case UpscaleQuality::kBalanced: return 1.7f;
    case UpscaleQuality::kPerformance: return 2.0f;
    case UpscaleQuality::kUltraPerformance: return 3.0f;
  }
  return 1.0f;
}

char DlssPresetFor(char preset, UpscaleQuality quality) {
  if (preset != kDlssPresetLatest) return preset;
  return quality == UpscaleQuality::kUltraPerformance ? 'L' : 'M';
}

bool ParseDlssPreset(const std::string& text, char& out) {
  const std::string s = Lower(text);
  if (s.empty() || s == "default" || s == "0") {
    out = 0;
    return true;
  }
  if (s == "latest") {
    out = kDlssPresetLatest;
    return true;
  }
  if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') {
    out = char(std::toupper(static_cast<unsigned char>(s[0])));
    return true;
  }
  return false;
}

uint32_t UpscaleInputSize(uint32_t output, float ratio) {
  if (!(ratio > 0.0f)) return std::max(output, 1u);
  const double v = std::floor(double(output) / double(ratio) + 0.5);
  return uint32_t(std::max(v, 1.0));
}

std::string DescribeUpscaleConfig(const UpscaleConfig& c) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s, %s, at %s%s%s, sharpness %s, mip bias %s, reactive %.2f, debug %u",
                UpscalerKindName(c.kind), UpscaleQualityName(c.quality),
                c.point == UpscalePoint::kPost ? "the post effects" : "Present",
                c.dlss_preset ? ", DLSS preset " : "", c.dlss_preset == kDlssPresetLatest ? "latest" : c.dlss_preset ? std::string(1, c.dlss_preset).c_str() : "",
                c.sharpness >= 0.0f ? std::to_string(c.sharpness).c_str() : "off",
                c.mip_bias_auto ? "auto" : std::to_string(c.mip_bias).c_str(), c.reactive, c.debug);
  return buf;
}

}  // namespace nr
