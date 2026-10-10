#include "plugin/native_graphics_system.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iterator>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/windowed_app_context.h>

#if NR_HAS_D3D12
#include <rex/ui/d3d12/d3d12_provider.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if NR_HAS_VULKAN
#include <rex/ui/vulkan/provider.h>
#endif

#include "backend/api_binding.h"
#include "backend/backend.h"
#include "backend/draw_state.h"
#include "backend/host_device.h"
#include "plugin/ring_skimmer.h"

REXCVAR_DECLARE(bool, native_draws);
REXCVAR_DECLARE(bool, native_log_packets);
REXCVAR_DECLARE(std::string, native_test);
REXCVAR_DECLARE(bool, native_wvp_transpose);
REXCVAR_DECLARE(bool, native_all_targets);
REXCVAR_DECLARE(bool, native_main_pass_only);
REXCVAR_DECLARE(bool, native_game);
REXCVAR_DECLARE(std::string, native_shader_pack);
REXCVAR_DECLARE(std::string, native_dxc);
REXCVAR_DECLARE(bool, native_element_endian);
REXCVAR_DECLARE(bool, native_flip_front_face);
REXCVAR_DECLARE(bool, native_texture_tail_mips);
REXCVAR_DECLARE(int32_t, native_dump_frame);
REXCVAR_DECLARE(int32_t, native_debug);

namespace nr {

namespace {

// The folder of the running executable (the game's).
std::filesystem::path ExecutableFolder() {
#if defined(_WIN32)
  wchar_t path[32768];
  DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  if (n == 0 || n >= std::size(path)) return {};
  return std::filesystem::path(std::wstring(path, n)).parent_path();
#else
  std::error_code ec;
  auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : p.parent_path();
#endif
}

std::string FindShaderPack(bool vulkan) {
  std::string wanted = REXCVAR_GET(native_shader_pack);
  if (!wanted.empty()) return wanted;
  const std::filesystem::path folder = ExecutableFolder();
  for (const char* name : {vulkan ? "kkshaders-spirv.pack" : "kkshaders-dxil.pack", "kkshaders.pack"}) {
    std::error_code ec;
    if (std::filesystem::exists(folder / name, ec)) return (folder / name).string();
  }
  return {};
}

}  // namespace

NativeGraphicsSystem* NativeGraphicsSystem::instance_ = nullptr;

NativeGraphicsSystem* NativeGraphicsSystem::Create(std::string_view backend) {
  // Vulkan is the primary backend (user decision, 10 October 2026): "any"
  // takes it whenever the SDK was built with it, D3D12 otherwise.
#if NR_HAS_VULKAN
  if (backend == "any") {
    backend = "vulkan";
  }
#endif
#if NR_HAS_D3D12
  if (backend == "any" || backend == "d3d12") {
    if (rex::ui::d3d12::D3D12Provider::IsD3D12APIAvailable()) {
      return new NativeGraphicsSystem(HostApi::kD3D12);
    }
    REXGPU_WARN("rexgpu-native: Direct3D 12 is not available on this machine");
    if (backend == "d3d12") {
      return nullptr;
    }
  }
#endif
#if NR_HAS_VULKAN
  if (backend == "any" || backend == "vulkan") {
    return new NativeGraphicsSystem(HostApi::kVulkan);
  }
#endif
  return nullptr;
}

NativeGraphicsSystem::NativeGraphicsSystem(HostApi api)
    : api_(api), registers_(std::make_unique<std::array<uint32_t, kRegisterCount>>()) {
  registers_->fill(0);
  instance_ = this;
  REXGPU_INFO("rexgpu-native: created ({})", api == HostApi::kD3D12 ? "Direct3D 12" : "Vulkan");
}

NativeGraphicsSystem::~NativeGraphicsSystem() {
  Shutdown();
  if (instance_ == this) {
    instance_ = nullptr;
  }
}

bool NativeGraphicsSystem::CreateProvider(bool with_presentation) {
  switch (api_) {
#if NR_HAS_D3D12
    case HostApi::kD3D12:
      // D3D12 providers always support presentation (the swap chain is the
      // presenter's).
      provider_ = rex::ui::d3d12::D3D12Provider::Create();
      break;
#endif
#if NR_HAS_VULKAN
    case HostApi::kVulkan:
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, with_presentation);
      break;
#endif
    default:
      break;
  }
  return provider_ != nullptr;
}

