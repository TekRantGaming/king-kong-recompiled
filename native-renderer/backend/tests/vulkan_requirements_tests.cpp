#include "backend/vulkan_requirements.h"
#include <cstdio>
#include <string>
#include <vector>

struct Features {
  bool shaderSampledImageArrayDynamicIndexing = true;
  bool shaderStorageBufferArrayDynamicIndexing = true;
  bool shaderSampledImageArrayNonUniformIndexing = true;
  bool shaderStorageBufferArrayNonUniformIndexing = true;
  bool descriptorBindingSampledImageUpdateAfterBind = true;
  bool descriptorBindingStorageBufferUpdateAfterBind = true;
  bool descriptorBindingUpdateUnusedWhilePending = true;
  bool descriptorBindingPartiallyBound = true;
  bool runtimeDescriptorArray = true;
};

struct Limits {
  unsigned maxPerStageDescriptorUpdateAfterBindSampledImages = nr::kNativeSampledImageSlots;
  unsigned maxPerStageDescriptorUpdateAfterBindStorageBuffers = nr::kNativeStorageBufferSlots;
  unsigned maxPerStageDescriptorUpdateAfterBindSamplers = nr::kNativeSamplerSlots;
  unsigned maxDescriptorSetUpdateAfterBindSampledImages = nr::kNativeSampledImageSlots;
  unsigned maxDescriptorSetUpdateAfterBindStorageBuffers = nr::kNativeStorageBufferSlots;
  unsigned maxDescriptorSetUpdateAfterBindSamplers = nr::kNativeSamplerSlots;
  unsigned maxPerStageUpdateAfterBindResources = nr::kNativeSampledImageSlots + nr::kNativeStorageBufferSlots + nr::kNativeSamplerSlots + 5;
  unsigned maxUpdateAfterBindDescriptorsInAllPools = nr::kNativeSampledImageSlots + nr::kNativeStorageBufferSlots + nr::kNativeSamplerSlots;
};

int main() {
  Features p;
  std::vector<std::string> missing;
  auto report = [&](const char* name) { missing.emplace_back(name); };
  if (!nr::CheckNativeBindlessFeatures(p, report) || !missing.empty()) return 1;
  bool Features::* members[] = {
    &Features::shaderSampledImageArrayDynamicIndexing, &Features::shaderStorageBufferArrayDynamicIndexing,
    &Features::shaderSampledImageArrayNonUniformIndexing, &Features::shaderStorageBufferArrayNonUniformIndexing,
    &Features::descriptorBindingSampledImageUpdateAfterBind, &Features::descriptorBindingStorageBufferUpdateAfterBind,
    &Features::descriptorBindingUpdateUnusedWhilePending, &Features::descriptorBindingPartiallyBound,
    &Features::runtimeDescriptorArray
  };
  for (auto member : members) {
    Features unavailable = p;
    unavailable.*member = false;
    missing.clear();
    if (nr::CheckNativeBindlessFeatures(unavailable, report) || missing.size() != 1) return 2;
  }
  missing.clear();
  Features unavailable{false,false,false,false,false,false,false,false,false};
  if (nr::CheckNativeBindlessFeatures(unavailable, report) || missing.size() != 9) return 3;
  Limits limits;
  auto limit_report = [&](const char* name, unsigned, unsigned) { missing.emplace_back(name); };
  missing.clear();
  if (!nr::CheckNativeBindlessLimits(7, limits, limit_report) || !missing.empty()) return 4;
  unsigned Limits::* limit_members[] = {
    &Limits::maxPerStageDescriptorUpdateAfterBindSampledImages, &Limits::maxPerStageDescriptorUpdateAfterBindStorageBuffers,
    &Limits::maxPerStageDescriptorUpdateAfterBindSamplers, &Limits::maxDescriptorSetUpdateAfterBindSampledImages,
    &Limits::maxDescriptorSetUpdateAfterBindStorageBuffers, &Limits::maxDescriptorSetUpdateAfterBindSamplers,
    &Limits::maxPerStageUpdateAfterBindResources, &Limits::maxUpdateAfterBindDescriptorsInAllPools
  };
  for (auto member : limit_members) {
    Limits too_small = limits;
    --(too_small.*member);
    missing.clear();
    if (nr::CheckNativeBindlessLimits(7, too_small, limit_report) || missing.size() != 1) return 5;
  }
  missing.clear();
  if (nr::CheckNativeBindlessLimits(6, limits, limit_report) || missing.size() != 1) return 6;
  std::puts("Native Vulkan requirements: 21 cases passed");
}
