// The ambient occlusion's settings: see ao_config.h.

#include "backend/ao_config.h"

namespace nr {

bool ParseAoMethod(const std::string& t, AoMethod& out) {
  if (t == "legacy" || t.empty()) {
    out = AoMethod::kLegacy;
    return true;
  }
  if (t == "gtao") {
    out = AoMethod::kGtao;
    return true;
  }
  return false;
}

bool ParseAoApply(const std::string& t, AoApply& out) {
  if (t == "resolve" || t.empty()) {
    out = AoApply::kResolve;
    return true;
  }
  if (t == "opaque") {
    out = AoApply::kOpaque;
    return true;
  }
  if (t == "ambient") {
    out = AoApply::kAmbient;
    return true;
  }
  return false;
}

bool ParseAoQuality(const std::string& t, AoQuality& out) {
  if (t == "full" || t.empty()) {
    out = AoQuality::kFull;
    return true;
  }
  if (t == "half") {
    out = AoQuality::kHalf;
    return true;
  }
  if (t == "auto") {
    out = AoQuality::kAuto;
    return true;
  }
  return false;
}

const char* AoMethodName(AoMethod m) { return m == AoMethod::kGtao ? "gtao" : "legacy"; }
const char* AoApplyName(AoApply a) {
  return a == AoApply::kOpaque ? "opaque" : (a == AoApply::kAmbient ? "ambient" : "resolve");
}
const char* AoQualityName(AoQuality q) {
  return q == AoQuality::kHalf ? "half" : (q == AoQuality::kAuto ? "auto" : "full");
}

}  // namespace nr
