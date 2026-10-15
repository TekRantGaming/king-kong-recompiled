#pragma once
#include <algorithm>
#include <array>
#include <cstddef>

namespace nr {
// Keep these in step with the six GameRenderer bindless table capacities.
inline constexpr unsigned kNativeSampledImageSlots = 16384 + 256 + 2048 + 256;
inline constexpr unsigned kNativeStorageBufferSlots = 16384;
inline constexpr unsigned kNativeSamplerSlots = 1024;
// In GameRenderer table order. Floors leave room for all fetches in a draw,
// including an AO view. Capacities are layout maxima, not shader ABI constants.
using NativeTableCapacities = std::array<unsigned, 6>;
inline constexpr NativeTableCapacities kNativeTableCapacities{16384, 256, 2048, 256, 1024, 16384};
inline constexpr NativeTableCapacities kSmallTableCapacities{1024, 128, 128, 128, 128, 1024};
inline constexpr NativeTableCapacities kMinimumTableCapacities{128, 128, 128, 128, 128, 256};
// b0-b5: VS, PS, draw, scale, ambient AO and lighting constants.
inline constexpr unsigned kNativeConstantBufferSlots = 6;

template <class Limits>
bool NativeTablesFit(const NativeTableCapacities& c, const Limits& p) {
  const unsigned images = c[0] + c[1] + c[2] + c[3];
  const unsigned all = images + c[4] + c[5];
  return images <= std::min(p.maxPerStageDescriptorUpdateAfterBindSampledImages, p.maxDescriptorSetUpdateAfterBindSampledImages)
      && c[5] <= std::min(p.maxPerStageDescriptorUpdateAfterBindStorageBuffers, p.maxDescriptorSetUpdateAfterBindStorageBuffers)
      && c[4] <= std::min(p.maxPerStageDescriptorUpdateAfterBindSamplers, p.maxDescriptorSetUpdateAfterBindSamplers)
      && all + kNativeConstantBufferSlots <= p.maxPerStageUpdateAfterBindResources
      && all <= p.maxUpdateAfterBindDescriptorsInAllPools;
}

template <class Limits>
bool SelectNativeTableCapacities(unsigned bound_sets, const Limits& p,
                                 bool force_small, NativeTableCapacities& c) {
  c = force_small ? kSmallTableCapacities : kNativeTableCapacities;
  if (bound_sets < 7 || !NativeTablesFit(kMinimumTableCapacities, p)) return false;
  while (!NativeTablesFit(c, p)) {
    // Shrink the largest remaining table. Check every aggregate again, so
    // combined image and resource limits cannot slip through the selection.
    std::size_t largest = c.size();
    for (std::size_t i = 0; i < c.size(); ++i)
      if (c[i] > kMinimumTableCapacities[i] && (largest == c.size() || c[i] > c[largest])) largest = i;
    if (largest == c.size()) return false;
    c[largest] = std::max(c[largest] / 2, kMinimumTableCapacities[largest]);
  }
  return true;
}
// These are the features used by translated bindless shaders and NVRHI's
// PARTIALLY_BOUND / UPDATE_AFTER_BIND / UPDATE_UNUSED_WHILE_PENDING layouts.
// Do not require optional indexing features that these layouts do not use.
template <class Properties, class Report>
bool CheckNativeBindlessFeatures(const Properties& p, Report report) {
  bool supported = true;
  auto require = [&](bool enabled, const char* name) {
    if (!enabled) { report(name); supported = false; }
  };
  require(p.shaderSampledImageArrayDynamicIndexing, "shaderSampledImageArrayDynamicIndexing");
  require(p.shaderStorageBufferArrayDynamicIndexing, "shaderStorageBufferArrayDynamicIndexing");
  require(p.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing");
  require(p.shaderStorageBufferArrayNonUniformIndexing, "shaderStorageBufferArrayNonUniformIndexing");
  require(p.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind");
  require(p.descriptorBindingStorageBufferUpdateAfterBind, "descriptorBindingStorageBufferUpdateAfterBind");
  require(p.descriptorBindingUpdateUnusedWhilePending, "descriptorBindingUpdateUnusedWhilePending");
  require(p.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
  require(p.runtimeDescriptorArray, "runtimeDescriptorArray");
  return supported;
}

template <class Limits, class Report>
bool CheckNativeBindlessLimits(unsigned bound_sets, const Limits& p, Report report,
                               const NativeTableCapacities& c = kNativeTableCapacities) {
  bool supported = true;
  auto require = [&](unsigned available, unsigned needed, const char* name) {
    if (available < needed) { report(name, available, needed); supported = false; }
  };
  const unsigned images = c[0] + c[1] + c[2] + c[3];
  require(bound_sets, 7, "maxBoundDescriptorSets");
  require(p.maxPerStageDescriptorUpdateAfterBindSampledImages, images, "maxPerStageDescriptorUpdateAfterBindSampledImages");
  require(p.maxPerStageDescriptorUpdateAfterBindStorageBuffers, c[5], "maxPerStageDescriptorUpdateAfterBindStorageBuffers");
  require(p.maxPerStageDescriptorUpdateAfterBindSamplers, c[4], "maxPerStageDescriptorUpdateAfterBindSamplers");
  require(p.maxDescriptorSetUpdateAfterBindSampledImages, images, "maxDescriptorSetUpdateAfterBindSampledImages");
  require(p.maxDescriptorSetUpdateAfterBindStorageBuffers, c[5], "maxDescriptorSetUpdateAfterBindStorageBuffers");
  require(p.maxDescriptorSetUpdateAfterBindSamplers, c[4], "maxDescriptorSetUpdateAfterBindSamplers");
  const unsigned descriptors = images + c[5] + c[4];
  require(p.maxPerStageUpdateAfterBindResources, descriptors + kNativeConstantBufferSlots, "maxPerStageUpdateAfterBindResources");
  require(p.maxUpdateAfterBindDescriptorsInAllPools, descriptors, "maxUpdateAfterBindDescriptorsInAllPools");
  return supported;
}
}  // namespace nr
