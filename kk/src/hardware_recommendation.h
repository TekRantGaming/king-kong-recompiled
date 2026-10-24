#pragma once

#include "platform.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
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
// Intel's MTL/ARL/LNL/PTL integrated IDs (Arc class integrated graphics), including adapters with generic names.
// https://github.com/torvalds/linux/blob/master/include/drm/intel/pciids.h
inline constexpr uint32_t kIntelArcIntegratedIds[] = {
    0x7D51, 0x7DD1, 0x7D41, 0x7D67, 0xB640, 0x7D40, 0x7D45,
    0x7D55, 0x7D60, 0x7DD5, 0x6420, 0x64A0, 0x64B0,
    0xB080, 0xB081, 0xB082, 0xB083, 0xB084, 0xB085,
    0xB086, 0xB087, 0xB08F, 0xB090, 0xB0A0, 0xB0B0};

// Task 368 (crash reports G1, owner 23 Oct): the game's graphics card (DetectGpu: Windows' high performance
// adapter, so a hybrid laptop's NVIDIA or AMD card wins over its Intel one) is older Intel integrated graphics
// (UHD, Iris, HD), where Intel's Direct3D 12 driver crashed making the setup's device. Then the default API is
// Vulkan and the setup skips the DirectX 12 shader pack. Told by: Intel's PCI vendor (0x8086, or the vendor
// when no ID is known), no "Arc" in the name, not one of the Arc class integrated IDs above (those run both
// APIs), and under 1 GiB of its own video memory (integrated graphics report 128 MB or less; Arc cards have
// 4 GiB or more). An unknown card is never matched.
inline bool IntelIntegratedNotArc(const GpuInfo& gpu) {
  const bool intel = gpu.vendor_id ? gpu.vendor_id == 0x8086 : gpu.vendor == GpuVendor::kIntel;
  if (!intel || gpu.name.empty()) return false;
  std::string model = gpu.name;
  std::transform(model.begin(), model.end(), model.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (model.find("arc") != std::string::npos) return false;
  if (std::find(std::begin(kIntelArcIntegratedIds), std::end(kIntelArcIntegratedIds), gpu.device_id) !=
      std::end(kIntelArcIntegratedIds))
    return false;
  return gpu.dedicated_memory < (uint64_t(1) << 30);
}

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
// Task 338: only the first install uses these more conservative choices. A later
// launch and the explicit Recommended action retain the existing tier policy.
inline size_t FirstSetupHardwareTierIndex(const HardwareProfile& h) {
  constexpr uint64_t GiB = uint64_t(1) << 30;
  if (h.steam_deck || h.gpu.dedicated_memory <= 6 * GiB || h.physical_cores < 6) return 0;  // 6-core desktops with a strong GPU are common: not Low by cores alone (coordinator)
  std::string model;
  for (size_t i = 0; i < h.gpu.name.size(); ++i) {
    // DXGI names may put (R) and (TM) between the words used below.
    if (h.gpu.name[i] == '(') {
      const auto end = h.gpu.name.find(')', i);
      if (end != std::string::npos) {
        std::string tag = h.gpu.name.substr(i + 1, end - i - 1);
        std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (tag == "r" || tag == "tm") {
          if (!model.empty() && model.back() != ' ') model += ' ';
          i = end;
          continue;
        }
      }
    }
    const auto c = static_cast<unsigned char>(h.gpu.name[i]);
    if (std::isalnum(c)) model += char(std::tolower(c));
    else if (!model.empty() && model.back() != ' ') model += ' ';
  }
  if (!model.empty() && model.back() == ' ') model.pop_back();
  const auto has = [&](std::string_view s) { return model.find(s) != std::string::npos; };
  const auto vendor = [&](uint32_t id, GpuVendor fallback) {
    return h.gpu.vendor_id ? h.gpu.vendor_id == id : h.gpu.vendor == fallback;
  };
  const bool amd_apu = vendor(0x1002, GpuVendor::kAmd) &&
      (has("radeon graphics") || has("radeon 780m") || has("radeon 890m") ||
       has("radeon 760m") || has("radeon 860m") || has("radeon 880m") ||
       has("ryzen z1") || has("ryzen z2"));
  const auto& intel_integrated = kIntelArcIntegratedIds;
  const bool intel_apu = vendor(0x8086, GpuVendor::kIntel) &&
      (std::find(std::begin(intel_integrated), std::end(intel_integrated), h.gpu.device_id) !=
           std::end(intel_integrated) || has("arc graphics") || has("arc integrated") ||
       has("arc 130") || has("arc 140") || has("arc 7 core") || has("arc 8 core") ||
       has("arc b370") || has("arc b390") || has("arc pro b370") || has("arc pro b390"));
  const bool entry_laptop = vendor(0x10DE, GpuVendor::kNvidia) &&
      (has("rtx 4050") || has("rtx 3050") || has("rtx 2050")) &&
      (has("laptop") || has("notebook") || has("mobile"));
  return amd_apu || intel_apu || entry_laptop ? 0 : HardwareTierIndex(h);
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
