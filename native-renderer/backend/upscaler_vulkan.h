// What the upscalers need from a Vulkan instance and device they did not create (docs/upscalers.md,
// "Vulkan"). DLSS and XeSS work on the game's own device, so its instance and device must be created
// with their extensions (VK_NVX_binary_import, VK_EXT_mutable_descriptor_type, ...) and features
// (shaderFloat16, ...). The plugin asks before the SDK's VulkanProvider makes them (sdk-patches/0003),
// the test device before it makes its own. FSR 3.1 needs nothing beyond what the renderer already
// enables.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "backend/upscale_config.h"

namespace nr {

struct UpscalerVulkanNeeds {
  std::vector<std::string> extensions;           // device extensions, enabled when supported
  std::vector<std::vector<uint8_t>> features;    // Vk*Features structures (sType first, pNext ignored);
                                                 // VK_TRUE members are enabled when supported
  void Add(const UpscalerVulkanNeeds& other);
};

// Which SDKs to prepare the device for, for `config` on a GPU of PCI vendor `vendor` (0 unknown: before
// the device is chosen). --native_upscaler=off: none, so nothing changes.
void UpscalerVulkanWants(const UpscaleConfig& config, uint32_t vendor, bool* dlss, bool* xess);

// Instance extensions DLSS and XeSS ask for (empty when not built in or their DLL is missing).
std::vector<std::string> UpscalerVulkanInstanceExtensions(bool dlss, bool xess);
// Device extensions and features for the chosen physical device. `gipa` may be null (the loader's).
UpscalerVulkanNeeds UpscalerVulkanDeviceNeeds(bool dlss, bool xess, VkInstance instance, VkPhysicalDevice physical,
                                             PFN_vkGetInstanceProcAddr gipa);
// The PCI vendor of `physical`.
uint32_t UpscalerVulkanVendor(VkInstance instance, VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr gipa);

// Each SDK's part (upscaler_dlss.cpp, upscaler_xess.cpp).
std::vector<std::string> DlssVulkanInstanceExtensions();
UpscalerVulkanNeeds DlssVulkanDeviceNeeds(VkInstance instance, VkPhysicalDevice physical);
std::vector<std::string> XessVulkanInstanceExtensions();
UpscalerVulkanNeeds XessVulkanDeviceNeeds(VkInstance instance, VkPhysicalDevice physical);

}  // namespace nr
