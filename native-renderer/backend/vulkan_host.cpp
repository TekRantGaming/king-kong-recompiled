// NVRHI on the SDK's VulkanProvider device and graphics queue, and the
// hand-over of a frame to the VulkanPresenter's guest output image (drawn,
// not copied: the SDK makes that image without TRANSFER_DST usage).
//
// Built with the Linux SDK bundle (the Windows bundle has no Vulkan).
// NVRHI is a Vulkan 1.3 library: on a device it did not create it needs
// timelineSemaphore, dynamicRendering and synchronization2 enabled. The SDK's
// VulkanDevice (v0.10.0) enables dynamicRendering only, so the SDK patch in
// native-renderer/sdk-patches is required; without it this logs what is
// missing and fails, and the plugin falls back to no renderer.

#include <array>
#include <chrono>
#include <mutex>
#include <vector>

// First: the SDK fixes the Vulkan macros (beta extensions, platforms) every
// includer must agree on.
#include <rex/ui/vulkan/api.h>

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

#include "backend/host_device.h"
#include "backend/cpu_profile.h"
#include "backend/output_blit.h"
#include "backend/vk_pipeline_cache.h"
#include "backend/vulkan_dispatch.h"
#include "backend/vulkan_requirements.h"

REXCVAR_DECLARE(bool, native_validation);
REXCVAR_DECLARE(bool, native_vk_pipeline_cache);
REXCVAR_DECLARE(int32_t, native_debug);

namespace nr {

namespace {

class VulkanHost final : public HostDevice {
 public:
  explicit VulkanHost(rex::ui::vulkan::VulkanProvider& provider) : provider_(provider) {}

  bool Initialize() {
    const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_.vulkan_device();
    const rex::ui::vulkan::VulkanInstance* vulkan_instance = provider_.vulkan_instance();
    if (!vulkan_device || !vulkan_instance) {
      return false;
    }
    if (!HasRequiredFeatures(*vulkan_device)) {
      return false;
    }
    VkPhysicalDeviceDescriptorIndexingProperties indexing{};
    indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &indexing;
    const auto get_properties = vulkan_instance->functions().vkGetPhysicalDeviceProperties2;
    if (!get_properties) {
      REXGPU_ERROR("rexgpu-native: cannot verify descriptor limits. Choose Native D3D12 or Classic.");
      return false;
    }
    get_properties(vulkan_device->physical_device(), &properties);
    if (!CheckNativeBindlessLimits(properties.properties.limits.maxBoundDescriptorSets, indexing,
          [](const char* name, unsigned available, unsigned needed) {
            REXGPU_ERROR("rexgpu-native: descriptor limit {} is {}, requires {}. Choose Native D3D12 or Classic.",
                         name, available, needed);
          })) return false;
    queue_family_ = vulkan_device->queue_family_graphics_compute();
    // The SDK loads the Vulkan loader itself; NVRHI's calls go through
    // vulkan.hpp's dispatcher, filled from the same loader.
    InitVulkanDispatch(vulkan_instance->functions().vkGetInstanceProcAddr,
                       vulkan_instance->instance(), vulkan_device->device());
    nvrhi::vulkan::DeviceDesc desc;
    if (REXCVAR_GET(native_debug) & 524288) desc.maxTimerQueries = 2048;
    desc.errorCB = GetMessageCallback();
    desc.instance = vulkan_instance->instance();
    desc.physicalDevice = vulkan_device->physical_device();
    desc.device = vulkan_device->device();
    {
      auto acquisition = vulkan_device->AcquireQueue(queue_family_, 0);
      desc.graphicsQueue = acquisition.queue();
    }
    desc.graphicsQueueIndex = int(queue_family_);
    desc.transferQueue = nullptr;
    desc.computeQueue = nullptr;
    if (REXCVAR_GET(native_vk_pipeline_cache)) {
      // Ours, so it can be filled from a file and saved (the game renderer does both).
      pipeline_cache_ = VulkanPipelineCache::Create(vulkan_device->physical_device(), vulkan_device->device());
      if (pipeline_cache_) {
        desc.pipelineCache = pipeline_cache_->handle();
      } else {
        REXGPU_WARN("rexgpu-native: unable to create the Vulkan pipeline cache; pipelines are not kept");
      }
    }
    device_ = nvrhi::vulkan::createDevice(desc);
    if (!device_) {
      return false;
    }
    if (REXCVAR_GET(native_validation)) {
      device_ = nvrhi::validation::createValidationLayer(device_);
    }
    output_blit_ = std::make_unique<OutputBlit>(device_);
    return true;
  }

