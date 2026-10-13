#pragma once

namespace nr {
// Keep these in step with the six GameRenderer bindless table capacities.
inline constexpr unsigned kNativeSampledImageSlots = 16384 + 256 + 2048 + 256;
inline constexpr unsigned kNativeStorageBufferSlots = 16384;
inline constexpr unsigned kNativeSamplerSlots = 1024;
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
bool CheckNativeBindlessLimits(unsigned bound_sets, const Limits& p, Report report) {
  bool supported = true;
  auto require = [&](unsigned available, unsigned needed, const char* name) {
    if (available < needed) { report(name, available, needed); supported = false; }
  };
  require(bound_sets, 7, "maxBoundDescriptorSets");
  require(p.maxPerStageDescriptorUpdateAfterBindSampledImages, kNativeSampledImageSlots, "maxPerStageDescriptorUpdateAfterBindSampledImages");
  require(p.maxPerStageDescriptorUpdateAfterBindStorageBuffers, kNativeStorageBufferSlots, "maxPerStageDescriptorUpdateAfterBindStorageBuffers");
  require(p.maxPerStageDescriptorUpdateAfterBindSamplers, kNativeSamplerSlots, "maxPerStageDescriptorUpdateAfterBindSamplers");
  require(p.maxDescriptorSetUpdateAfterBindSampledImages, kNativeSampledImageSlots, "maxDescriptorSetUpdateAfterBindSampledImages");
  require(p.maxDescriptorSetUpdateAfterBindStorageBuffers, kNativeStorageBufferSlots, "maxDescriptorSetUpdateAfterBindStorageBuffers");
  require(p.maxDescriptorSetUpdateAfterBindSamplers, kNativeSamplerSlots, "maxDescriptorSetUpdateAfterBindSamplers");
  const unsigned descriptors = kNativeSampledImageSlots + kNativeStorageBufferSlots + kNativeSamplerSlots;
  require(p.maxPerStageUpdateAfterBindResources, descriptors + 5, "maxPerStageUpdateAfterBindResources");
  require(p.maxUpdateAfterBindDescriptorsInAllPools, descriptors, "maxUpdateAfterBindDescriptorsInAllPools");
  return supported;
}
}  // namespace nr
