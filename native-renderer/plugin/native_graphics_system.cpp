#include "plugin/native_graphics_system.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
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
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/provider.h>
#endif

#include "backend/api_binding.h"
#include "backend/backend.h"
#include "backend/draw_state.h"
#include "backend/host_device.h"
#include "plugin/launcher_prewarm.h"
#include "plugin/plugin_settings.h"
#include "plugin/native_startup.h"
#include "plugin/ring_skimmer.h"
#include "plugin/upscaler_query.h"
#if NR_GAME_RENDERER
#include "plugin/shader_pack_service.h"
#endif
#if NR_GAME_RENDERER && NR_HAS_VULKAN
#include "backend/upscaler_vulkan.h"
#endif

REXCVAR_DECLARE(bool, native_draws);
REXCVAR_DECLARE(bool, native_game);
REXCVAR_DECLARE(std::string, native_shader_pack);
REXCVAR_DECLARE(bool, native_log_packets);
REXCVAR_DECLARE(std::string, native_test);
REXCVAR_DECLARE(bool, native_wvp_transpose);
REXCVAR_DECLARE(bool, native_all_targets);
REXCVAR_DECLARE(bool, native_main_pass_only);
REXCVAR_DECLARE(bool, native_pipeline_cache);
REXCVAR_DECLARE(bool, native_pipeline_list);
REXCVAR_DECLARE(bool, native_vk_pipeline_cache);
REXCVAR_DECLARE(bool, native_present_unlocked);
REXCVAR_DECLARE(bool, native_async_submit);
REXCVAR_DECLARE(bool, native_async_present);
REXCVAR_DECLARE(bool, native_render_thread_fences);


