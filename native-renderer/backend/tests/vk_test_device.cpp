#include "backend/tests/vk_test_device.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

#include "backend/upscaler_vulkan.h"
#include "backend/vk_pipeline_cache.h"
#include "backend/vulkan_dispatch.h"

namespace nr::test {

namespace {

int g_vulkan_errors = 0;
int g_nvrhi_errors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* /*user*/) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    ++g_vulkan_errors;
    std::fprintf(stderr, "  [vulkan validation] %s\n", data->pMessage);
  }
  return VK_FALSE;
}

class MessageCallback final : public nvrhi::IMessageCallback {
 public:
  void message(nvrhi::MessageSeverity severity, const char* text) override {
    if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal) {
      ++g_nvrhi_errors;
      std::fprintf(stderr, "  [nvrhi] %s\n", text);
    } else if (severity == nvrhi::MessageSeverity::Warning) {
      std::fprintf(stderr, "  [nvrhi warning] %s\n", text);
    }
  }
};
MessageCallback g_message_callback;

}  // namespace

struct VulkanTestDevice::Impl {
  vk::detail::DynamicLoader loader;
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  vk::Instance instance;
  vk::DebugUtilsMessengerEXT messenger;
  vk::PhysicalDevice physical_device;
  vk::Device device;
  vk::Queue queue;
  uint32_t queue_family = 0;
  std::vector<std::string> instance_extensions, device_extensions;
};

std::unique_ptr<VulkanTestDevice> VulkanTestDevice::Create(bool validation, bool driver_pipeline_cache,
                                                           bool upscaler_needs) {
  std::unique_ptr<VulkanTestDevice> d(new VulkanTestDevice());
  try {
    if (!d->Init(validation, driver_pipeline_cache, upscaler_needs)) return nullptr;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Vulkan test device: %s\n", e.what());
    return nullptr;
  }
  return d;
}

