// rexgpu-native plugin entry points.
//
// The runtime loads rexgpu-native.dll when the gpu_plugin cvar is "native"
// (--gpu_plugin=native) and asks for a graphics system through rex_gpu_create.
// nr_get_api is the second export: the D3D hooks compiled into king_kong.exe
// (native-renderer/hooks) use it to reach the backend (native_api.h).

#include <filesystem>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include "backend/backend.h"
#include "plugin/launcher_prewarm.h"
#include "plugin/native_api.h"
#include "plugin/native_graphics_system.h"
#include "plugin/native_prewarm.h"
#include "plugin/native_stutter.h"

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
                      "The translated shader pack. Empty: kkshaders-spirv.pack (Vulkan) or "
                      "kkshaders-dxil.pack (D3D12) in <cache root>/native-shaders, where the launcher "
                      "builds it from the game's files, else beside the game (kkshaders db-build), else "
                      "every shader is compiled with DXC on first use")
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

REXCVAR_DEFINE_BOOL(native_pipeline_cache, true, "Native renderer",
                    "Record every pipeline description the game's draws need to a file under the cache root "
                    "and create them all on the worker threads at the next start, so the first visit to a "
                    "level does not stall for the driver's compile (Vulkan; needs native_async_pipelines)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_pipeline_cache_dir, "", "Native renderer",
                      "Folder for the pipeline cache file when native_pipeline_cache is on. Empty: "
                      "<cache root>/native-pipelines")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_vk_pipeline_cache, true, "Native renderer",
                    "Vulkan: keep the driver's compiled pipelines (a VkPipelineCache) in a file under the cache "
                    "root (<cache root>/native-pipelines/<title>.vkcache, or native_pipeline_cache_dir) and load it "
                    "at start, so making the pipelines again is fast. A file another GPU, driver or driver version "
                    "made is not used: the cache is rebuilt in the background and the file replaced")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_launcher_prewarm, false, "Native renderer",
                    "When the launcher opens, make the game's known pipelines (native_pipeline_cache's file and the "
                    "shipped list) on a Vulkan device of the plugin's own while the player is still there. The driver "
                    "keeps what it compiled (its own disk cache, and native_vk_pipeline_cache's file), so the game's "
                    "start makes them again quickly. Stopped when the game starts")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_stutter_stats, false, "Native renderer",
                    "Pipeline stutter counter: frames in which a draw waited for its pipeline or was skipped "
                    "because it was not ready, and the worst wait, in the log (every 300 swaps and at the end) and "
                    "under the frame counter (kk_show_fps, F2)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_pipeline_list, true, "Native renderer",
                    "Load the pipeline list shipped beside the game (kkpipelines.list, made by nr_pipelines merge "
                    "from the pipeline cache files of several runs) at start and create its pipelines on the "
                    "worker threads, so a fresh install draws everything from its first frame (needs "
                    "native_async_pipelines; without the file nothing is loaded). The list is never written")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_pipeline_list_file, "", "Native renderer",
                      "The shipped pipeline list when native_pipeline_list is on. Empty: kkpipelines.list beside "
                      "the game")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_pipeline_wait, false, "Native renderer",
                    "A draw whose pipeline is still being created waits for it (true) instead of being "
                    "skipped for a frame or two (false, the Xenos plugin's choice). Only with "
                    "native_async_pipelines")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_dynamic_vertex_buffers, true, "Native renderer",
                    "Vertex buffers the game rewrites every frame are not watched for writes: each draw "
                    "uploads the vertices it reads (no write fault per page and frame)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_render_thread, true, "Native renderer",
                    "Draw on a thread of the renderer's own, as the Xenos plugin's command processor does: "
                    "the game's thread only copies each call's state, and the guest GPU's fences wait for "
                    "the renderer (the game's timing then follows the Xenos plugin's)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_render_thread_fences, true, "Native renderer",
                    "With native_render_thread: the guest GPU's fences, swaps and interrupts wait until the render "
                    "thread has run the calls queued before them (false: they pass at once, as without the render "
                    "thread; for testing)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(native_render_thread_presents, 1, "Native renderer",
                     "With native_render_thread: how many frames the game may queue ahead of the render thread "
                     "before its Present waits (1 to 4)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_async_present, true, "Native renderer",
                    "Hand each frame to the presenter from a thread of its own, so the guest GPU's command "
                    "stream (and the game waiting on it) does not wait for the presenter")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_async_submit, true, "Native renderer",
                    "Submit each frame's GPU work from a thread of its own, so the game's thread does not "
                    "wait for the queue while the presenter holds it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_present_unlocked, true, "Native renderer",
                    "Hand the game's frame to the presenter without holding the renderer's lock, so the "
                    "game's draws do not wait while the presenter waits for its previous paint")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_DOUBLE(native_pipeline_wait_budget_ms, 30.0, "Native renderer",
                      "Without native_pipeline_wait: milliseconds a frame may wait in all for pipelines "
                      "being created before its draws are skipped (0: skip at once)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_render_scale, "auto", "Native renderer",
                      "Render scale: the game's render targets, viewports, resolves and textures made from "
                      "them are this many times the 360's 1280x720 (1 = the 360's size; 1.5, 2, 3, any number "
                      "from 0.25 to 8), or a frame size such as 1920x1080. The frame handed to the presenter "
                      "is the scaled one; its upscalers and FXAA still apply. Above 1 the pixel shaders are "
                      "the render scale aware ones: build the pack with kkshaders db-build --also-scaled, "
                      "else they are compiled at startup (needs DXC). auto: the port's draw resolution scale "
                      "(draw_resolution_scale_x / _y, which the launcher's render quality presets set, or "
                      "resolution_scale), as the Xenos plugin uses it (the default; 1 = the old default)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_DOUBLE(native_shadow_scale, 0.0, "Native renderer",
                      "Render scale of the shadow maps (k_32_FLOAT targets that are not the frame's size, "
                      "832x832 in the game). 0: like native_render_scale; 1: keep them at the 360's size")
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

