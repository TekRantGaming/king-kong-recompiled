// The launcher's question "which temporal upscalers run here" (native_upscaler_query.h).

#include "plugin/upscaler_query.h"
#include "plugin/gpu_preference.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

#if NR_GAME_RENDERER && NR_HAS_D3D12
// Task 335: two players' launchers (Vulkan, NVIDIA) crashed in NVIDIA's Direct3D driver while this question's
// Direct3D 12 device was being made beside the launcher's own presentation. The answer is kept, with the graphics
// card, driver and renderer build it is for, and asked again only when one of them changes.
REXCVAR_DEFINE_STRING(native_upscaler_query_saved, "", "Native renderer",
                      "Internal: the last answer to the launcher's upscaler question and the graphics card, driver "
                      "and renderer build it is for; asked again when one of them changes. Empty: asked again.");
#endif

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
std::string g_key;          // the card, driver and build of this question (SavedKey)
bool g_save_pending = false;  // the thread's answer is still to go into native_upscaler_query_saved

// What the answer depends on: the adapter the question uses (name, vendor and device id), its driver version
// and this renderer's build (its own file's link time stamp: a new build asks again). Empty when unknown.
std::string SavedKey() {
  std::string name;
  IDXGIAdapter1* adapter = ChosenD3D12Adapter(&name);
  if (!adapter) return {};
  DXGI_ADAPTER_DESC1 desc{};
  LARGE_INTEGER driver{};
  const bool have_desc = SUCCEEDED(adapter->GetDesc1(&desc));
  const bool have_driver = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver));
  adapter->Release();
  if (!have_desc || !have_driver) return {};
  uint32_t stamp = 0;
  HMODULE self = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&SavedKey), &self) && self) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
    stamp = nt->FileHeader.TimeDateStamp;
  }
  for (char& c : name)
    if (c == '|') c = ' ';
  const auto n = static_cast<unsigned long long>(driver.QuadPart);
  char key[512];
  std::snprintf(key, sizeof(key), "%s/%04x:%04x/%llu.%llu.%llu.%llu/%08x", name.c_str(), desc.VendorId, desc.DeviceId,
                n >> 48, (n >> 32) & 65535, (n >> 16) & 65535, n & 65535, stamp);
  return key;
}

// native_upscaler_query_saved: "1|key|vendor|fsr3 fsr4 dlss xess (four 0/1 digits)|notes".
std::string SavedText(const std::string& key, const NrUpscalerSupport& r) {
  char head[64];
  std::snprintf(head, sizeof(head), "|%x|%d%d%d%d|", r.vendor, r.fsr3 ? 1 : 0, r.fsr4 ? 1 : 0, r.dlss ? 1 : 0,
                r.xess ? 1 : 0);
  return "1|" + key + head + r.notes;
}

bool ReadSaved(const std::string& text, const std::string& key, NrUpscalerSupport* out) {
  if (key.empty() || text.rfind("1|" + key + "|", 0) != 0) return false;
  const size_t vendor_at = 3 + key.size();
  const size_t flags_at = text.find('|', vendor_at);
  if (flags_at == std::string::npos || text.size() < flags_at + 6 || text[flags_at + 5] != '|') return false;
  const std::string flags = text.substr(flags_at + 1, 4);
  if (flags.find_first_not_of("01") != std::string::npos) return false;
  *out = {};
  out->size = sizeof(*out);
  out->vendor = uint32_t(std::strtoul(text.substr(vendor_at, flags_at - vendor_at).c_str(), nullptr, 16));
  out->fsr3 = flags[0] == '1', out->fsr4 = flags[1] == '1', out->dlss = flags[2] == '1', out->xess = flags[3] == '1';
  std::snprintf(out->notes, sizeof(out->notes), "%s", text.c_str() + flags_at + 6);
  return true;
}
#endif

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
void Run() {
  NrUpscalerSupport out = {};
  out.size = sizeof(out);
  out.vulkan = 0;
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  std::string adapter_name;
  // The adapter the game's D3D12 provider uses (plugin/gpu_preference.h: high performance, never a
  // software or Basic Render adapter, or the player's d3d12_adapter), task 203.
  if (IDXGIAdapter1* adapter = ChosenD3D12Adapter(&adapter_name)) {
    // The lowest feature level any of the SDKs could run at: each SDK's own support check (below)
    // decides what runs on the device, not the level (a card at 11_0 or 11_1 can still run FSR 3.1
    // and XeSS).
    bool ok = false;
    for (const D3D_FEATURE_LEVEL level :
         {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}) {
      if (SUCCEEDED(D3D12CreateDevice(adapter, level, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device)))) {
        ok = true;
        break;
      }
    }
    if (!ok) adapter_name.clear();
    adapter->Release();
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
  std::snprintf(out.notes, sizeof(out.notes), "%s", notes.c_str());
  REXLOG_INFO("rexgpu-native: upscalers this PC offers ({}, {}): FSR 3.1 {}, FSR 4 {}, DLSS {}, XeSS {}{}{}",
              adapter_name, "Direct3D 12", out.fsr3 ? "yes" : "no", out.fsr4 ? "yes" : "no",
              out.dlss ? "yes" : "no", out.xess ? "yes" : "no", notes.empty() ? "" : "; ", notes);
  {
    std::lock_guard lock(g_mutex);
    g_save_pending = !g_key.empty();  // saved by the next caller (the settings are not this thread's)
  }
  Finish(NR_UPSCALER_QUERY_DONE, out);
}

