// NVRHI on the SDK's D3D12Provider device and direct queue, and the hand-over
// of a frame to the D3D12Presenter's guest output resource.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>

#include "backend/host_device.h"
#include "backend/gpu_loss.h"
#include "backend/cpu_profile.h"
#include "backend/dev_gpu_stats.h"
#include "backend/buffer_pool.h"
#include "backend/placed_textures.h"
#include "backend/d3d12_requirements.h"

REXCVAR_DECLARE(bool, native_validation);
REXCVAR_DECLARE(int32_t, native_debug);
REXCVAR_DECLARE(bool, native_d3d12_placed_textures);
REXCVAR_DECLARE(int32_t, native_d3d12_upload_chunk_kib);
REXCVAR_DECLARE(bool, native_d3d12_buffer_pool);

namespace nr {

namespace {

constexpr uint64_t kPlacedTextureHeapBytes = 64ull << 20;

class D3D12Host final : public HostDevice {
 public:
  explicit D3D12Host(rex::ui::d3d12::D3D12Provider& provider) : provider_(provider) {}

  ~D3D12Host() override {
    if (frame_fence_) frame_fence_->Release();
    if (budget_adapter_) budget_adapter_->Release();
    if (debug_queue_) {
      debug_queue_->UnregisterMessageCallback(debug_cookie_);
      debug_queue_->Release();
    }
  }

