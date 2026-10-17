#pragma once
// Which GPU every device we make uses (task 203, Beta 9): the high-performance one the game draws
// with, never an integrated GPU when a discrete one is there, never a software adapter (WARP, the
// Basic Render Driver, lavapipe) and never a layered device such as Dozen ("Microsoft Direct3D12
// (...)", Vulkan on top of D3D12). A hybrid laptop (Intel UHD + RTX 4050) crashed in Intel's D3D12
// driver because the launcher's shader setup took the SDK's default, the first adapter (the iGPU).
//
// Pure selection only (no API calls), so the order is unit tested with fake adapter lists
// (backend/tests/gpu_choice_tests.cpp); plugin/gpu_preference.cpp fills the lists from DXGI and
// Vulkan and points the SDK's d3d12_adapter / vulkan_device at the choice.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nr::gpu_choice {

enum class Kind { kOther, kIntegrated, kDiscrete, kVirtual, kCpu };

struct Candidate {
  int index = -1;              // what the SDK cvar counts: EnumAdapters1 / vkEnumeratePhysicalDevices order
  std::string name;
  Kind kind = Kind::kOther;    // Vulkan's device type (DXGI has none: kOther)
  uint64_t local_memory = 0;   // dedicated video memory / largest device-local heap, bytes
  bool software = false;       // WARP, Basic Render Driver, a CPU Vulkan device
  bool usable = true;          // the API can make a device on it (D3D12: D3D12CreateDevice at 11_0)
  int preference = -1;         // rank in DXGI's high-performance order (D3D12); -1 when unknown
};

// "Microsoft Direct3D12 (Intel(R) UHD Graphics)" is Mesa's Dozen: Vulkan layered on D3D12.
inline bool IsLayeredName(std::string_view name) {
  return name.rfind("Microsoft Direct3D12", 0) == 0 || name.rfind("Microsoft Direct3D 12", 0) == 0;
}
// The Basic Render Driver (vendor 0x1414, device 0x8C) is a software adapter even without the flag.
inline bool IsBasicRender(uint32_t vendor, uint32_t device, std::string_view name) {
  return (vendor == 0x1414 && device == 0x8C) || name.rfind("Microsoft Basic Render", 0) == 0;
}

inline bool Eligible(const Candidate& c) {
  return c.usable && !c.software && c.kind != Kind::kCpu && !IsLayeredName(c.name);
}

inline int KindRank(Kind k) {
  switch (k) {
    case Kind::kDiscrete: return 0;
    case Kind::kIntegrated: return 1;
    case Kind::kVirtual: return 2;
    case Kind::kOther: return 3;
    default: return 4;
  }
}

// The chosen candidate's index, or -1 when none is eligible (the SDK's own choice then stands).
// Order: DXGI's high-performance rank when known; otherwise discrete before integrated before
// virtual, then the most local memory, then the lower index.
inline int Choose(const std::vector<Candidate>& candidates) {
  const Candidate* best = nullptr;
  auto better = [](const Candidate& a, const Candidate& b) {
    const bool ap = a.preference >= 0, bp = b.preference >= 0;
    if (ap && bp && a.preference != b.preference) return a.preference < b.preference;
    if (ap != bp) return ap;
    if (KindRank(a.kind) != KindRank(b.kind)) return KindRank(a.kind) < KindRank(b.kind);
    if (a.local_memory != b.local_memory) return a.local_memory > b.local_memory;
    return a.index < b.index;
  };
  for (const auto& c : candidates)
    if (Eligible(c) && (!best || better(c, *best))) best = &c;
  return best ? best->index : -1;
}

inline const Candidate* Find(const std::vector<Candidate>& candidates, int index) {
  for (const auto& c : candidates)
    if (c.index == index) return &c;
  return nullptr;
}

}  // namespace nr::gpu_choice
