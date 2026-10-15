// A host API device for the backend: NVRHI created on the SDK provider's
// existing device and queue, plus the two things NVRHI cannot do by itself:
// submit under the SDK's queue lock (Vulkan) and hand a frame to the SDK
// presenter's guest output image with the barriers the presenter expects.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <nvrhi/nvrhi.h>
#include <rex/ui/presenter.h>
#include "backend/vulkan_requirements.h"

namespace rex::ui {
class GraphicsProvider;
}

namespace nr {

class DriverPipelineCache;

class HostDevice {
 public:
  virtual ~HostDevice() = default;

  virtual nvrhi::IDevice* device() = 0;
  virtual nvrhi::GraphicsAPI api() const = 0;
  virtual bool tessellation_supported() const { return api() == nvrhi::GraphicsAPI::D3D12; }
  virtual NativeTableCapacities table_capacities() const { return kNativeTableCapacities; }

  // Submits a closed command list (serialised against the SDK's own use of
  // the queue where that matters).
  virtual void ExecuteCommandList(nvrhi::ICommandList* command_list) = 0;
  virtual void ExecuteFrameCommandList(nvrhi::ICommandList* command_list, nvrhi::IEventQuery* completion) {
    ExecuteCommandList(command_list);
    device()->resetEventQuery(completion);
    device()->setEventQuery(completion, nvrhi::CommandQueue::Graphics);
  }
  virtual void WaitFrame(nvrhi::IEventQuery* completion) { device()->waitEventQuery(completion); }
  virtual void WaitForIdle() = 0;
  // Runs `fn` with the GPU idle and the queue locked against the other threads (an SDK's set-up).
  virtual void RunExclusive(const std::function<void()>& fn) {
    WaitForIdle();
    fn();
  }
  // Microseconds spent in ExecuteCommandList: waiting for the queue, submitting, collecting garbage.
  struct SubmitTimes {
    uint64_t wait_us = 0, execute_us = 0, garbage_us = 0, count = 0;
    uint64_t cpu_us = 0;
  };
  virtual SubmitTimes submit_times() const { return {}; }
  // The cache the device creates every pipeline with, when the host keeps one in a file
  // (--native_vk_pipeline_cache, Vulkan); null otherwise.
  virtual DriverPipelineCache* pipeline_cache() { return nullptr; }

  // Records, into the open `command_list`, a copy of `source` (width x
  // height, R10G10B10A2_UNORM) into the presenter's guest output image behind
  // `context`, leaving the image in the state the presenter requires.
  virtual bool CopyToGuestOutput(rex::ui::Presenter::GuestOutputRefreshContext& context,
                                 nvrhi::ICommandList* command_list, nvrhi::ITexture* source,
                                 uint32_t width, uint32_t height) = 0;
};

#if NR_HAS_D3D12
std::unique_ptr<HostDevice> CreateD3D12HostDevice(rex::ui::GraphicsProvider& provider);
#endif
#if NR_HAS_VULKAN
std::unique_ptr<HostDevice> CreateVulkanHostDevice(rex::ui::GraphicsProvider& provider);
#endif

// Logs NVRHI's messages through the runtime's GPU log category.
nvrhi::IMessageCallback* GetMessageCallback();

}  // namespace nr