  bool Initialize() {
    IDXGIFactory4* factory = nullptr;
    if (SUCCEEDED(provider_.GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory)))) {
      factory->EnumAdapterByLuid(provider_.GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(&budget_adapter_));
      factory->Release();
    }
    const auto requirements = CheckD3D12Requirements(
        [this](D3D12_FEATURE feature, void* data, UINT size) {
          return provider_.GetDevice()->CheckFeatureSupport(feature, data, size);
        },
        [](const char* missing) { REXGPU_ERROR("rexgpu-native: D3D12 requirement missing: {}", missing); });
    REXLOG_INFO("rexgpu-native: D3D12 requirements: resource binding tier {}, shader model {}.{}, "
                "required formats {}/{}: {}", requirements.binding_tier,
                requirements.shader_model >> 4, requirements.shader_model & 15,
                requirements.formats_supported, requirements.formats_checked,
                requirements.supported ? "supported" : "unsupported");
    if (!requirements.supported) {
      REXGPU_ERROR("rexgpu-native: native D3D12 unavailable; choose Vulkan in Graphics > Advanced > "
                   "Graphics API, or use the Xenos renderer (--gpu_plugin=xenos)");
      return false;
    }
    if (REXCVAR_GET(native_validation) &&
        SUCCEEDED(provider_.GetDevice()->QueryInterface(IID_PPV_ARGS(&debug_queue_)))) {
      if (FAILED(debug_queue_->RegisterMessageCallback(DebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                                                       nullptr, &debug_cookie_))) {
        REXGPU_WARN("rexgpu-native: D3D12 debug callback unavailable; NVRHI validation remains enabled");
        debug_queue_->Release();
        debug_queue_ = nullptr;
      }
    } else if (REXCVAR_GET(native_validation)) {
      REXGPU_WARN("rexgpu-native: D3D12 info queue unavailable; enable the SDK D3D12 debug layer "
                  "(--d3d12_debug=true); NVRHI validation remains enabled");
    }
    nvrhi::d3d12::DeviceDesc desc;
    if (REXCVAR_GET(native_debug) & 524288) desc.maxTimerQueries = 2048;
    desc.errorCB = GetMessageCallback();
    desc.pDevice = provider_.GetDevice();
    desc.pGraphicsCommandQueue = provider_.GetDirectQueue();
    // The game renderer's bindless tables (backend/game_renderer.cpp): about
    // 35,000 views and 1,024 samplers; a shader-visible sampler heap holds at
    // most 2,048.
    desc.shaderResourceViewHeapSize = 65536;
    desc.samplerHeapSize = 2048;
    // Task 300: a committed texture costs ~0.3 ms to make and ~0.13 ms to release on an RTX 4070 Ti
    // against ~0.03 ms placed; a new upload buffer ~0.2-0.8 ms (thirdparty/NOTES.md).
    if (REXCVAR_GET(native_d3d12_placed_textures)) desc.placedTextureHeapSize = kPlacedTextureHeapBytes;
    desc.minUploadChunkSize = uint64_t(std::max(REXCVAR_GET(native_d3d12_upload_chunk_kib), 0)) << 10;
    if (desc.placedTextureHeapSize || desc.minUploadChunkSize) {
      REXLOG_INFO("rexgpu-native: D3D12 allocation: textures {}, upload chunks {}",
                  desc.placedTextureHeapSize ? "placed in pooled 64 MiB heaps" : "committed",
                  desc.minUploadChunkSize ? std::to_string(desc.minUploadChunkSize >> 10) + " KiB minimum, not zeroed"
                                          : std::string("NVRHI's default"));
    }
    device_ = nvrhi::d3d12::createDevice(desc);
    // Task 304: the game renderer zeroes the subresources a guest texture upload leaves out.
    g_texture_memory_not_zeroed.store(desc.placedTextureHeapSize != 0, std::memory_order_relaxed);
    // Task 309: the game renderer suballocates guest vertex and index buffers (buffer_pool.h).
    g_buffer_pool_enabled.store(REXCVAR_GET(native_d3d12_buffer_pool), std::memory_order_relaxed);
    if (REXCVAR_GET(native_d3d12_buffer_pool)) REXLOG_INFO("rexgpu-native: D3D12 guest buffers: pooled in 32 MiB buffers");
    g_dev_gpu_stats_enabled.store((REXCVAR_GET(native_debug) & 32768) != 0, std::memory_order_relaxed);
    if (!device_) {
      return false;
    }
    if (REXCVAR_GET(native_validation)) {
      device_ = nvrhi::validation::createValidationLayer(device_);
    }
    // Our own fence for WaitFrame, so the wait does not need NVRHI (and execute_mutex_).
    if (FAILED(provider_.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frame_fence_)))) {
      frame_fence_ = nullptr;
      REXGPU_WARN("rexgpu-native: unable to create the frame fence; frame waits hold the submit lock");
    }
    return true;
  }

  nvrhi::IDevice* device() override { return device_; }
  nvrhi::GraphicsAPI api() const override { return nvrhi::GraphicsAPI::D3D12; }

  MemoryBudget QueryMemoryBudget() override {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!budget_adapter_ || FAILED(budget_adapter_->QueryVideoMemoryInfo(
        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) || !info.Budget) return {};
    return {info.Budget, info.CurrentUsage, true};
  }

  void ExecuteCommandList(nvrhi::ICommandList* command_list) override {
    ExecuteFrameCommandList(command_list, nullptr);
  }
  void WaitFrame(nvrhi::IEventQuery* completion) override {
    if (!frame_fence_) {
      std::lock_guard lock(execute_mutex_);
      device_->waitEventQuery(completion);
      return;
    }
    // Only the lookup holds execute_mutex_. The wait itself is on our private fence, so the
    // submission thread can keep feeding the GPU while the renderer waits for an older frame.
    uint64_t value = 0;
    {
      std::lock_guard lock(execute_mutex_);
      for (const auto& entry : frame_values_) {
        if (entry.first == completion) value = entry.second;
      }
    }
    const uint64_t completed = frame_fence_->GetCompletedValue();
    if (completed == UINT64_MAX) {
      // Task 266: a removed device reports every fence as UINT64_MAX (we never signal that value). Report
      // it here (gpu_loss.h) instead of rendering on until the presenter notices.
      ReportIfRemoved("D3D12 frame wait");
      return;
    }
    if (value == 0 || completed >= value) return;
    // A null event blocks this thread until the fence reaches the value.
    frame_fence_->SetEventOnCompletion(value, nullptr);
  }
  // Task 268: reports a removed device with its reason (gpu_loss.h). A lost device's fence signal, idle
  // wait and frame wait all lead here; one GetDeviceRemovedReason call, never in a normal frame.
  void ReportIfRemoved(const char* where) {
    const HRESULT reason = provider_.GetDevice()->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) return;
    char what[128];
    std::snprintf(what, sizeof(what), "%s: device removed (reason 0x%08X)", where, unsigned(reason));
    ReportGpuLoss(what);
  }

  void ExecuteFrameCommandList(nvrhi::ICommandList* command_list, nvrhi::IEventQuery* completion) override {
    // ID3D12CommandQueue is thread safe, NVRHI's queue is not: executeCommandList records the list as in
    // flight and runGarbageCollection retires finished ones from the same list. The submission thread
    // (--native_async_submit) and the present thread (--native_async_present) both submit, so one at a
    // time (without the lock the device was removed within a minute at an unlimited frame rate).
    using clock = std::chrono::steady_clock;
    const uint64_t cpu_start = (REXCVAR_GET(native_debug) & 32768) ? CurrentThreadCpuMicroseconds() : 0;
    const auto t0 = clock::now();
#if defined(KK_DEV_TOOLS)
    if (completion && DevGpuLossNow(dev_loss_frames_, "d3d12")) {  // KK_DEV_GPU_LOSS_AFTER
      ReportGpuLoss("D3D12 submission: device removed (developer injection)");
      return;
    }
#endif
    bool signal_failed = false;
    std::unique_lock lock(execute_mutex_);
    const auto t1 = clock::now();
    device_->executeCommandList(command_list);
    if (completion) {
      device_->resetEventQuery(completion);
      device_->setEventQuery(completion, nvrhi::CommandQueue::Graphics);
      if (frame_fence_ && SUCCEEDED(provider_.GetDirectQueue()->Signal(frame_fence_, frame_fence_value_ + 1))) {
        ++frame_fence_value_;
        RecordFrameValue(completion, frame_fence_value_);
      } else if (frame_fence_) {
        signal_failed = true;
      }
    }
    const auto t2 = clock::now();
    device_->runGarbageCollection();
    const auto t3 = clock::now();
    auto us = [](clock::time_point a, clock::time_point b) {
      return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };
    times_.wait_us += us(t0, t1);
    times_.execute_us += us(t1, t2);
    times_.garbage_us += us(t2, t3);
    if (cpu_start) times_.cpu_us += CurrentThreadCpuMicroseconds() - cpu_start;
    ++times_.count;
    lock.unlock();
    if (signal_failed) ReportIfRemoved("D3D12 submission");
  }
  SubmitTimes submit_times() const override {
    std::lock_guard lock(execute_mutex_);
    return times_;
  }

  void WaitForIdle() override {
    {
      std::lock_guard lock(execute_mutex_);
      device_->waitForIdle();
    }
    ReportIfRemoved("D3D12 wait for idle");
  }
  void RunExclusive(const std::function<void()>& fn) override {
    {
      std::lock_guard lock(execute_mutex_);
      device_->waitForIdle();
      fn();
    }
    ReportIfRemoved("D3D12 exclusive work");
  }

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
  // Caller holds execute_mutex_. The renderer reuses a handful of completion queries.
  void RecordFrameValue(nvrhi::IEventQuery* completion, uint64_t value) {
    for (auto& entry : frame_values_) {
      if (entry.first == completion) {
        entry.second = value;
        return;
      }
    }
    // Queries recreated with the renderer leave old entries behind; keep the list short.
    if (frame_values_.size() >= 16) frame_values_.erase(frame_values_.begin());
    frame_values_.emplace_back(completion, value);
  }

  IDXGIAdapter3* budget_adapter_ = nullptr;
  static void CALLBACK DebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                                   D3D12_MESSAGE_ID id, LPCSTR description, void*) {
    if (severity <= D3D12_MESSAGE_SEVERITY_ERROR)
      REXGPU_ERROR("D3D12 validation {}: {}", uint32_t(id), description);
    else if (severity == D3D12_MESSAGE_SEVERITY_WARNING)
      REXLOG_WARN("D3D12 validation {}: {}", uint32_t(id), description);
  }

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
  mutable std::mutex execute_mutex_;
  SubmitTimes times_;
  ID3D12InfoQueue1* debug_queue_ = nullptr;
  DWORD debug_cookie_ = 0;
  nvrhi::DeviceHandle device_;
  ID3D12Fence* frame_fence_ = nullptr;
  uint64_t frame_fence_value_ = 0;
#if defined(KK_DEV_TOOLS)
  std::atomic<uint64_t> dev_loss_frames_{0};
#endif
  std::vector<std::pair<nvrhi::IEventQuery*, uint64_t>> frame_values_;
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
