// rexgpu-native plugin entry points.
//
// The runtime loads rexgpu-native.dll when the gpu_plugin cvar is "native"
// (--gpu_plugin=native) and asks for a graphics system through rex_gpu_create.
// nr_get_api is the second export: the D3D hooks compiled into king_kong.exe
// (native-renderer/hooks) use it to reach the backend (native_api.h).

#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include "plugin/native_api.h"
#include "plugin/native_graphics_system.h"

REXCVAR_DEFINE_STRING(native_backend, "auto", "Native renderer",
                      "Host API for the native renderer: auto, d3d12 or vulkan")
    .allowed({"auto", "d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_test, "triangle", "Native renderer",
                      "Test picture shown when the game's draws are not routed to the native "
                      "renderer: clear (a colour that changes each frame) or triangle")
    .allowed({"clear", "triangle"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_draws, true, "Native renderer",
                    "Draw the game's DrawIndexedVertices / DrawVertices calls through the "
                    "placeholder pipeline (needs the hooks built into the game)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_log_packets, false, "Native renderer",
                    "Log the GPU packets the ring skimmer sees (very noisy)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-native: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-native: invalid GpuCreateInfo");
    return nullptr;
  }
  // The host passes "any"; the native_backend cvar narrows it.
  std::string_view backend = info->backend ? info->backend : "any";
  std::string wanted = REXCVAR_GET(native_backend);
  if (wanted != "auto") {
    backend = wanted;
  }
  auto* system = nr::NativeGraphicsSystem::Create(backend);
  if (!system) {
    REXLOG_ERROR("rexgpu-native: no usable host API for backend '{}'", backend);
  }
  return system;
}

extern "C" REX_GPU_PLUGIN_EXPORT const NrApi* nr_get_api(void) {
  return nr::NativeGraphicsSystem::GetApi();
}