X_STATUS NativeGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext* app_context) {
  if (presenter_) {
    return X_STATUS_SUCCESS;
  }
  if (!provider_) {
    if (!CreateProvider(true)) {
      REXGPU_ERROR("rexgpu-native: unable to create the graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    REXGPU_ERROR("rexgpu-native: SetupPresentation after a headless SetupGuestGpu");
    return X_STATUS_UNSUCCESSFUL;
  }

  app_context_ = app_context;
  auto loss_callback = [this](bool is_responsible, bool /*statically_from_ui_thread*/) {
    OnHostGpuLoss(is_responsible);
  };
  if (app_context_) {
    // Presenters are created on the UI thread.
    app_context_->CallInUIThreadSynchronous(
        [this, loss_callback]() { presenter_ = provider_->CreatePresenter(loss_callback); });
  } else {
    presenter_ = provider_->CreatePresenter(loss_callback);
  }
  if (!presenter_) {
    REXGPU_ERROR("rexgpu-native: unable to create the presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS NativeGraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                                             rex::system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  if (!provider_) {
    if (!CreateProvider(false)) {
      REXGPU_ERROR("rexgpu-native: unable to create the headless graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = false;
  }

  // NVRHI on the provider's device.
  std::unique_ptr<HostDevice> host;
  switch (api_) {
#if NR_HAS_D3D12
    case HostApi::kD3D12:
      host = CreateD3D12HostDevice(*provider_);
      break;
#endif
#if NR_HAS_VULKAN
    case HostApi::kVulkan:
      host = CreateVulkanHostDevice(*provider_);
      break;
#endif
    default:
      break;
  }
  if (!host) {
    REXGPU_ERROR("rexgpu-native: unable to create the NVRHI device");
    return X_STATUS_UNSUCCESSFUL;
  }
  rex::system::X_VIDEO_MODE video_mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  backend_ = std::make_unique<Backend>(std::move(host), memory_);
  GameSettings game;
  game.enabled = REXCVAR_GET(native_game);
  game.shader_pack = FindShaderPack(api_ == HostApi::kVulkan);
  game.dxc = REXCVAR_GET(native_dxc);
  if (game.dxc.empty()) game.dxc = ExecutableFolder().string();
  game.element_endian = REXCVAR_GET(native_element_endian);
  game.flip_front_face = REXCVAR_GET(native_flip_front_face);
  game.texture_tail_mips = REXCVAR_GET(native_texture_tail_mips);
  game.dump_frame = REXCVAR_GET(native_dump_frame);
  game.debug = uint32_t(REXCVAR_GET(native_debug));
  if (!backend_->Initialize(std::max<uint32_t>(1, video_mode.display_width),
                            std::max<uint32_t>(1, video_mode.display_height), game)) {
    REXGPU_ERROR("rexgpu-native: backend initialisation failed");
    backend_.reset();
    return X_STATUS_UNSUCCESSFUL;
  }
  backend_->renderer().options().transpose_wvp = REXCVAR_GET(native_wvp_transpose);
  backend_->renderer().options().only_main_surface = !REXCVAR_GET(native_all_targets);
  backend_->renderer().options().only_depth_tested = REXCVAR_GET(native_main_pass_only);
  backend_->set_test_picture(REXCVAR_GET(native_test) == "clear" ? TestPicture::kClear
                                                                  : TestPicture::kTriangle);

  // GPU registers: 0x7FC80000-0x7FCFFFFF. The game's library writes the ring
  // buffer write pointer here and reads a few status registers.
  memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this, ReadRegisterThunk,
                                 WriteRegisterThunk);

  skimmer_ = std::make_unique<RingSkimmer>(this, memory_, REXCVAR_GET(native_log_packets));
  if (!skimmer_->Start(kernel_state_)) {
    REXGPU_ERROR("rexgpu-native: unable to start the ring skimmer");
    return X_STATUS_UNSUCCESSFUL;
  }

  // Guest vblank timer at the video mode's refresh rate: the game waits for
  // these interrupts every frame.
  vsync_running_ = true;
  vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        double refresh_rate_hz = std::max(1.0, double(float(mode.refresh_rate)));
        uint64_t tick_frequency = rex::chrono::Clock::guest_tick_frequency();
        uint64_t interval_ticks =
            std::max<uint64_t>(1, uint64_t(double(tick_frequency) / refresh_rate_hz));
        uint64_t last_frame_time = rex::chrono::Clock::QueryGuestTickCount();
        while (vsync_running_) {
          uint64_t current_time = rex::chrono::Clock::QueryGuestTickCount();
          while (current_time - last_frame_time >= interval_ticks) {
            MarkVblank();
            last_frame_time += interval_ticks;
          }
          rex::thread::Sleep(std::chrono::milliseconds(1));
        }
        return 0;
      }));
  vsync_thread_->set_name("GPU VSync (native)");
  vsync_thread_->Create();

  guest_gpu_ready_ = true;
  REXGPU_INFO("rexgpu-native: guest GPU ready ({}x{} @ {} Hz)", uint32_t(video_mode.display_width),
              uint32_t(video_mode.display_height), uint32_t(video_mode.refresh_rate));
  return X_STATUS_SUCCESS;
}

void NativeGraphicsSystem::Shutdown() {
  guest_gpu_ready_ = false;
  if (skimmer_) {
    skimmer_->Stop();
    skimmer_.reset();
  }
  if (vsync_thread_) {
    vsync_running_ = false;
    vsync_thread_->Wait(0, 0, 0, nullptr);
    vsync_thread_.reset();
  }
  if (backend_) {
    backend_->Shutdown();
    backend_.reset();
  }
  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    presenter_.reset();
  }
  provider_.reset();
}

