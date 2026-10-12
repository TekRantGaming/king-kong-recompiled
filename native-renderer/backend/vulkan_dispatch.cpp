#include "backend/vulkan_dispatch.h"

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace nr {

void InitVulkanDispatch(PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkInstance instance,
                        VkDevice device) {
  VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);
  if (instance) {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance));
  }
  if (device) {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device));
  }
}

PFN_vkGetInstanceProcAddr VulkanGetInstanceProcAddr() { return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr; }

PFN_vkGetDeviceProcAddr VulkanGetDeviceProcAddr() { return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr; }

}  // namespace nr
