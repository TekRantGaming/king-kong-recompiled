#include "plugin/gpu_preference.h"

#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "backend/gpu_choice.h"

#if NR_HAS_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif
#if NR_HAS_VULKAN
#include <rex/ui/vulkan/instance.h>
#endif

namespace nr {
namespace {

using gpu_choice::Candidate;

std::string List(const std::vector<Candidate>& list) {
  std::string out;
  for (const auto& c : list) {
    if (!out.empty()) out += "; ";
    out += std::to_string(c.index) + " " + c.name;
    if (c.software) out += " (software)";
    else if (gpu_choice::IsLayeredName(c.name)) out += " (layered)";
    else if (!c.usable) out += " (no device)";
  }
  return out;
}

// d3d12_adapter / vulkan_device: -1 is automatic; anything else the player (or a test) asked for.
bool PlayerChose(const char* cvar, int* value) {
  *value = rex::cvar::Query<int32_t>(cvar);
  return *value != -1;
}

void Apply(const char* cvar, const char* api_name, const std::vector<Candidate>& list, int chosen) {
  int value = -1;
  if (PlayerChose(cvar, &value)) {
    const Candidate* c = gpu_choice::Find(list, value);
    REXLOG_INFO("rexgpu-native: {} GPU: the player's {} = {} ({}); adapters: {}", api_name, cvar, value,
                c ? c->name : std::string("not listed"), List(list));
    return;
  }
  const Candidate* c = gpu_choice::Find(list, chosen);
  if (!c) {
    REXLOG_WARN("rexgpu-native: {} GPU: no suitable adapter found; the SDK chooses. Adapters: {}", api_name,
                List(list));
    return;
  }
  // The SDK tries index 0 first anyway: only a different choice needs the cvar (single-GPU PCs keep
  // exactly the old path).
  if (chosen > 0) rex::cvar::SetFlagFromCommandLine(cvar, std::to_string(chosen));
  REXLOG_INFO("rexgpu-native: {} GPU: {} (index {}, high performance){}; adapters: {}", api_name, c->name, chosen,
              chosen > 0 ? "" : " (the SDK's default)", List(list));
}

#if NR_HAS_D3D12
std::string Narrow(const wchar_t* w) {
  std::string out;
  for (; *w; ++w) out += *w < 128 ? char(*w) : '?';
  return out;
}

int g_d3d12_choice = -1;

void ChooseD3D12() {
  std::vector<Candidate> list;
  std::vector<LUID> luids;
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    REXLOG_WARN("rexgpu-native: Direct3D 12 GPU: no DXGI factory; the SDK chooses");
    return;
  }
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* a = nullptr;
    if (factory->EnumAdapters1(i, &a) != S_OK) break;
    DXGI_ADAPTER_DESC1 d{};
    if (SUCCEEDED(a->GetDesc1(&d))) {
      Candidate c;
      c.index = int(i);
      c.name = Narrow(d.Description);
      c.local_memory = d.DedicatedVideoMemory;
      c.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || gpu_choice::IsBasicRender(d.VendorId, d.DeviceId, c.name);
      list.push_back(c);
      luids.push_back(d.AdapterLuid);
    }
    a->Release();
  }
  IDXGIFactory6* factory6 = nullptr;
  if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory6)))) {
    for (UINT r = 0;; ++r) {
      IDXGIAdapter1* a = nullptr;
      if (factory6->EnumAdapterByGpuPreference(r, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                               reinterpret_cast<void**>(&a)) != S_OK)
        break;
      DXGI_ADAPTER_DESC1 d{};
      if (SUCCEEDED(a->GetDesc1(&d)))
        for (size_t k = 0; k < luids.size(); ++k)
          if (luids[k].LowPart == d.AdapterLuid.LowPart && luids[k].HighPart == d.AdapterLuid.HighPart &&
              list[k].preference < 0)
            list[k].preference = int(r);
      a->Release();
    }
    factory6->Release();
  }
  // Test only the candidates in choice order (the SDK's provider needs feature level 11_0), so a
  // driver we will not use is not loaded.
  int chosen = -1;
  for (;;) {
    chosen = gpu_choice::Choose(list);
    if (chosen < 0) break;
    IDXGIAdapter1* a = nullptr;
    bool ok = false;
    if (factory->EnumAdapters1(UINT(chosen), &a) == S_OK) {
      ok = SUCCEEDED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
      a->Release();
    }
    if (ok) break;
    for (auto& c : list)
      if (c.index == chosen) c.usable = false;
  }
  factory->Release();
  g_d3d12_choice = chosen;
  Apply("d3d12_adapter", "Direct3D 12", list, chosen);
}
#endif

#if NR_HAS_VULKAN
void ChooseVulkan() {
  // The SDK's own instance class: the same loader and the same device order as VulkanProvider.
  auto instance = rex::ui::vulkan::VulkanInstance::Create(false, false);
  if (!instance) {
    REXLOG_WARN("rexgpu-native: Vulkan GPU: no Vulkan instance; the SDK chooses");
    return;
  }
  const auto& ifn = instance->functions();
  std::vector<VkPhysicalDevice> devices;
  instance->EnumeratePhysicalDevices(devices);
  std::vector<Candidate> list;
  for (size_t i = 0; i < devices.size(); ++i) {
    VkPhysicalDeviceProperties p{};
    ifn.vkGetPhysicalDeviceProperties(devices[i], &p);
    VkPhysicalDeviceMemoryProperties m{};
    ifn.vkGetPhysicalDeviceMemoryProperties(devices[i], &m);
    Candidate c;
    c.index = int(i);
    c.name = p.deviceName;
    switch (p.deviceType) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: c.kind = gpu_choice::Kind::kDiscrete; break;
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: c.kind = gpu_choice::Kind::kIntegrated; break;
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: c.kind = gpu_choice::Kind::kVirtual; break;
      case VK_PHYSICAL_DEVICE_TYPE_CPU: c.kind = gpu_choice::Kind::kCpu; c.software = true; break;
      default: break;
    }
    for (uint32_t h = 0; h < m.memoryHeapCount; ++h)
      if ((m.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && m.memoryHeaps[h].size > c.local_memory)
        c.local_memory = m.memoryHeaps[h].size;
    list.push_back(c);
  }
  Apply("vulkan_device", "Vulkan", list, gpu_choice::Choose(list));
}
#endif

}  // namespace

void PreferHighPerformanceGpu(GpuApi api) {
#if NR_HAS_D3D12
  static std::once_flag d3d12_once;
  if (api == GpuApi::kD3D12) std::call_once(d3d12_once, ChooseD3D12);
#endif
#if NR_HAS_VULKAN
  static std::once_flag vulkan_once;
  if (api == GpuApi::kVulkan) std::call_once(vulkan_once, ChooseVulkan);
#endif
  (void)api;
}

#if NR_HAS_D3D12
IDXGIAdapter1* ChosenD3D12Adapter(std::string* name) {
  PreferHighPerformanceGpu(GpuApi::kD3D12);
  int index = -1;
  if (!PlayerChose("d3d12_adapter", &index)) index = g_d3d12_choice;
  if (index < 0) return nullptr;  // none suitable, or WARP asked for (-2)
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return nullptr;
  IDXGIAdapter1* a = nullptr;
  if (factory->EnumAdapters1(UINT(index), &a) != S_OK) a = nullptr;
  factory->Release();
  if (a && name) {
    DXGI_ADAPTER_DESC1 d{};
    if (SUCCEEDED(a->GetDesc1(&d))) *name = Narrow(d.Description);
  }
  return a;
}
#endif

}  // namespace nr
