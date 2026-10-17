// Vulkan graphics pipeline libraries: device support and requests (vk_pipeline_library.h).

#include "backend/vk_pipeline_library.h"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "backend/vulkan_dispatch.h"

namespace nr {

namespace {

PFN_vkGetInstanceProcAddr FindGipa(PFN_vkGetInstanceProcAddr gipa) {
  if (gipa) return gipa;
  if ((gipa = VulkanGetInstanceProcAddr())) return gipa;
#if defined(_WIN32)
  if (HMODULE m = GetModuleHandleW(L"vulkan-1.dll"))
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(m, "vkGetInstanceProcAddr"));
#else
  if (void* m = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD))
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m, "vkGetInstanceProcAddr"));
#endif
  return nullptr;
}

}  // namespace

std::string VulkanPipelineLibrarySupport::reason() const {
  if (!api_1_3) return "the device is not Vulkan 1.3";
  if (!extension) return "the driver has no VK_EXT_graphics_pipeline_library";
  if (!feature) return "the driver offers VK_EXT_graphics_pipeline_library without graphicsPipelineLibrary";
  if (!fast_linking) return "the driver's library links are not fast (graphicsPipelineLibraryFastLinking)";
  if (!vendor_tested) return "only tested on NVIDIA so far (KK_GPL_ANY_VENDOR=1 turns it on here)";
  return "";
}

bool VulkanPipelineLibraryAllowedByEnvironment(std::string* why) {
  const char* v = std::getenv("KK_DEV_NO_GPL");
  if (v && *v && std::strcmp(v, "0") != 0) {
    if (why) *why = "KK_DEV_NO_GPL is set";
    return false;
  }
  return true;
}

VulkanPipelineLibrarySupport QueryVulkanPipelineLibrary(VkInstance instance, VkPhysicalDevice physical,
                                                        PFN_vkGetInstanceProcAddr gipa) {
  VulkanPipelineLibrarySupport s;
  gipa = FindGipa(gipa);
  if (!gipa || !instance || !physical) return s;
  auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
      gipa(instance, "vkEnumerateDeviceExtensionProperties"));
  auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2"));
  auto properties2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
  if (!enumerate || !features2 || !properties2) return s;
  uint32_t count = 0;
  if (enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS) return s;
  std::vector<VkExtensionProperties> extensions(count);
  if (enumerate(physical, nullptr, &count, extensions.data()) != VK_SUCCESS) return s;
  bool gpl = false, library = false;
  for (const VkExtensionProperties& e : extensions) {
    gpl = gpl || std::strcmp(e.extensionName, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) == 0;
    library = library || std::strcmp(e.extensionName, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) == 0;
  }
  s.extension = gpl && library;
  VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_properties{};
  gpl_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  if (s.extension) properties.pNext = &gpl_properties;
  properties2(physical, &properties);
  s.api_1_3 = properties.properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0);
  {
    const char* any = std::getenv("KK_GPL_ANY_VENDOR");
    s.vendor_tested = properties.properties.vendorID == 0x10DE || (any && *any && std::strcmp(any, "0") != 0);
  }
  if (!s.extension) return s;
  s.fast_linking = gpl_properties.graphicsPipelineLibraryFastLinking == VK_TRUE;
  VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl_features{};
  gpl_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
  VkPhysicalDeviceFeatures2 features{};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext = &gpl_features;
  features2(physical, &features);
  s.feature = gpl_features.graphicsPipelineLibrary == VK_TRUE;
  return s;
}

void AddVulkanPipelineLibraryRequests(VkInstance instance, VkPhysicalDevice physical,
                                      std::vector<std::string>& extensions,
                                      std::vector<std::vector<uint8_t>>& features) {
  if (!VulkanPipelineLibraryAllowedByEnvironment()) return;
  if (!QueryVulkanPipelineLibrary(instance, physical).usable()) return;
  for (const char* name : {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME}) {
    bool have = false;
    for (const std::string& e : extensions) have = have || e == name;
    if (!have) extensions.emplace_back(name);
  }
  VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT request{};
  request.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
  request.graphicsPipelineLibrary = VK_TRUE;
  std::vector<uint8_t> bytes(sizeof(request));
  std::memcpy(bytes.data(), &request, sizeof(request));
  features.push_back(std::move(bytes));
}

}  // namespace nr