void NativeGraphicsSystem::OnHostGpuLoss(bool /*is_responsible*/) {
  if (gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  rex::FatalError("Graphics device lost (native renderer)");
}

uint32_t NativeGraphicsSystem::ReadRegisterThunk(void* /*ppc_context*/, void* self,
                                                 uint32_t addr) {
  return static_cast<NativeGraphicsSystem*>(self)->ReadRegister(addr);
}

void NativeGraphicsSystem::WriteRegisterThunk(void* /*ppc_context*/, void* self, uint32_t addr,
                                              uint32_t value) {
  static_cast<NativeGraphicsSystem*>(self)->WriteRegister(addr, value);
}

uint32_t NativeGraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      rex::system::X_VIDEO_MODE mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
      return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:  // interrupt status: vblank
      return 1;
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      rex::system::X_VIDEO_MODE mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
      uint32_t w = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
      uint32_t h = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      return (w << 16) | h;
    }
    default:
      break;
  }
  if (r >= kRegisterCount) {
    return 0;
  }
  return (*registers_)[r];
}

void NativeGraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      if (skimmer_) {
        skimmer_->UpdateWritePointer(value);
      }
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_DEBUG("rexgpu-native: GPU register {:04X} write {:08X}", r, value);
      break;
  }
  if (r < kRegisterCount) {
    (*registers_)[r] = value;
  }
}

void NativeGraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  if (skimmer_) {
    skimmer_->InitializeRingBuffer(ptr, size_log2);
  }
}

void NativeGraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  if (skimmer_) {
    skimmer_->EnableReadPointerWriteBack(ptr, block_size_log2);
  }
}

void NativeGraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("rexgpu-native: SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void NativeGraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) {
    return;
  }
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread) {
    return;
  }
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(uint8_t(cpu));
  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args, 2);
}

void NativeGraphicsSystem::MarkVblank() {
  counter_++;
  DispatchInterruptCallback(0, 2);
}

void NativeGraphicsSystem::OnSwap(uint32_t /*frontbuffer_ptr*/, uint32_t width, uint32_t height) {
  if (!presenter_ || !backend_) {
    return;
  }
  rex::system::X_VIDEO_MODE mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  backend_->Present(presenter_.get(), width, height, std::max<uint32_t>(1, mode.display_width),
                    std::max<uint32_t>(1, mode.display_height));
}

// ------------------------------------------------------------ the C API ---

namespace {

ApiBinding g_api_binding;

// The binding is created once and may be cached by the hooks before the
// system exists: look the system up on every call.
DrawTracker* ResolveDraws(void* /*owner*/) {
  NativeGraphicsSystem* system = NativeGraphicsSystem::Instance();
  if (!system || !system->guest_gpu_ready() || !system->backend() || !REXCVAR_GET(native_draws)) {
    return nullptr;
  }
  return system->backend()->draws();
}

}  // namespace

const NrApi* NativeGraphicsSystem::GetApi() {
  static const bool initialized = [] {
    InitApiBinding(g_api_binding, nullptr, ResolveDraws);
    return true;
  }();
  (void)initialized;
  return &g_api_binding.api;
}

}  // namespace nr
