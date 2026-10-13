// What the upscalers need from a Vulkan instance and device (upscaler_vulkan.h).

#include "backend/upscaler_vulkan.h"

#if NR_HAS_VULKAN

#include <algorithm>
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

PFN_vkGetInstanceProcAddr LoaderGipa(PFN_vkGetInstanceProcAddr gipa) {
  if (gipa) return gipa;
  if ((gipa = VulkanGetInstanceProcAddr())) return gipa;
#if defined(_WIN32)
  if (HMODULE m = GetModuleHandleW(L"vulkan-1.dll")) {
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(m, "vkGetInstanceProcAddr"));
  }
#else
  if (void* m = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD)) {
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m, "vkGetInstanceProcAddr"));
  }
#endif
  return nullptr;
}

void AddUnique(std::vector<std::string>& to, const std::vector<std::string>& from) {
  for (const std::string& s : from) {
    if (!s.empty() && std::find(to.begin(), to.end(), s) == to.end()) to.push_back(s);
  }
}

}  // namespace

void UpscalerVulkanNeeds::Add(const UpscalerVulkanNeeds& other) {
  AddUnique(extensions, other.extensions);
  features.insert(features.end(), other.features.begin(), other.features.end());
}

void UpscalerVulkanWants(const UpscaleConfig& config, uint32_t vendor, bool* dlss, bool* xess) {
  *dlss = *xess = false;
  if (!config.enabled()) return;
  switch (config.kind) {
    case UpscalerKind::kDlss: *dlss = true; break;
    case UpscalerKind::kXess: *xess = true; break;
    case UpscalerKind::kAuto:
      // Before the device is chosen (vendor 0) both are asked for; the instance extensions are few
      // and harmless.
      *dlss = vendor == 0 || vendor == 0x10DE;
      *xess = vendor == 0 || vendor == 0x8086;
      break;
    default: break;
  }
}

std::vector<std::string> UpscalerVulkanInstanceExtensions(bool dlss, bool xess) {
  std::vector<std::string> out;
  if (dlss) AddUnique(out, DlssVulkanInstanceExtensions());
  if (xess) AddUnique(out, XessVulkanInstanceExtensions());
  return out;
}

UpscalerVulkanNeeds UpscalerVulkanDeviceNeeds(bool dlss, bool xess, VkInstance instance, VkPhysicalDevice physical,
                                             PFN_vkGetInstanceProcAddr gipa) {
  UpscalerVulkanNeeds out;
  if (!instance || !physical) return out;
  (void)gipa;
  if (dlss) out.Add(DlssVulkanDeviceNeeds(instance, physical));
  if (xess) out.Add(XessVulkanDeviceNeeds(instance, physical));
  return out;
}

uint32_t UpscalerVulkanVendor(VkInstance instance, VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr gipa) {
  gipa = LoaderGipa(gipa);
  if (!gipa || !instance || !physical) return 0;
  auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
  if (!props) return 0;
  VkPhysicalDeviceProperties p{};
  props(physical, &p);
  return p.vendorID;
}

}  // namespace nr

#endif  // NR_HAS_VULKAN