namespace nr {

namespace {

// Upscalers work on the game's Vulkan device: its instance and device are made with what they ask
// for (docs/upscalers.md, "Vulkan"; the SDK's sdk-patches/0003). The graphics panel (F8) switches the
// upscaler while the game runs, so whatever this PC can run is asked for, whichever upscaler the
// settings name now: DLSS's needs on NVIDIA, XeSS's on any GPU (extensions and features are enabled
// only where supported). Nothing is asked for when the SDK cannot take requests.
void PrepareVulkanForUpscalers() {
#if NR_GAME_RENDERER && NR_HAS_VULKAN && defined(REX_UI_VULKAN_HAS_INSTANCE_REQUESTS) && \
    defined(REX_UI_VULKAN_HAS_DEVICE_REQUESTS)
  UpscaleConfig config = ReadUpscalerChoice();
  config.kind = UpscalerKind::kAuto;  // any of them may be chosen later
  bool dlss = false, xess = false;
  UpscalerVulkanWants(config, 0, &dlss, &xess);
  std::vector<std::string> instance_extensions = UpscalerVulkanInstanceExtensions(dlss, true);
  rex::ui::vulkan::SetVulkanInstanceExtensionRequests(instance_extensions);
  rex::ui::vulkan::SetVulkanDeviceRequestCallback([config](VkInstance instance, VkPhysicalDevice physical) {
    rex::ui::vulkan::VulkanDeviceRequests requests;
    bool want_dlss = false, want_xess = false;
    UpscalerVulkanWants(config, UpscalerVulkanVendor(instance, physical, nullptr), &want_dlss, &want_xess);
    want_xess = true;  // XeSS runs on other vendors' GPUs too
    UpscalerVulkanNeeds needs = UpscalerVulkanDeviceNeeds(want_dlss, want_xess, instance, physical, nullptr);
    REXGPU_INFO("rexgpu-native: the upscalers ask the Vulkan device for {} extensions and {} feature structures",
                needs.extensions.size(), needs.features.size());
    requests.extensions = std::move(needs.extensions);
    requests.features = std::move(needs.features);
    return requests;
  });
  REXGPU_INFO("rexgpu-native: the upscalers ask the Vulkan instance for {} extensions", instance_extensions.size());
#endif
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
  // The launcher's upscaler question (its Direct3D 12 device and the SDKs it loaded) is over first.
  WaitUpscalerQuery();
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
      PrepareVulkanForUpscalers();
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
      ReportStartupFailure("could not initialise its graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    REXGPU_ERROR("rexgpu-native: SetupPresentation after a headless SetupGuestGpu");
    ReportStartupFailure("could not initialise graphics presentation");
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
    ReportStartupFailure("could not initialise its presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS NativeGraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                                             rex::system::KernelState* kernel_state) {
#if defined(KK_DEV_TOOLS)
  const auto mode = rex::cvar::GetFlagByName("kk_dev_startup_failure");
  const auto target = rex::cvar::GetFlagByName("kk_dev_startup_api");
  if (target == "all" || target == (api_ == HostApi::kD3D12 ? "d3d12" : "vulkan")) {
    if (mode == "init") {
      ReportStartupFailure("failed native initialisation (developer injection)");
      return X_STATUS_UNSUCCESSFUL;
    }
    if (mode == "device-lost") { OnHostGpuLoss(true); return X_STATUS_UNSUCCESSFUL; }
  }
#endif
  // The launcher's prewarm ends here: its device goes (saving the driver's pipeline cache) before
  // the game's is made.
  StopLauncherPrewarm();
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  if (!provider_) {
    if (!CreateProvider(false)) {
      REXGPU_ERROR("rexgpu-native: unable to create the headless graphics provider");
      ReportStartupFailure("could not initialise its headless graphics provider");
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
    ReportStartupFailure("could not initialise its native device");
    return X_STATUS_UNSUCCESSFUL;
  }
  rex::system::X_VIDEO_MODE video_mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  backend_ = std::make_unique<Backend>(std::move(host), memory_);
  const GameSettings game = ReadGameSettings(api_ == HostApi::kVulkan);
  if (!backend_->Initialize(std::max<uint32_t>(1, video_mode.display_width),
                            std::max<uint32_t>(1, video_mode.display_height), game)) {
    REXGPU_ERROR("rexgpu-native: backend initialisation failed");
    ReportStartupFailure("failed native renderer initialisation");
    backend_.reset();
    return X_STATUS_UNSUCCESSFUL;
  }
  {
    // --native_pipeline_cache_dir wins; otherwise the cache root arrives in InitializeShaderStorage
    // (after this point when the SDK's app runs the game; a place it brought earlier is kept). A
    // shipped list alone needs no folder: it starts now.
    OpenShaderPackFor(shader_cache_root_);  // when the cache root came first
    if (pipeline_place_.empty()) pipeline_place_ = PipelinePlaceFromCvar();
    if (!pipeline_place_.empty() || !PipelineFilesNeedCacheRoot()) StartPipelines();
  }
  backend_->set_unlocked_present(REXCVAR_GET(native_present_unlocked));
  backend_->set_async_submit(REXCVAR_GET(native_async_submit));
  backend_->set_async_present(REXCVAR_GET(native_async_present));
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
  // these interrupts every frame. Like the SDK's GraphicsSystem, it ticks at
  // 1000 Hz when the Xenos plugin's "vsync" cvar is off: the port turns it off
  // and paces frames with kk_frame_rate, so a fixed 60 Hz here capped the game.
  vsync_running_ = true;
  vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        double refresh_rate_hz = std::max(1.0, double(float(mode.refresh_rate)));
        uint64_t tick_frequency = rex::chrono::Clock::guest_tick_frequency();
        uint64_t vsync_interval_ticks =
            std::max<uint64_t>(1, uint64_t(double(tick_frequency) / refresh_rate_hz));
        uint64_t no_vsync_interval_ticks = std::max<uint64_t>(1, tick_frequency / 1000);
        uint64_t last_frame_time = rex::chrono::Clock::QueryGuestTickCount();
        while (vsync_running_) {
          uint64_t interval_ticks = rex::cvar::GetFlagByName("vsync") == "false"
                                        ? no_vsync_interval_ticks
                                        : vsync_interval_ticks;
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

// The cache root the application gives the graphics system for persistent storage: the pipeline
// cache file goes under it (the Xenos plugin keeps its shader storage there too).
void NativeGraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                                                   bool blocking) {
  (void)blocking;
  shader_cache_root_ = cache_root;
  if (backend_) OpenShaderPackFor(cache_root);
  if (pipelines_started_ || !pipeline_place_.empty()) return;
  pipeline_place_ = PipelinePlaceUnder(cache_root, title_id);
  if (backend_) StartPipelines();
}

// The shader pack (unless --native_shader_pack named one): the user's cache, where the launcher builds it
// from the player's game files, else beside the game, else every shader is compiled with DXC when the game
// creates it. Before the pipelines start, so their prewarm finds the pack's shaders.
void NativeGraphicsSystem::OpenShaderPackFor(const std::filesystem::path& cache_root) {
#if NR_GAME_RENDERER
  if (!backend_ || cache_root.empty() || !REXCVAR_GET(native_game) || backend_->has_shader_pack() ||
      !REXCVAR_GET(native_shader_pack).empty()) {
    return;
  }
  std::string where;
  const std::string pack = FindShaderPackForRun(cache_root, api_ == HostApi::kVulkan, &where);
  if (!pack.empty() && backend_->OpenShaderPack(pack)) {
    REXLOG_INFO("rexgpu-native: shader pack from {}: {}", where, pack);
  } else {
    REXLOG_WARN("rexgpu-native: no shader pack{} (the launcher builds it into {}): shaders are compiled with "
                "DXC on first use",
                pack.empty() ? "" : " that opens", (cache_root / kkshaders::kPackFolderName).string());
  }
#else
  (void)cache_root;
#endif
}

// In this order: the driver's pipeline cache (read while nothing creates a pipeline), the player's
// cache file, then the shipped list (each description is made once, whichever names it first).
// Before the game's first draw: the SDK's app initialises the shader storage before the game's
// main thread starts.
void NativeGraphicsSystem::StartPipelines() {
  if (pipelines_started_ || !backend_) return;
  pipelines_started_ = true;
  if (REXCVAR_GET(native_vk_pipeline_cache) && !pipeline_place_.empty() &&
      !backend_->SetDriverPipelineCache(pipeline_place_.driver_cache())) {
    REXGPU_WARN("rexgpu-native: native_vk_pipeline_cache needs Vulkan and the game renderer; not used");
  }
  if (REXCVAR_GET(native_pipeline_cache) && !pipeline_place_.empty()) {
    backend_->SetPipelineCache(pipeline_place_.records());
  }
  if (REXCVAR_GET(native_pipeline_list)) backend_->LoadPipelineList(ShippedPipelineList());
  backend_->LoadResourceList((ExecutableFolder() / "kkresources.list").string());
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
  ReportStartupFailure("lost its graphics device during startup");
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
        // The renderer's calls queued so far precede every packet of this kick (render thread).
        skimmer_->UpdateWritePointer(value, backend_ ? backend_->RenderTicket() : 0);
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

void NativeGraphicsSystem::WaitForRenderer(uint64_t ticket) {
  static const bool gate = REXCVAR_GET(native_render_thread_fences);
  if (gate && backend_ && ticket) backend_->WaitRendered(ticket);
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
  // With --native_async_present the frame is shown after Present returns: a later swap sees it.
  if (!startup_presented_ && backend_->presented_game_frames()) {
    startup_presented_ = true;
    ReportStartupPresented();
  }
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
