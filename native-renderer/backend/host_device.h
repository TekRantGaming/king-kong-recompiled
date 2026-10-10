// A host API device for the backend: NVRHI created on the SDK provider's
// existing device and queue, plus the two things NVRHI cannot do by itself:
// submit under the SDK's queue lock (Vulkan) and hand a frame to the SDK
// presenter's guest output image with the barriers the presenter expects.
#pragma once

#include <cstdint>
#include <memory>

#include <nvrhi/nvrhi.h>
#include <rex/ui/presenter.h>

namespace rex::ui {
class GraphicsProvider;
}

namespace nr {

class HostDevice {
 public:
  virtual ~HostDevice() = default;

  virtual nvrhi::IDevice* device() = 0;
  virtual nvrhi::GraphicsAPI api() const = 0;

  // Submits a closed command list (serialised against the SDK's own use of
  // the queue where that matters).
  virtual void ExecuteCommandList(nvrhi::ICommandList* command_list) = 0;
  virtual void WaitForIdle() = 0;

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
