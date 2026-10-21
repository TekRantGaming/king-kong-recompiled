// Choosing the upscaler (upscaler.h).

#include "backend/upscaler.h"

#include <atomic>
#include <mutex>
#include <vector>

#include "backend/log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if NR_HAS_D3D12
#include <d3d12.h>
#include <dxgi1_4.h>
#endif
#if NR_HAS_VULKAN
#include <vulkan/vulkan_core.h>

#include "backend/vulkan_dispatch.h"
#endif

namespace nr {

namespace {
std::atomic<UpscalerGate> g_gate{nullptr};  // constant-initialised (task 353)
std::mutex g_idle_mutex;
nvrhi::IDevice* g_idle_device = nullptr;
std::function<void()> g_idle_wait;
UpscalerExclusive g_exclusive;
nvrhi::IDevice* g_exclusive_device = nullptr;
thread_local bool t_exclusive = false;  // inside UpscalerRunExclusive: the locks are held
}  // namespace

void SetUpscalerGate(UpscalerGate gate) { g_gate.store(gate); }

bool UpscalerGateAllows(UpscalerKind kind, std::string* why) {
  const UpscalerGate gate = g_gate.load();
  return !gate || gate(kind, why);
}

void SetUpscalerExclusive(nvrhi::IDevice* device, UpscalerExclusive run) {
  std::lock_guard lock(g_idle_mutex);
  g_exclusive_device = run ? device : nullptr;
  g_exclusive = std::move(run);
}

void UpscalerRunExclusive(nvrhi::IDevice* device, const std::function<void()>& fn) {
  UpscalerExclusive run;
  {
    std::lock_guard lock(g_idle_mutex);
    if (device && device == g_exclusive_device) run = g_exclusive;
  }
  if (run && !t_exclusive) {
    run([&] {
      t_exclusive = true;
      fn();
      t_exclusive = false;
    });
    return;
  }
  if (device && !t_exclusive) device->waitForIdle();
  fn();
}

void SetUpscalerIdleWait(nvrhi::IDevice* device, std::function<void()> wait) {
  std::lock_guard lock(g_idle_mutex);
  g_idle_device = wait ? device : nullptr;
  g_idle_wait = std::move(wait);
}

void UpscalerWaitForIdle(nvrhi::IDevice* device) {
  std::function<void()> wait;
  {
    std::lock_guard lock(g_idle_mutex);
    if (device && device == g_idle_device && !t_exclusive) wait = g_idle_wait;
  }
  if (wait) wait();
  else if (device) device->waitForIdle();
}

std::wstring UpscalerDllFolder() {
#if defined(_WIN32)
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  std::wstring s(path, n);
  const size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
#else
  return {};
#endif
}

uint32_t UpscalerDeviceVendor(nvrhi::IDevice* device) {
  if (!device) return 0;
#if NR_HAS_VULKAN
  if (device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN) {
    auto physical = static_cast<VkPhysicalDevice>(device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice).pointer);
    auto instance = static_cast<VkInstance>(device->getNativeObject(nvrhi::ObjectTypes::VK_Instance).pointer);
    PFN_vkGetInstanceProcAddr gipa = VulkanGetInstanceProcAddr();
    if (!physical || !instance || !gipa) return 0;
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    if (!props) return 0;
    VkPhysicalDeviceProperties p{};
    props(physical, &p);
    return p.vendorID;
  }
#endif
#if NR_HAS_D3D12
  if (device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12) {
    auto* d3d = static_cast<ID3D12Device*>(device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device).pointer);
    if (!d3d) return 0;
    const LUID luid = d3d->GetAdapterLuid();
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) return 0;
    uint32_t vendor = 0;
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter)))) {
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc))) vendor = desc.VendorId;
      adapter->Release();
    }
    factory->Release();
    return vendor;
  }
#endif
  return 0;
}

namespace {

const char* VendorName(uint32_t v) {
  switch (v) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    default: return "other";
  }
}

}  // namespace

UpscalerSupport QueryUpscalerSupport(nvrhi::IDevice* device) {
  UpscalerSupport s;
  s.vendor = UpscalerDeviceVendor(device);
  UpscaleConfig probe;
  probe.quality = UpscaleQuality::kQuality;
  std::string why;
  auto note = [&](const char* name, bool ok) {
    if (!ok) s.notes += std::string(s.notes.empty() ? "" : "; ") + name + ": " + why;
    why.clear();
  };
  s.fsr3 = CreateFsr3Upscaler(device, probe, &why) != nullptr;
  note("FSR 3.1", s.fsr3);
  s.fsr4 = CreateFsr4Upscaler(device, probe, &why) != nullptr;
  note("FSR 4", s.fsr4);
  s.dlss = CreateDlssUpscaler(device, probe, &why) != nullptr;
  note("DLSS", s.dlss);
  s.xess = CreateXessUpscaler(device, probe, &why) != nullptr;
  note("XeSS", s.xess);
  return s;
}

std::unique_ptr<Upscaler> CreateUpscaler(nvrhi::IDevice* device, const UpscaleConfig& config, UpscalerKind* chosen,
                                         std::string* why) {
  std::string reasons;
  auto add = [&](const char* name, const std::string& w) {
    reasons += std::string(reasons.empty() ? "" : "; ") + name + " unavailable: " + w;
  };
  std::vector<UpscalerKind> order;
  const uint32_t vendor = UpscalerDeviceVendor(device);
  switch (config.kind) {
    case UpscalerKind::kAuto:
      if (vendor == 0x10DE) order = {UpscalerKind::kDlss, UpscalerKind::kFsr3};
      else if (vendor == 0x1002) order = {UpscalerKind::kFsr4, UpscalerKind::kFsr3};
      else if (vendor == 0x8086) order = {UpscalerKind::kXess, UpscalerKind::kFsr3};
      else order = {UpscalerKind::kFsr3};
      break;
    case UpscalerKind::kFsr4: order = {UpscalerKind::kFsr4, UpscalerKind::kFsr3}; break;
    case UpscalerKind::kDlss: order = {UpscalerKind::kDlss, UpscalerKind::kFsr3}; break;
    case UpscalerKind::kXess: order = {UpscalerKind::kXess, UpscalerKind::kFsr3}; break;
    case UpscalerKind::kFsr3: order = {UpscalerKind::kFsr3}; break;
    default: break;
  }
  for (UpscalerKind k : order) {
    std::string w;
    std::unique_ptr<Upscaler> u;
    switch (k) {
      case UpscalerKind::kFsr3: u = CreateFsr3Upscaler(device, config, &w); break;
      case UpscalerKind::kFsr4: u = CreateFsr4Upscaler(device, config, &w); break;
      case UpscalerKind::kDlss: u = CreateDlssUpscaler(device, config, &w); break;
      case UpscalerKind::kXess: u = CreateXessUpscaler(device, config, &w); break;
      default: break;
    }
    if (u) {
      if (chosen) *chosen = k;
      if (why) {
        *why = std::string("GPU vendor ") + VendorName(vendor) + (reasons.empty() ? "" : "; " + reasons);
      }
      return u;
    }
    add(UpscalerKindName(k), w);
  }
  if (chosen) *chosen = config.kind == UpscalerKind::kBilinear ? UpscalerKind::kBilinear : UpscalerKind::kOff;
  if (why) *why = std::string("GPU vendor ") + VendorName(vendor) + (reasons.empty() ? "" : "; " + reasons);
  return nullptr;
}

}  // namespace nr
