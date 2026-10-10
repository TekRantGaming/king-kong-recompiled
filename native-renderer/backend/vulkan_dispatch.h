// NVRHI's Vulkan backend calls Vulkan through vulkan.hpp's default dynamic
// dispatcher. With NVRHI built static, the dispatcher's storage lives in the
// program that links it (here: vulkan_dispatch.cpp), and the program must
// initialise it before nvrhi::vulkan::createDevice.
#pragma once

#include <vulkan/vulkan_core.h>

namespace nr {

void InitVulkanDispatch(PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkInstance instance,
                        VkDevice device);

}  // namespace nr
