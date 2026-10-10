// NVRHI on the SDK's D3D12Provider device and direct queue, and the hand-over
// of a frame to the D3D12Presenter's guest output resource.

#include <array>
#include <utility>

#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>

#include "backend/host_device.h"

REXCVAR_DECLARE(bool, native_validation);

namespace nr {

namespace {

class D3D12Host final : public HostDevice {
 public:
  explicit D3D12Host(rex::ui::d3d12::D3D12Provider& provider) : provider_(provider) {}

  bool Initialize() {
    nvrhi::d3d12::DeviceDesc desc;
    desc.errorCB = GetMessageCallback();
    desc.pDevice = provider_.GetDevice();
    desc.pGraphicsCommandQueue = provider_.GetDirectQueue();
    // The game renderer's bindless tables (backend/game_renderer.cpp): about
    // 35,000 views and 1,024 samplers; a shader-visible sampler heap holds at
    // most 2,048.
    desc.shaderResourceViewHeapSize = 65536;
    desc.samplerHeapSize = 2048;
    device_ = nvrhi::d3d12::createDevice(desc);
    if (!device_) {
      return false;
    }
    if (REXCVAR_GET(native_validation)) {
      device_ = nvrhi::validation::createValidationLayer(device_);
    }
    return true;
  }

  nvrhi::IDevice* device() override { return device_; }
  nvrhi::GraphicsAPI api() const override { return nvrhi::GraphicsAPI::D3D12; }

  void ExecuteCommandList(nvrhi::ICommandList* command_list) override {
    // ID3D12CommandQueue is thread safe; the presenter submits to the same
    // queue without a lock.
    device_->executeCommandList(command_list);
    device_->runGarbageCollection();
  }

  void WaitForIdle() override { device_->waitForIdle(); }

  bool CopyToGuestOutput(rex::ui::Presenter::GuestOutputRefreshContext& context,
                         nvrhi::ICommandList* command_list, nvrhi::ITexture* source,
                         uint32_t width, uint32_t height) override {
    auto& d3d12_context =
        static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
    ID3D12Resource* resource = d3d12_context.resource_uav_capable();
    nvrhi::ITexture* target = Wrap(resource, width, height);
    if (!target) {
      return false;
    }
    // The presenter keeps the image in PIXEL_SHADER_RESOURCE. NVRHI tracks the
    // wrapped texture as permanently in CopyDest (keepInitialState), so the
    // two transitions are issued by hand on the native command list.
    auto* native = static_cast<ID3D12GraphicsCommandList*>(
        command_list->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
    command_list->commitBarriers();
    Transition(native, resource, rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
               D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->copyTexture(target, nvrhi::TextureSlice(), source, nvrhi::TextureSlice());
    command_list->commitBarriers();
    Transition(native, resource, D3D12_RESOURCE_STATE_COPY_DEST,
               rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
    context.SetIs8bpc(true);
    return true;
  }

 private:
  struct Wrapped {
    ID3D12Resource* resource = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    nvrhi::TextureHandle texture;
  };

  static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                         D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
  }

  // The presenter has a mailbox of three guest output resources, recreated
  // when the size changes. Keep an NVRHI handle per live resource.
  nvrhi::ITexture* Wrap(ID3D12Resource* resource, uint32_t width, uint32_t height) {
    for (Wrapped& w : wrapped_) {
      if (w.resource == resource && w.width == width && w.height == height) {
        return w.texture;
      }
    }
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = nvrhi::Format::R10G10B10A2_UNORM;
    desc.isRenderTarget = true;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    desc.debugName = "Presenter guest output";
    nvrhi::TextureHandle texture = device_->createHandleForNativeTexture(
        nvrhi::ObjectTypes::D3D12_Resource, nvrhi::Object(resource), desc);
    if (!texture) {
      REXGPU_ERROR("rexgpu-native: unable to wrap the presenter's guest output resource");
      return nullptr;
    }
    // Replace the oldest slot.
    Wrapped& slot = wrapped_[next_slot_];
    next_slot_ = (next_slot_ + 1) % wrapped_.size();
    slot = Wrapped{resource, width, height, texture};
    return slot.texture;
  }

  rex::ui::d3d12::D3D12Provider& provider_;
  nvrhi::DeviceHandle device_;
  std::array<Wrapped, 4> wrapped_;
  size_t next_slot_ = 0;
};

}  // namespace

std::unique_ptr<HostDevice> CreateD3D12HostDevice(rex::ui::GraphicsProvider& provider) {
  auto host = std::make_unique<D3D12Host>(static_cast<rex::ui::d3d12::D3D12Provider&>(provider));
  if (!host->Initialize()) {
    return nullptr;
  }
  return host;
}

}  // namespace nr