#endif

}  // namespace

uint32_t UpscalerQuery(NrUpscalerSupport* out) { return UpscalerQueryAnswer(out, true); }

uint32_t UpscalerQueryPeek(NrUpscalerSupport* out) { return UpscalerQueryAnswer(out, false); }

uint32_t UpscalerQueryAnswer(NrUpscalerSupport* out, bool start) {
  std::lock_guard lock(g_mutex);
  if (!g_started && start) {
    g_started = true;
#if NR_GAME_RENDERER && NR_HAS_D3D12
    g_key = SavedKey();
    NrUpscalerSupport saved;
    if (ReadSaved(REXCVAR_GET(native_upscaler_query_saved), g_key, &saved)) {
      // Task 335: the same card, driver and build as the saved answer: no Direct3D 12 device in this process.
      g_result = saved;
      g_result.state = g_state = NR_UPSCALER_QUERY_DONE;
      REXLOG_INFO("rexgpu-native: upscalers this PC offers ({}, Direct3D 12, saved answer for this graphics card, "
                  "driver and build): FSR 3.1 {}, FSR 4 {}, DLSS {}, XeSS {}{}{}",
                  g_key.substr(0, g_key.find('/')), saved.fsr3 ? "yes" : "no", saved.fsr4 ? "yes" : "no",
                  saved.dlss ? "yes" : "no", saved.xess ? "yes" : "no", saved.notes[0] ? "; " : "", saved.notes);
    } else {
      g_state = NR_UPSCALER_QUERY_RUNNING;
      g_thread = new std::thread([] { Run(); });
    }
#else
    REXLOG_INFO("rexgpu-native: upscaler query: this build has no temporal upscalers to offer");
    g_state = NR_UPSCALER_QUERY_OFF;
#endif
  }
#if NR_GAME_RENDERER && NR_HAS_D3D12
  if (g_save_pending && g_state == NR_UPSCALER_QUERY_DONE) {
    g_save_pending = false;
    rex::cvar::SetFlagByName("native_upscaler_query_saved", SavedText(g_key, g_result));
  }
#endif
  if (out && out->size >= sizeof(NrUpscalerSupport)) {
    if (g_state == NR_UPSCALER_QUERY_DONE) {
      *out = g_result;
      // Keep the SDK answer for both APIs: the player can change the API in the launcher.
#if NR_HAS_VULKAN
      out->vulkan = REXCVAR_GET(native_backend) != "d3d12";
      if (out->vulkan) {
        out->fsr4 = 0;  // AMD FSR 4 has no Vulkan backend.
        std::string notes = out->notes;
        if (g_result.fsr4)
          notes += std::string(notes.empty() ? "" : "; ") + "FSR 4: needs Direct3D 12 (Vulkan selected)";
#if !(defined(REX_UI_VULKAN_HAS_INSTANCE_REQUESTS) && defined(REX_UI_VULKAN_HAS_DEVICE_REQUESTS))
        out->dlss = out->xess = 0;
        if (g_result.dlss || g_result.xess)
          notes += std::string(notes.empty() ? "" : "; ") + "DLSS, XeSS: this build cannot enable their Vulkan extensions";
#endif
        std::snprintf(out->notes, sizeof(out->notes), "%s", notes.c_str());
      }
#endif
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
