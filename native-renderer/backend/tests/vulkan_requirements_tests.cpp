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
  std::puts("Native Vulkan requirements: 11 cases passed");
}
