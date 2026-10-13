#pragma once

namespace nr {
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
}  // namespace nr
