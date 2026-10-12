// The screen-space global illumination's settings: see gi_config.h.

#include "backend/gi_config.h"

namespace nr {

GiLevelParams GiParamsFor(GiLevel level) {
  GiLevelParams p;
  switch (level) {
    case GiLevel::kLow:
      p = {2, 4, 2, true, 1.5f};
      break;
    case GiLevel::kMedium:
      p = {3, 6, 3, true, 2.0f};
      break;
    case GiLevel::kHigh:
      p = {4, 8, 3, false, 2.5f};
      break;
    default:
      break;
  }
  return p;
}

bool ParseGiLevel(const std::string& t, GiLevel& out) {
  if (t == "off" || t == "0" || t.empty()) {
    out = GiLevel::kOff;
    return true;
  }
  if (t == "low" || t == "1") {
    out = GiLevel::kLow;
    return true;
  }
  if (t == "medium" || t == "2") {
    out = GiLevel::kMedium;
    return true;
  }
  if (t == "high" || t == "3") {
    out = GiLevel::kHigh;
    return true;
  }
  return false;
}

bool ParseGiQuality(const std::string& t, GiQuality& out) {
  if (t == "auto" || t.empty()) {
    out = GiQuality::kAuto;
    return true;
  }
  if (t == "half") {
    out = GiQuality::kHalf;
    return true;
  }
  if (t == "full") {
    out = GiQuality::kFull;
    return true;
  }
  return false;
}

const char* GiLevelName(GiLevel l) {
  switch (l) {
    case GiLevel::kLow: return "low";
    case GiLevel::kMedium: return "medium";
    case GiLevel::kHigh: return "high";
    default: return "off";
  }
}

const char* GiQualityName(GiQuality q) {
  return q == GiQuality::kHalf ? "half" : (q == GiQuality::kFull ? "full" : "auto");
}

}  // namespace nr