  nvrhi::IDevice* device() override { return device_; }
  nvrhi::GraphicsAPI api() const override { return nvrhi::GraphicsAPI::VULKAN; }

  void ExecuteCommandList(nvrhi::ICommandList* command_list) override {
    ExecuteFrameCommandList(command_list, nullptr);
  }
  void WaitFrame(nvrhi::IEventQuery* completion) override {
    // Waiting on a timeline semaphore does not access VkQueue. Avoid the SDK's
    // queue lock here: its presenter may hold that lock while waiting for display.
    // NVRHI's completion bookkeeping still needs serialization with submission/GC.
    std::lock_guard lock(execute_mutex_);
    device_->waitEventQuery(completion);
  }
  void ExecuteFrameCommandList(nvrhi::ICommandList* command_list, nvrhi::IEventQuery* completion) override {
    // Host access to a VkQueue must be externally synchronised: hold the SDK's
    // queue lock (recursive) while NVRHI submits.
    using clock = std::chrono::steady_clock;
    const uint64_t cpu_start = CurrentThreadCpuMicroseconds();
    const auto t0 = clock::now();
    auto acquisition = provider_.vulkan_device()->AcquireQueue(queue_family_, 0);
    std::lock_guard execute_lock(execute_mutex_);
    const auto t1 = clock::now();
    device_->executeCommandList(command_list);
    if (completion) {
      device_->resetEventQuery(completion);
      device_->setEventQuery(completion, nvrhi::CommandQueue::Graphics);
    }
    const auto t2 = clock::now();
    device_->runGarbageCollection();
    const auto t3 = clock::now();
    auto us = [](clock::time_point a, clock::time_point b) {
      return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };
    std::lock_guard lock(times_mutex_);
    times_.wait_us += us(t0, t1);
    times_.execute_us += us(t1, t2);
    times_.garbage_us += us(t2, t3);
    times_.cpu_us += CurrentThreadCpuMicroseconds() - cpu_start;
    ++times_.count;
  }
  SubmitTimes submit_times() const override {
    std::lock_guard lock(times_mutex_);
    return times_;
  }
  DriverPipelineCache* pipeline_cache() override { return pipeline_cache_.get(); }

  void WaitForIdle() override {
    auto acquisition = provider_.vulkan_device()->AcquireQueue(queue_family_, 0);
    std::lock_guard execute_lock(execute_mutex_);
    device_->waitForIdle();
  }

