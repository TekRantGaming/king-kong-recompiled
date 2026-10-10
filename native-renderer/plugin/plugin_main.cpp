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

REXCVAR_DEFINE_BOOL(native_validation, false, "Native renderer",
                    "Wrap the NVRHI device in NVRHI's validation layer (slow; for debugging)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_wvp_transpose, false, "Native renderer",
                    "Placeholder draws: read vertex shader constants c0..c3 as the columns of "
                    "the world-view-projection matrix instead of its rows")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_all_targets, false, "Native renderer",
                    "Placeholder draws: also draw render-to-texture passes into the frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_main_pass_only, true, "Native renderer",
                    "Placeholder draws: draw only depth-tested draws that write depth or are opaque "
                    "(the main pass); off also draws full-screen passes, particles and the HUD, "
                    "which cover the frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_game, true, "Native renderer",
                    "Draw the game with its own shaders, textures and states (Phase 2); off: the "
                    "milestone 3 placeholder pipeline (flat-coloured silhouettes)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_shader_pack, "", "Native renderer",
                      "The translated shader pack (kkshaders db-build). Empty: kkshaders-spirv.pack "
                      "(Vulkan) or kkshaders-dxil.pack (D3D12), else kkshaders.pack, beside the game")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_dxc, "", "Native renderer",
                      "DXC (dxcompiler) folder or library, for shaders missing from the pack. Empty: "
                      "beside the game, then the system's search path")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_element_endian, true, "Native renderer",
                    "Read each vertex element with its declaration's endian field (on: what the "
                    "game's frames need) instead of the vertex buffer's")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_async_pipelines, true, "Native renderer",
                    "Vulkan: create pipelines on a worker thread and skip a draw until its pipeline "
                    "is ready, instead of stalling the game for the driver's compile")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_flip_front_face, false, "Native renderer",
                    "Swap the front face of every draw (a debugging aid for culling)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_texture_tail_mips, false, "Native renderer",
                    "Small textures whose whole mip chain sits in the base level's packed tail (bound with "
                    "mip address 0): read the smaller levels from that tail. Off: level 0 only, as the SDK")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(native_debug, 0, "Native renderer",
                     "Debugging aids (bits): 1 the game's clears are green, 2 the frame image is "
                     "magenta under the back buffer")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(native_dump_frame, -1, "Native renderer",
                     "Log every clear, draw, resolve and present the hooks see in this game "
                     "frame (counted from the first Present; -1 for none)");

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
