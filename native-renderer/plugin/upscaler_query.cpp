// The launcher's question "which temporal upscalers run here" (native_upscaler_query.h).

#include "plugin/upscaler_query.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#if NR_GAME_RENDERER && NR_HAS_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <nvrhi/d3d12.h>

#include "backend/upscaler.h"
#endif
#if NR_HAS_VULKAN
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#endif

REXCVAR_DECLARE(std::string, native_backend);

namespace nr {

namespace {

std::mutex g_mutex;
bool g_started = false;
uint32_t g_state = NR_UPSCALER_QUERY_OFF;
NrUpscalerSupport g_result = {};
// Never destroyed: the process may end while it runs (the launcher's Quit); the game's start and
// the launcher's restart wait for it (WaitUpscalerQuery).
std::thread* g_thread = nullptr;

#if NR_GAME_RENDERER && NR_HAS_D3D12

class QuietMessages final : public nvrhi::IMessageCallback {
 public:
  void message(nvrhi::MessageSeverity severity, const char* text) override {
    if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal) {
      REXLOG_WARN("rexgpu-native: upscaler query: {}", text);
    }
  }
};

void Finish(uint32_t state, const NrUpscalerSupport& result) {
  std::lock_guard lock(g_mutex);
  g_result = result;
  g_result.state = state;
  g_state = state;
}

// The question itself, on its own thread: a Direct3D 12 device on the high-performance adapter (the
// one the game draws with), each SDK asked whether it runs on it, everything destroyed again.
void Run(bool vulkan) {
  NrUpscalerSupport out = {};
  out.size = sizeof(out);
  out.vulkan = vulkan ? 1 : 0;
  IDXGIFactory6* factory = nullptr;
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  std::string adapter_name;
  if (SUCCEEDED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory)))) {
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                         __uuidof(IDXGIAdapter1),
                                                         reinterpret_cast<void**>(&adapter)) == S_OK;
         ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      adapter->GetDesc1(&desc);
      // The lowest feature level any of the SDKs could run at: each SDK's own support check (below)
      // decides what runs on the device, not the level (a card at 11_0 or 11_1 can still run FSR 3.1
      // and XeSS).
      bool ok = false;
      if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        for (const D3D_FEATURE_LEVEL level :
             {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}) {
          if (SUCCEEDED(D3D12CreateDevice(adapter, level, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))) {
            ok = true;
            break;
          }
        }
      }
      if (ok) {
        for (const wchar_t* p = desc.Description; *p; ++p) adapter_name += *p < 128 ? char(*p) : '?';
      }
      adapter->Release();
      if (ok) break;
    }
    factory->Release();
  }
  if (device) {
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue)))) {
      queue = nullptr;
    }
  }
  if (!device || !queue) {
    if (queue) queue->Release();
    if (device) device->Release();
    REXLOG_WARN("rexgpu-native: upscaler query: no Direct3D 12 device");
    Finish(NR_UPSCALER_QUERY_FAILED, out);
    return;
  }
  UpscalerSupport s;
  {
    static QuietMessages messages;
    nvrhi::d3d12::DeviceDesc desc;
    desc.errorCB = &messages;
    desc.pDevice = device;
    desc.pGraphicsCommandQueue = queue;
    nvrhi::DeviceHandle nvrhi_device = nvrhi::d3d12::createDevice(desc);
    if (nvrhi_device) {
      s = QueryUpscalerSupport(nvrhi_device.Get());
      nvrhi_device->waitForIdle();
      nvrhi_device->runGarbageCollection();
    }
  }
  queue->Release();
  device->Release();
  out.vendor = s.vendor;
  out.fsr3 = s.fsr3;
  out.fsr4 = s.fsr4;
  out.dlss = s.dlss;
  out.xess = s.xess;
  std::string notes = s.notes;
  auto note = [&](const char* text) { notes += std::string(notes.empty() ? "" : "; ") + text; };
  if (vulkan) {
    if (out.fsr4) note("FSR 4: needs Direct3D 12 (the game draws with Vulkan)");
    out.fsr4 = 0;
#if !(defined(REX_UI_VULKAN_HAS_INSTANCE_REQUESTS) && defined(REX_UI_VULKAN_HAS_DEVICE_REQUESTS))
    // The SDK cannot make its Vulkan device with what DLSS and XeSS ask for (sdk-patches/0003).
    if (out.dlss || out.xess) note("DLSS, XeSS: this build's SDK cannot enable their Vulkan extensions");
    out.dlss = out.xess = 0;
#endif
  }
  std::snprintf(out.notes, sizeof(out.notes), "%s", notes.c_str());
  REXLOG_INFO("rexgpu-native: upscalers this PC offers ({}, {}): FSR 3.1 {}, FSR 4 {}, DLSS {}, XeSS {}{}{}",
              adapter_name, vulkan ? "Vulkan" : "Direct3D 12", out.fsr3 ? "yes" : "no", out.fsr4 ? "yes" : "no",
              out.dlss ? "yes" : "no", out.xess ? "yes" : "no", notes.empty() ? "" : "; ", notes);
  Finish(NR_UPSCALER_QUERY_DONE, out);
}

#endif

}  // namespace

uint32_t UpscalerQuery(NrUpscalerSupport* out) {
  std::lock_guard lock(g_mutex);
  if (!g_started) {
    g_started = true;
#if NR_GAME_RENDERER && NR_HAS_D3D12
#if NR_HAS_VULKAN
    const bool vulkan = REXCVAR_GET(native_backend) != "d3d12";
#else
    const bool vulkan = false;
#endif
    g_state = NR_UPSCALER_QUERY_RUNNING;
    g_thread = new std::thread([vulkan] { Run(vulkan); });
#else
    REXLOG_INFO("rexgpu-native: upscaler query: this build has no temporal upscalers to offer");
    g_state = NR_UPSCALER_QUERY_OFF;
#endif
  }
  if (out && out->size >= sizeof(NrUpscalerSupport)) {
    if (g_state == NR_UPSCALER_QUERY_DONE) {
      *out = g_result;
    } else {
      *out = {};
      out->size = sizeof(NrUpscalerSupport);
    }
    out->state = g_state;
  }
  return g_state;
}

void WaitUpscalerQuery() {
  std::thread* thread = nullptr;
  {
    std::lock_guard lock(g_mutex);
    thread = g_thread;
  }
  static std::mutex join_mutex;  // two callers: one joins, the other waits for it
  std::lock_guard join_lock(join_mutex);
  if (thread && thread->joinable()) thread->join();
}

}  // namespace nr
