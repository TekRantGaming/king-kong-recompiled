// Intel XeSS Super Resolution (upscaler.h). Built when NR_XESS_SDK_DIR names the XeSS SDK.

#include "backend/upscaler.h"
#if NR_HAS_VULKAN
#include "backend/upscaler_vulkan.h"
#endif

namespace nr {

#if 1  // until the XeSS stage
std::unique_ptr<Upscaler> CreateXessUpscaler(nvrhi::IDevice*, const UpscaleConfig&, std::string* why) {
  if (why) *why = "not built in (the XeSS SDK was not found at build time: NR_XESS_SDK_DIR)";
  return nullptr;
}
#if NR_HAS_VULKAN
std::vector<std::string> XessVulkanInstanceExtensions() { return {}; }
UpscalerVulkanNeeds XessVulkanDeviceNeeds(VkInstance, VkPhysicalDevice) { return {}; }
#endif
#endif

}  // namespace nr