  bool CopyToGuestOutput(rex::ui::Presenter::GuestOutputRefreshContext& context,
                         nvrhi::ICommandList* command_list, nvrhi::ITexture* source,
                         uint32_t width, uint32_t height) override {
    using rex::ui::vulkan::VulkanPresenter;
    auto& vulkan_context = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
    VkImage image = vulkan_context.image();
    nvrhi::ITexture* target = Wrap(image, vulkan_context.image_version(), width, height);
    if (!target) {
      return false;
    }
    const auto& fn = provider_.vulkan_device()->functions();
    auto native = static_cast<VkCommandBuffer>(
        command_list->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer);
    command_list->commitBarriers();

    // Acquire: from the presenter's internal layout (or undefined for a
    // never-written image) to colour attachment.
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    bool written = vulkan_context.image_ever_written_previously();
    barrier.srcAccessMask = written ? VulkanPresenter::kGuestOutputInternalAccessMask : 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout =
        written ? VulkanPresenter::kGuestOutputInternalLayout : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    fn.vkCmdPipelineBarrier(native,
                            written ? VkPipelineStageFlags(VulkanPresenter::kGuestOutputInternalStageMask)
                                    : VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &barrier);

    // The source's own barrier (to shader resource) comes with the draw's state. --native_debug bit
    // 1048576: the old copy instead (against the image's usage; to compare validation runs).
    bool ok = true;
    if (REXCVAR_GET(native_debug) & 1048576) {
      // NVRHI moves the wrapped image (tracked as a render target) to copy destination and back.
      command_list->copyTexture(target, nvrhi::TextureSlice(), source, nvrhi::TextureSlice());
      command_list->setTextureState(target, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
      command_list->commitBarriers();
    } else {
      ok = output_blit_->Record(command_list, source, target, width, height);
      command_list->commitBarriers();
    }

    // Release: back to the presenter's internal layout.
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VulkanPresenter::kGuestOutputInternalLayout;
    fn.vkCmdPipelineBarrier(native, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
                            nullptr, 1, &barrier);
    context.SetIs8bpc(true);
    return ok;
  }

 private:
  // NVRHI needs Vulkan 1.3 with dynamicRendering, synchronization2 and
  // timelineSemaphore enabled on the device. The SDK reports what it enabled
  // in its Properties; the last two only with sdk-patches/0001 applied
  // (REX_UI_VULKAN_DEVICE_HAS_SYNC_FEATURES).
  static bool HasRequiredFeatures(const rex::ui::vulkan::VulkanDevice& device) {
    const auto& props = device.properties();
    if (props.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
      REXGPU_ERROR("rexgpu-native: Vulkan 1.3 is required (the device has {}.{})",
                   VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
      return false;
    }
    bool ok = true;
    if (!props.dynamicRendering) {
      REXGPU_ERROR("rexgpu-native: the Vulkan device was created without dynamicRendering");
      ok = false;
    }
#if defined(REX_UI_VULKAN_DEVICE_HAS_SYNC_FEATURES)
    if (!props.timelineSemaphore || !props.synchronization2) {
      REXGPU_ERROR("rexgpu-native: the Vulkan device lacks timelineSemaphore or synchronization2");
      ok = false;
    }
#else
    REXGPU_ERROR("rexgpu-native: SDK synchronization features cannot be verified; apply sdk-patches/0001");
    ok = false;
#endif
// Validate what was actually enabled on this shared SDK device before creating
// any bindless layouts, instead of relying on a late driver error or missing draws.
#if defined(REX_UI_VULKAN_DEVICE_HAS_DESCRIPTOR_INDEXING)
    if (!CheckNativeBindlessFeatures(props, [](const char* name) {
          REXGPU_ERROR("rexgpu-native: Native Vulkan requires enabled feature {}", name);
        })) ok = false;
#else
    REXGPU_ERROR("rexgpu-native: SDK bindless features cannot be verified; apply sdk-patches/0002");
    ok = false;
#endif
    if (!ok) REXGPU_ERROR("rexgpu-native: Native Vulkan unsupported on this device. Choose Native D3D12 or Classic in the launcher.");
    return ok;
  }

  struct Wrapped {
    VkImage image = VK_NULL_HANDLE;
    uint64_t version = UINT64_MAX;
    uint32_t width = 0;
    uint32_t height = 0;
    nvrhi::TextureHandle texture;
  };

  nvrhi::ITexture* Wrap(VkImage image, uint64_t version, uint32_t width, uint32_t height) {
    for (Wrapped& w : wrapped_) {
      if (w.image == image && w.version == version && w.width == width && w.height == height) {
        return w.texture;
      }
    }
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = nvrhi::Format::R10G10B10A2_UNORM;
    desc.isRenderTarget = true;
    // Only ever drawn into; the layouts around the draw are set by hand.
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.keepInitialState = true;
    desc.debugName = "Presenter guest output";
    nvrhi::TextureHandle texture = device_->createHandleForNativeTexture(
        nvrhi::ObjectTypes::VK_Image, nvrhi::Object(image), desc);
    if (!texture) {
      REXGPU_ERROR("rexgpu-native: unable to wrap the presenter's guest output image");
      return nullptr;
    }
    Wrapped& slot = wrapped_[next_slot_];
    next_slot_ = (next_slot_ + 1) % wrapped_.size();
    if (slot.texture) output_blit_->Forget(slot.texture);
    REXLOG_INFO("rexgpu-native: guest output image {:#x} ({}x{}, version {}) wrapped", uint64_t(image), width, height,
                 version);
    slot = Wrapped{image, version, width, height, texture};
    return slot.texture;
  }

  rex::ui::vulkan::VulkanProvider& provider_;
  mutable std::mutex times_mutex_;
  std::mutex execute_mutex_;
  SubmitTimes times_;
  uint32_t queue_family_ = 0;
  // Before device_: destroyed after it (NVRHI's device only borrows the cache).
  std::unique_ptr<VulkanPipelineCache> pipeline_cache_;
  nvrhi::DeviceHandle device_;
  std::unique_ptr<OutputBlit> output_blit_;
  std::array<Wrapped, 4> wrapped_;
  size_t next_slot_ = 0;
};

}  // namespace

std::unique_ptr<HostDevice> CreateVulkanHostDevice(rex::ui::GraphicsProvider& provider) {
  auto host = std::make_unique<VulkanHost>(static_cast<rex::ui::vulkan::VulkanProvider&>(provider));
  if (!host->Initialize()) {
    return nullptr;
  }
  return host;
}

}  // namespace nr
