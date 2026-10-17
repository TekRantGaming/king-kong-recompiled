// Vulkan graphics pipeline libraries (task 183; VK_EXT_graphics_pipeline_library, docs/backend.md "Pipeline
// libraries"): what the device is asked for and whether it can link pipelines from parts.
//
// With the library, a game pipeline the renderer has not made yet is linked from four parts the device makes once
// and keeps (vertex input, pre-rasterization shaders, fragment shader, fragment output: the vendored NVRHI,
// thirdparty/NOTES.md): microseconds when the parts exist, one shader compile when a part is new, instead of a whole
// pipeline compile. The whole (optimised) pipeline is then made in the background and replaces the linked one.
// Without it (an older driver, Mesa without the extension, KK_DEV_NO_GPL=1, --native_vk_pipeline_library=false)
// every pipeline is compiled whole as before.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace nr {

struct VulkanPipelineLibrarySupport {
  bool extension = false;     // VK_EXT_graphics_pipeline_library and VK_KHR_pipeline_library are offered
  bool feature = false;       // graphicsPipelineLibrary
  bool fast_linking = false;  // graphicsPipelineLibraryFastLinking: a link without optimisation is cheap
  bool api_1_3 = false;       // the linked pipelines' dynamic depth/stencil/cull state is core in 1.3
  bool usable() const { return extension && feature && fast_linking && api_1_3; }
  // Why not, for the log ("" when usable).
  std::string reason() const;
};

// False when the user or a dev switch turned it off (KK_DEV_NO_GPL=1); `why` says which.
bool VulkanPipelineLibraryAllowedByEnvironment(std::string* why = nullptr);

// What the physical device offers. `gipa` may be null (the loader's is found).
VulkanPipelineLibrarySupport QueryVulkanPipelineLibrary(VkInstance instance, VkPhysicalDevice physical,
                                                        PFN_vkGetInstanceProcAddr gipa = nullptr);

// The device extensions and feature structure (SDK device requests, sdk-patches/0003) the library needs; nothing
// when the device does not offer it.
void AddVulkanPipelineLibraryRequests(VkInstance instance, VkPhysicalDevice physical,
                                      std::vector<std::string>& extensions,
                                      std::vector<std::vector<uint8_t>>& features);

}  // namespace nr