bool VulkanTestDevice::Init(bool validation, bool driver_pipeline_cache, bool upscaler_needs) {
  impl_ = std::make_unique<Impl>();
  auto gipa = impl_->loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
  impl_->gipa = gipa;
  if (!gipa) {
    std::fprintf(stderr, "Vulkan test device: no Vulkan loader (libvulkan.so.1)\n");
    return false;
  }
  InitVulkanDispatch(gipa, VK_NULL_HANDLE, VK_NULL_HANDLE);

  std::vector<const char*> layers;
  if (validation) {
    for (const auto& l : vk::enumerateInstanceLayerProperties()) {
      if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        khronos_validation_ = true;
      }
    }
  }
  std::vector<const char*> extensions;
  bool debug_utils = false;
  for (const auto& e : vk::enumerateInstanceExtensionProperties()) {
    if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) debug_utils = true;
  }
  if (debug_utils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  // What the upscalers ask for, when the implementation has it.
  std::vector<std::string> upscaler_instance_extensions;
  if (upscaler_needs) {
    const auto available = vk::enumerateInstanceExtensionProperties();
    for (const std::string& name : UpscalerVulkanInstanceExtensions(true, true)) {
      for (const auto& e : available) {
        if (name == e.extensionName.data() && name != VK_EXT_DEBUG_UTILS_EXTENSION_NAME) {
          upscaler_instance_extensions.push_back(name);
          break;
        }
      }
    }
  }
  for (const std::string& name : upscaler_instance_extensions) extensions.push_back(name.c_str());

  vk::ApplicationInfo app("nr_render_tests", 1, "kk-native-renderer", 1, VK_API_VERSION_1_3);
  vk::InstanceCreateInfo ici({}, &app, layers, extensions);
  impl_->instance = vk::createInstance(ici);
  InitVulkanDispatch(gipa, impl_->instance, VK_NULL_HANDLE);
  for (const char* e : extensions) impl_->instance_extensions.push_back(e);

  if (debug_utils) {
    vk::DebugUtilsMessengerCreateInfoEXT mci(
        {},
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning,
        vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
            vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
            vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(DebugCallback));
    impl_->messenger = impl_->instance.createDebugUtilsMessengerEXT(mci);
  }

  // Lavapipe first (the CPU driver: the same results on every machine), else
  // the first device with Vulkan 1.3.
  auto devices = impl_->instance.enumeratePhysicalDevices();
  vk::PhysicalDevice chosen;
  for (int pass = 0; pass < 2 && !chosen; ++pass) {
    for (auto& pd : devices) {
      auto props2 = pd.getProperties2<vk::PhysicalDeviceProperties2,
                                      vk::PhysicalDeviceVulkan12Properties>();
      const auto& props = props2.get<vk::PhysicalDeviceProperties2>().properties;
      if (props.apiVersion < VK_API_VERSION_1_3) continue;
      bool lavapipe = props2.get<vk::PhysicalDeviceVulkan12Properties>().driverID ==
                      vk::DriverId::eMesaLlvmpipe;
      if (pass == 0 && !lavapipe) continue;
      chosen = pd;
      device_name_ = props.deviceName.data();
      break;
    }
  }
  if (!chosen) {
    std::fprintf(stderr, "Vulkan test device: no Vulkan 1.3 device\n");
    return false;
  }
  impl_->physical_device = chosen;

  auto families = chosen.getQueueFamilyProperties();
  bool found = false;
  for (uint32_t i = 0; i < families.size(); ++i) {
    if (families[i].queueFlags & vk::QueueFlagBits::eGraphics) {
      impl_->queue_family = i;
      found = true;
      break;
    }
  }
  if (!found) return false;

  // What NVRHI (a Vulkan 1.3 library) needs from a device it did not create:
  // timeline semaphores for its queues, dynamic rendering for its render
  // passes and synchronization2 for its barriers. The SDK's VulkanDevice must
  // enable the same (sdk-patches/, see the stream's doc).
  auto supported = chosen.getFeatures2<vk::PhysicalDeviceFeatures2,
                                       vk::PhysicalDeviceVulkan12Features,
                                       vk::PhysicalDeviceVulkan13Features>();
  if (!supported.get<vk::PhysicalDeviceVulkan12Features>().timelineSemaphore ||
      !supported.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering ||
      !supported.get<vk::PhysicalDeviceVulkan13Features>().synchronization2) {
    std::fprintf(stderr, "Vulkan test device: %s lacks a Vulkan 1.3 feature NVRHI needs\n",
                 device_name_.c_str());
    return false;
  }
  vk::PhysicalDeviceVulkan13Features features13;
  features13.dynamicRendering = VK_TRUE;
  features13.synchronization2 = VK_TRUE;
  vk::PhysicalDeviceVulkan12Features features12;
  features12.timelineSemaphore = VK_TRUE;
  features12.pNext = &features13;
  vk::PhysicalDeviceFeatures2 features2;
  features2.pNext = &features12;
  // The game renderer's bindless tables (sdk-patches/0002 enables the same on
  // the SDK's device): every feature that is supported.
  const auto& s10 = supported.get<vk::PhysicalDeviceFeatures2>().features;
  const auto& s12 = supported.get<vk::PhysicalDeviceVulkan12Features>();
  // And the Vulkan 1.0 features the SDK enables for GPU emulation that the
  // game renderer relies on: clip distances in the translated vertex shaders,
  // a blend state per render target, depth clamping (no depth clip), wireframe,
  // anisotropy.
  features2.features.fullDrawIndexUint32 = s10.fullDrawIndexUint32;
  features2.features.tessellationShader = s10.tessellationShader;
  features2.features.independentBlend = s10.independentBlend;
  features2.features.depthClamp = s10.depthClamp;
  features2.features.fillModeNonSolid = s10.fillModeNonSolid;
  features2.features.samplerAnisotropy = s10.samplerAnisotropy;
  features2.features.shaderClipDistance = s10.shaderClipDistance;
  features2.features.shaderCullDistance = s10.shaderCullDistance;
  features2.features.shaderUniformBufferArrayDynamicIndexing = s10.shaderUniformBufferArrayDynamicIndexing;
  features2.features.shaderSampledImageArrayDynamicIndexing = s10.shaderSampledImageArrayDynamicIndexing;
  features2.features.shaderStorageBufferArrayDynamicIndexing = s10.shaderStorageBufferArrayDynamicIndexing;
  features12.descriptorIndexing = s12.descriptorIndexing;
  features12.shaderSampledImageArrayNonUniformIndexing = s12.shaderSampledImageArrayNonUniformIndexing;
  features12.shaderStorageBufferArrayNonUniformIndexing = s12.shaderStorageBufferArrayNonUniformIndexing;
  features12.descriptorBindingSampledImageUpdateAfterBind = s12.descriptorBindingSampledImageUpdateAfterBind;
  features12.descriptorBindingStorageBufferUpdateAfterBind = s12.descriptorBindingStorageBufferUpdateAfterBind;
  features12.descriptorBindingUpdateUnusedWhilePending = s12.descriptorBindingUpdateUnusedWhilePending;
  features12.descriptorBindingPartiallyBound = s12.descriptorBindingPartiallyBound;
  features12.descriptorBindingVariableDescriptorCount = s12.descriptorBindingVariableDescriptorCount;
  features12.runtimeDescriptorArray = s12.runtimeDescriptorArray;
  // What the upscalers ask for: the extensions the device has, the features it supports (merged into
  // the structures above, or chained on).
  std::vector<std::string> upscaler_device_extensions;
  std::vector<std::vector<uint8_t>> upscaler_features;
  if (upscaler_needs) {
    const UpscalerVulkanNeeds needs = UpscalerVulkanDeviceNeeds(true, true, impl_->instance, chosen, gipa);
    const auto available = chosen.enumerateDeviceExtensionProperties();
    for (const std::string& name : needs.extensions) {
      for (const auto& e : available) {
        if (name == e.extensionName.data()) {
          upscaler_device_extensions.push_back(name);
          break;
        }
      }
    }
    const size_t header = sizeof(VkBaseOutStructure);
    auto merge = [](uint8_t* enabled, const uint8_t* have, const uint8_t* requested, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        VkBool32 r, h;
        std::memcpy(&r, requested + i * 4, 4);
        std::memcpy(&h, have + i * 4, 4);
        if (r && h) std::memcpy(enabled + i * 4, &r, 4);
      }
    };
    const auto& s13 = supported.get<vk::PhysicalDeviceVulkan13Features>();
    auto get_features2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(impl_->instance, "vkGetPhysicalDeviceFeatures2"));
    void* chain_end = &features13;
    for (const auto& request : needs.features) {
      if (request.size() <= header) continue;
      VkStructureType type;
      std::memcpy(&type, request.data(), sizeof(type));
      const size_t count = (request.size() - header) / 4;
      if (type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
        merge(reinterpret_cast<uint8_t*>(&features2.features), reinterpret_cast<const uint8_t*>(&s10),
              request.data() + header, std::min(count, sizeof(VkPhysicalDeviceFeatures) / 4));
      } else if (type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
        merge(reinterpret_cast<uint8_t*>(&features12) + header, reinterpret_cast<const uint8_t*>(&s12) + header,
              request.data() + header, std::min(count, (sizeof(VkPhysicalDeviceVulkan12Features) - header) / 4));
      } else if (type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
        merge(reinterpret_cast<uint8_t*>(&features13) + header, reinterpret_cast<const uint8_t*>(&s13) + header,
              request.data() + header, std::min(count, (sizeof(VkPhysicalDeviceVulkan13Features) - header) / 4));
      } else if (get_features2) {
        std::vector<uint8_t> have(request.size() + 512, 0);
        std::memcpy(have.data(), &type, sizeof(type));
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = have.data();
        get_features2(chosen, &query);
        std::vector<uint8_t>& added = upscaler_features.emplace_back(request.size(), 0);
        std::memcpy(added.data(), &type, sizeof(type));
        merge(added.data() + header, have.data() + header, request.data() + header, count);
        reinterpret_cast<VkBaseOutStructure*>(chain_end)->pNext = reinterpret_cast<VkBaseOutStructure*>(added.data());
        chain_end = added.data();
      }
    }
    std::printf("  upscaler needs: %zu instance extensions, %zu of %zu device extensions, %zu feature structures\n",
                upscaler_instance_extensions.size(), upscaler_device_extensions.size(), needs.extensions.size(),
                needs.features.size());
    for (const auto& e : upscaler_instance_extensions) std::printf("    instance: %s\n", e.c_str());
    for (const auto& e : needs.extensions) std::printf("    device: %s\n", e.c_str());
    for (const auto& f : needs.features) {
      VkStructureType type;
      std::memcpy(&type, f.data(), sizeof(type));
      std::printf("    features: structure type %d (%zu bytes)\n", int(type), f.size());
    }
  }
  std::vector<const char*> device_extension_names;
  for (const std::string& name : upscaler_device_extensions) device_extension_names.push_back(name.c_str());
  float priority = 1.0f;
  vk::DeviceQueueCreateInfo qci({}, impl_->queue_family, 1, &priority);
  vk::DeviceCreateInfo dci({}, qci, {}, device_extension_names);
  dci.pNext = &features2;
  impl_->device = chosen.createDevice(dci);
  InitVulkanDispatch(gipa, impl_->instance, impl_->device);
  impl_->queue = impl_->device.getQueue(impl_->queue_family, 0);

  std::vector<const char*> instance_ext_ptrs, device_ext_ptrs;
  for (auto& s : impl_->instance_extensions) instance_ext_ptrs.push_back(s.c_str());
  nvrhi::vulkan::DeviceDesc desc;
  desc.errorCB = &g_message_callback;
  desc.instance = impl_->instance;
  desc.physicalDevice = impl_->physical_device;
  desc.device = impl_->device;
  desc.graphicsQueue = impl_->queue;
  desc.graphicsQueueIndex = int(impl_->queue_family);
  desc.instanceExtensions = instance_ext_ptrs.data();
  desc.numInstanceExtensions = instance_ext_ptrs.size();
  desc.deviceExtensions = device_ext_ptrs.data();
  desc.numDeviceExtensions = device_ext_ptrs.size();
  if (driver_pipeline_cache) {
    pipeline_cache_ = VulkanPipelineCache::Create(impl_->physical_device, impl_->device);
    if (!pipeline_cache_) return false;
    desc.pipelineCache = pipeline_cache_->handle();
  }
  nvrhi_device_ = nvrhi::vulkan::createDevice(desc);
  if (!nvrhi_device_) return false;
  if (validation) {
    nvrhi_device_ = nvrhi::validation::createValidationLayer(nvrhi_device_);
  }
  return true;
}

VulkanTestDevice::~VulkanTestDevice() {
  if (nvrhi_device_) {
    nvrhi_device_->waitForIdle();
    nvrhi_device_->runGarbageCollection();
  }
  nvrhi_device_ = nullptr;
  pipeline_cache_.reset();  // NVRHI's device only borrowed it
  if (impl_) {
    if (impl_->device) impl_->device.destroy();
    if (impl_->messenger) impl_->instance.destroyDebugUtilsMessengerEXT(impl_->messenger);
    if (impl_->instance) impl_->instance.destroy();
  }
}

void VulkanTestDevice::MakeCurrent() {
  if (impl_ && impl_->gipa) InitVulkanDispatch(impl_->gipa, impl_->instance, impl_->device);
}

int VulkanTestDevice::errors() const { return g_vulkan_errors + g_nvrhi_errors; }

void VulkanTestDevice::Execute(nvrhi::ICommandList* command_list) {
  nvrhi_device_->executeCommandList(command_list);
  nvrhi_device_->runGarbageCollection();
}

void VulkanTestDevice::WaitIdle() {
  nvrhi_device_->waitForIdle();
  nvrhi_device_->runGarbageCollection();
}

}  // namespace nr::test
