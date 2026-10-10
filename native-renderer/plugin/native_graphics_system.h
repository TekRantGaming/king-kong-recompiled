// The native renderer's IGraphicsSystem: what the runtime talks to.
//
// Compared with the Xenos plugin's GraphicsSystem this keeps the provider, the
// presenter, the GPU register MMIO range, the vblank worker and the interrupt
// dispatch, and replaces the command processor with the ring skimmer (the
// game's own Direct3D library still writes GPU packets; the skimmer consumes
// them so the library's fences and swaps complete, and nothing is drawn from
// them). Drawing happens in the backend, fed by the D3D hooks (native_api.h).
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <string_view>

#include <rex/system/interfaces/graphics.h>
#include <rex/system/xobject.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>

#include "plugin/native_api.h"

namespace rex::memory {
class Memory;
}
namespace rex::system {
class KernelState;
class XHostThread;
}
namespace rex::runtime {
class FunctionDispatcher;
}

namespace nr {

using rex::X_STATUS;

class Backend;
class RingSkimmer;

enum class HostApi { kD3D12, kVulkan };

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  // backend: "any", "d3d12" or "vulkan". Returns null when the host API is not
  // compiled in or not available.
  static NativeGraphicsSystem* Create(std::string_view backend);
  // The C interface for the hooks (plugin_main.cpp exports it).
  static const NrApi* GetApi();
  static NativeGraphicsSystem* Instance() { return instance_; }

  ~NativeGraphicsSystem() override;

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                         rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }
  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;
  void Shutdown() override;

  // For the ring skimmer and the vblank worker (both guest host-threads).
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);
  void OnSwap(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height);
  // The GPU's vblank / swap counter (EVENT_WRITE_SHD can write it to memory).
  std::atomic<uint32_t>& counter() { return counter_; }
  // The GPU register file image (packets and MMIO write it).
  uint32_t* registers() { return registers_->data(); }
  static constexpr size_t kRegisterCount = 0x5003;

  HostApi api() const { return api_; }
  rex::memory::Memory* memory() const { return memory_; }
  Backend* backend() const { return backend_.get(); }
  bool guest_gpu_ready() const { return guest_gpu_ready_.load(); }

 private:
  explicit NativeGraphicsSystem(HostApi api);

  bool CreateProvider(bool with_presentation);
  static uint32_t ReadRegisterThunk(void* ppc_context, void* self, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, void* self, uint32_t addr, uint32_t value);
  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);
  void MarkVblank();
  void OnHostGpuLoss(bool is_responsible);

  HostApi api_;
  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::ui::WindowedAppContext* app_context_ = nullptr;

  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  bool provider_supports_presentation_ = false;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  std::unique_ptr<Backend> backend_;
  std::unique_ptr<RingSkimmer> skimmer_;
  std::atomic<bool> guest_gpu_ready_{false};

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;
  std::atomic<bool> vsync_running_{false};
  rex::system::object_ref<rex::system::XHostThread> vsync_thread_;

  std::atomic<uint32_t> counter_{0};
  std::unique_ptr<std::array<uint32_t, kRegisterCount>> registers_;
  std::atomic_flag gpu_loss_reported_ = ATOMIC_FLAG_INIT;

  static NativeGraphicsSystem* instance_;
};

}  // namespace nr
