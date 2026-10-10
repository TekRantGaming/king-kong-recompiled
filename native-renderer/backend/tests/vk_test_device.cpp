#include "backend/tests/vk_test_device.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

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
  vk::Instance instance;
  vk::DebugUtilsMessengerEXT messenger;
  vk::PhysicalDevice physical_device;
  vk::Device device;
  vk::Queue queue;
  uint32_t queue_family = 0;
  std::vector<std::string> instance_extensions, device_extensions;
};

std::unique_ptr<VulkanTestDevice> VulkanTestDevice::Create(bool validation) {
  std::unique_ptr<VulkanTestDevice> d(new VulkanTestDevice());
  try {
    if (!d->Init(validation)) return nullptr;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Vulkan test device: %s\n", e.what());
    return nullptr;
  }
  return d;
}

bool VulkanTestDevice::Init(bool validation) {
  impl_ = std::make_unique<Impl>();
  auto gipa = impl_->loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
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
  float priority = 1.0f;
  vk::DeviceQueueCreateInfo qci({}, impl_->queue_family, 1, &priority);
  vk::DeviceCreateInfo dci({}, qci, {}, {});
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
  if (impl_) {
    if (impl_->device) impl_->device.destroy();
    if (impl_->messenger) impl_->instance.destroyDebugUtilsMessengerEXT(impl_->messenger);
    if (impl_->instance) impl_->instance.destroy();
  }
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

VulkanTestDevice::Pixels VulkanTestDevice::ReadBack(nvrhi::ITexture* texture) {
  Pixels out;
  const nvrhi::TextureDesc& td = texture->getDesc();
  nvrhi::TextureDesc sd;
  sd.width = td.width;
  sd.height = td.height;
  sd.format = td.format;
  sd.debugName = "Readback";
  nvrhi::StagingTextureHandle staging =
      nvrhi_device_->createStagingTexture(sd, nvrhi::CpuAccessMode::Read);
  nvrhi::CommandListHandle cl = nvrhi_device_->createCommandList();
  cl->open();
  cl->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
  cl->close();
  Execute(cl);
  WaitIdle();
  size_t pitch = 0;
  auto* data = static_cast<const uint8_t*>(
      nvrhi_device_->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read,
                                       &pitch));
  if (!data) return out;
  out.width = td.width;
  out.height = td.height;
  out.rgba.resize(size_t(td.width) * td.height * 4);
  for (uint32_t y = 0; y < td.height; ++y) {
    for (uint32_t x = 0; x < td.width; ++x) {
      uint32_t v;
      std::memcpy(&v, data + y * pitch + x * 4, 4);
      float* p = &out.rgba[(size_t(y) * td.width + x) * 4];
      if (td.format == nvrhi::Format::R10G10B10A2_UNORM) {
        p[0] = float(v & 1023) / 1023.0f;
        p[1] = float((v >> 10) & 1023) / 1023.0f;
        p[2] = float((v >> 20) & 1023) / 1023.0f;
        p[3] = float(v >> 30) / 3.0f;
      } else {
        for (int c = 0; c < 4; ++c) p[c] = float((v >> (8 * c)) & 255) / 255.0f;
      }
    }
  }
  nvrhi_device_->unmapStagingTexture(staging);
  return out;
}

bool WritePpm(const std::string& path, const VulkanTestDevice::Pixels& pixels) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << "P6\n" << pixels.width << " " << pixels.height << "\n255\n";
  for (size_t i = 0; i < size_t(pixels.width) * pixels.height; ++i) {
    for (int c = 0; c < 3; ++c) {
      float v = pixels.rgba[i * 4 + c];
      f.put(char(uint8_t(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f)));
    }
  }
  return bool(f);
}

}  // namespace nr::test
