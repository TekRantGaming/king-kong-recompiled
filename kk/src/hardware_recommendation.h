#pragma once

#include "platform.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace kk {
struct HardwareProfile {
  GpuInfo gpu;
  unsigned physical_cores = 0;
  uint64_t system_memory = 0;
  int width = 0, height = 0, refresh_hz = 0;
  bool steam_deck = false;
};
HardwareProfile DetectHardware();

struct HardwareTier {
  const char* id;
  const char* render_quality;
  const char* ao_quality;
  int ao_samples;
  bool ao_enabled;
  const char* gi;
};
// Conservative starting points, not measured performance guarantees.
inline constexpr std::array<HardwareTier, 3> kHardwareTiers = {{
    {"low", "performance", "half", 1, false, "low"},
    {"mid", "quality", "half", 2, true, "medium"},
    {"high", "quality", "auto", 3, true, "high"},
}};
inline size_t HardwareTierIndex(const HardwareProfile& h) {
  constexpr uint64_t GiB = uint64_t(1) << 30;
  std::string model = h.gpu.name;
  std::transform(model.begin(), model.end(), model.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const auto has = [&](std::string_view s) { return model.find(s) != std::string::npos; };
  const bool entry = has("gt  ") || has("gt 7") || has("gt 10") || has("gtx 10") || has("gtx 16") ||
                     has("rtx 3050") || has("rx 5") || has("rx 6400") || has("rx 6500") || has("a380");
  if (h.steam_deck || h.gpu.vendor == GpuVendor::kUnknown || h.gpu.dedicated_memory < 4 * GiB ||
      h.physical_cores < 4 || h.system_memory < 8 * GiB || entry) return 0;
  // Do not mistake a large old framebuffer for a fast GPU. Unknown models stay mid.
  const bool modern = has("rtx 30") || has("rtx 40") || has("rtx 50") || has("rx 67") || has("rx 68") ||
                      has("rx 69") || has("rx 77") || has("rx 78") || has("rx 79") || has("rx 90") ||
                      has("arc a770") || has("arc b580");
  return modern && h.gpu.dedicated_memory >= 10 * GiB && h.physical_cores >= 6 &&
         h.system_memory >= 16 * GiB ? 2 : 1;
}
inline const char* RecommendedRenderQuality(const HardwareTier& tier, int width, int height) {
  // High pixel counts need a more conservative mode even on dedicated GPUs.
  const uint64_t pixels = uint64_t(std::max(0, width)) * uint64_t(std::max(0, height));
  if (pixels > uint64_t(2560) * 1440 && std::string_view(tier.id) != "high") return "performance";
  return tier.render_quality;
}
struct UpscalerSupport {
  bool known = false, dlss = false, fsr3 = false, fsr4 = false, xess = false;
};
inline const char* RecommendedUpscaler(GpuVendor vendor, const UpscalerSupport& s, bool d3d12) {
  if (!s.known) {
    if (vendor == GpuVendor::kNvidia) return "dlss";
    if (vendor == GpuVendor::kIntel) return "xess";
    return "fsr3";
  }
  if (vendor == GpuVendor::kNvidia && s.dlss) return "dlss";
  if (vendor == GpuVendor::kIntel && s.xess) return "xess";
  if (vendor == GpuVendor::kAmd && d3d12 && s.fsr4) return "fsr4";
  return s.fsr3 ? "fsr3" : "off"; // presenter uses FSR 1 when no temporal SDK runs
}
// Explicit settings always win, including a choice equal to the current default.
inline bool PreserveHardwareSetting(bool explicit_value, bool custom_scale, bool original_look) {
  return explicit_value || custom_scale || original_look;
}
} // namespace kk