REXCVAR_DEFINE_STRING(native_show_texture, "", "Native renderer",
                      "Debugging: show the texture a resolve wrote at this guest address (hex, the fetch "
                      "constant's base) instead of the frame")
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

// The launcher's prewarm (native_prewarm.h).
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t nr_prewarm_start(const NrPrewarmRequest* request) {
  if (!request || request->size < sizeof(NrPrewarmRequest) || request->version != NR_PREWARM_VERSION) {
    REXLOG_ERROR("rexgpu-native: nr_prewarm_start: a request of another version");
    return NR_PREWARM_OFF;
  }
  const char* root = request->cache_root ? request->cache_root : "";
  return nr::StartLauncherPrewarm(std::filesystem::path(reinterpret_cast<const char8_t*>(root)), request->title_id);
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t nr_prewarm_status(NrPrewarmStatus* status) {
  return nr::LauncherPrewarmStatus(status);
}

extern "C" REX_GPU_PLUGIN_EXPORT void nr_prewarm_stop(void) { nr::StopLauncherPrewarm(); }

// The pipeline stutter counter for the overlay (native_stutter.h).
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t nr_stutter_stats(NrStutterStats* stats) {
  nr::StutterSnapshot s;
  if (!stats || stats->size < sizeof(NrStutterStats) || !nr::ReadStutterSnapshot(&s)) return 0;
  stats->frames = s.frames;
  stats->stutter_frames = s.stutter_frames;
  stats->frames_waited = s.frames_waited;
  stats->frames_skipped = s.frames_skipped;
  stats->draws_skipped = s.draws_skipped;
  stats->worst_wait_frame = s.worst_wait_frame;
  stats->wait_ms = s.wait_ms;
  stats->worst_wait_ms = s.worst_wait_ms;
  return 1;
}
