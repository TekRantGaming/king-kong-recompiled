#pragma once
// Every device the plugin makes (the game's provider, the launcher's shader setup and prewarm, the
// upscaler question) on the same high-performance GPU (task 203; selection order in
// backend/gpu_choice.h).

#include <string>

#if NR_HAS_D3D12
struct IDXGIAdapter1;
#endif

namespace nr {

enum class GpuApi { kD3D12, kVulkan };

// Once per process and API, before an SDK provider is made: unless the player set d3d12_adapter /
// vulkan_device (config, command line or environment), point it at the high-performance GPU. The
// value is applied as a command-line value so the launcher never writes it to the settings file
// (adapter numbers change when a GPU is added or removed). Logs the choice.
void PreferHighPerformanceGpu(GpuApi api);

#if NR_HAS_D3D12
// The adapter the SDK's D3D12 provider will use (the choice above, or the player's), AddRef'd, for
// code that makes its own D3D12 device. nullptr when none; `name` gets its description.
IDXGIAdapter1* ChosenD3D12Adapter(std::string* name);
#endif

}  // namespace nr
