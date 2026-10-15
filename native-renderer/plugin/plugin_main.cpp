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
#include "plugin/native_startup.h"
#include "plugin/native_graphics_system.h"
#include "plugin/native_prewarm.h"
#include "plugin/native_stutter.h"
#include "plugin/native_effect_stats.h"
#include "plugin/upscaler_query.h"

// Shared graphics settings are owned here; the emulation plugin is never loaded.
REXCVAR_DEFINE_BOOL(vsync, false, "GPU", "Enable vertical sync");
REXCVAR_DEFINE_STRING(swap_post_effect, "fxaa", "GPU", "Anti-aliasing: none, fxaa, smaa (SMAA 1x), fxaa_extreme")
    .allowed({"none", "fxaa", "smaa", "fxaa_extreme"});
REXCVAR_DEFINE_INT32(anisotropic_override, 3, "GPU", "Texture filtering override (-1 = game default)").range(-1, 5);
REXCVAR_DEFINE_INT32(draw_resolution_scale_x, 1, "GPU", "Draw resolution scale X").range(1, 8);
REXCVAR_DEFINE_INT32(draw_resolution_scale_y, 1, "GPU", "Draw resolution scale Y").range(1, 8);
REXCVAR_DEFINE_INT32(resolution_scale, 1, "GPU", "Draw resolution scale").range(1, 8);
REXCVAR_DEFINE_INT32(ao_mode, 0, "GPU", "Ambient occlusion: 0 off, 1 on, 2 AO only").range(0, 2);
REXCVAR_DEFINE_INT32(ao_strength, 3, "GPU", "Ambient occlusion strength: 1 Light, 2 Medium, 3 Strong").range(1, 3);

REXCVAR_DEFINE_STRING(native_backend, "auto", "Native renderer",
                      "Host API for the native renderer: auto, d3d12 or vulkan")
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

REXCVAR_DEFINE_STRING(native_failed_backends, "", "Native renderer",
                      "Set by the game's startup recovery: the graphics APIs (vulkan, d3d12, comma separated) that "
                      "failed to start on this PC. Setup does not prepare shaders or pipelines for them")
    .debug_only();

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

REXCVAR_DEFINE_BOOL(native_diagnostics, false, "Native renderer",
                    "Collect detailed native renderer call timings, slowest frames and ring telemetry")
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

REXCVAR_DEFINE_BOOL(native_pipeline_wait, true, "Native renderer",
                    "A draw whose pipeline is still being created waits for it (true) instead of being "
                    "skipped for a frame or two (false). Waiting prevents first-use surfaces from flashing or vanishing. "
                    "Only with "
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

REXCVAR_DEFINE_STRING(native_frame_wait, "submit", "Native renderer",
                      "Where the renderer keeps the GPU at most two frames behind: submit (wait for frame N-2 "
                      "just before frame N is submitted, so frame N is recorded meanwhile) or open (wait before "
                      "frame N is recorded, the first Beta 4 candidate's behaviour)")
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

// Ambient occlusion beyond the launcher's ao_mode and ao_strength (docs/ao.md). The defaults are
// today's AO.
REXCVAR_DEFINE_STRING(native_ao_method, "legacy", "Native renderer",
                      "Ambient occlusion method: legacy (the occlusion ported from the Xenos plugin) or gtao "
                      "(XeGTAO from the scene's depth). ao_mode and ao_strength still turn it on and set its strength")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_ao_apply, "resolve", "Native renderer",
                      "When the gtao method's occlusion reaches the picture: resolve (at the scene copy after the "
                      "depth resolve, today's moment), opaque (at the end of the opaque pass, before water, smoke, "
                      "particles and fog), ambient (inside the lighting shaders of native_ao_shaders, opaque "
                      "otherwise). The legacy method always uses resolve")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_ao_quality, "auto", "Native renderer",
                      "The gtao method's resolution: full or half (of the scene target, at the render scale), "
                      "or auto (full up to a render scale of 1.5, half above)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(native_ao_temporal, true, "Native renderer",
                    "GTAO noise smoothing: blend each pixel's occlusion with last frame's (or let the temporal "
                    "upscaler do it). Off shows the raw, grainier occlusion. Applied live");
REXCVAR_DEFINE_INT32(native_ao_samples, 2, "Native renderer",
                     "GTAO sample quality: 1 low (2 slices x 2 steps), 2 medium (3 x 3, the previous setting), "
                     "3 high (9 x 3). Applied live, independently of native_ao_quality's resolution");

REXCVAR_DEFINE_DOUBLE(native_ao_radius, 0.0, "Native renderer",
                      "The gtao method's occlusion radius in the game's world units; 0: from the camera (the "
                      "legacy method's radius for the strength, in near-plane distances)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_ao_rules, "", "Native renderer",
                      "A file of draw classifier rules placed before the built-in ones (docs/ao.md)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_ao_shaders, "", "Native renderer",
                      "The ambient-only mode's list of lighting pixel shaders and their ambient term (docs/ao.md)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_DOUBLE(native_ao_radius_scale, 1.0, "Native renderer",
                      "The gtao method's radius from the camera times this (tuning, docs/ao.md)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_DOUBLE(native_ao_falloff, 0.0, "Native renderer",
                      "The gtao method's falloff range, 0 to 1 of the radius (0: XeGTAO's 0.615)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_DOUBLE(native_ao_power, 0.0, "Native renderer",
                      "The gtao method's final power (0: from ao_strength)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_DOUBLE(native_ao_fade, 0.0, "Native renderer",
                      "The gtao method's occlusion fades out linearly to this many near-plane distances (0: none)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(native_ao_dump_every, 0, "Native renderer",
                     "Every this many frames, log the AO classifier's view of each draw (0: off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_ao_gpu_time, false, "Native renderer",
                    "Time the ambient occlusion's GPU passes and log the average every 600 frames")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Screen-space global illumination (docs/gi.md). Off by default; the launcher's Graphics page sets
// native_gi ("Global illumination").
REXCVAR_DEFINE_BOOL(native_tessellation, false, "Native renderer", "Experimental Ann Phong tessellation for supported material parts")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(native_tessellation_smoothness, 0.75, "Native renderer", "Ann tessellation smoothness, 0 to 1")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(native_tessellation_detail, 4, "Native renderer", "Ann tessellation detail, 1 to 16")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(native_gi, "off", "Native renderer",
                      "Screen-space global illumination (one bounce of light from the surfaces on the screen, "
                      "added at the end of the opaque pass): off, low, medium or high")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(native_gi_intensity, 1.0, "Native renderer",
                      "Global illumination strength (1: normal calibrated bounce, 4: strong)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(native_gi_quality, "auto", "Native renderer",
                      "The global illumination's working resolution: auto (low and medium half, high full up to a "
                      "render scale of 1.5), half or full")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(native_gi_radius, 0.0, "Native renderer",
                      "How far the global illumination gathers light from, in world units (0: the level's)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_DOUBLE(native_gi_thickness, 0.0, "Native renderer",
                      "How deep the global illumination takes a surface on the screen to be, in world units (0: a "
                      "fifth of the radius, 0.25 to 1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_DOUBLE(native_gi_albedo, 0.75, "Native renderer",
                      "The brightest surface colour the global illumination's composite assumes (0.05 to 1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_DOUBLE(native_gi_occlusion, 0.0, "Native renderer",
                      "With ambient occlusion off: how much of the flat ambient light the global illumination "
                      "takes away where surfaces hide the sky (0 to 1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(native_gi_debug, 0, "Native renderer",
                     "Global illumination debug view: 0 off, 1 the bounce light alone, 2 the change on grey, 3 the "
                     "visibility")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_gi_temporal, true, "Native renderer",
                    "Global illumination: blend in last frame's light where the camera saw the same surface (the "
                    "camera's motion voted from the scene's draws)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_gi_gpu_time, false, "Native renderer",
                    "Time the global illumination's GPU passes and log the average every 600 frames")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Temporal upscalers (docs/upscalers.md). Off by default: nothing of them runs.
REXCVAR_DEFINE_STRING(native_upscaler, "auto", "Native renderer",
                      "Temporal upscaler: off, auto (DLSS on NVIDIA RTX, FSR 4 on supported AMD, XeSS on Intel, "
                      "FSR 3.1 otherwise), fsr3, fsr4, dlss, xess (bilinear: a plain stretch, for comparisons). "
                      "The output is native_render_scale's size. auto until the game sets its own default (the "
                      "best one this PC runs) once its settings are read. Applied live with its mode, DLSS preset, "
                      "sharpness, mip bias and reactive mask: the upscaler is made again at a frame boundary")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(native_upscaler_quality, "quality", "Native renderer",
                      "The upscaler's mode: native (anti-aliasing at the output resolution: DLAA, FSR native AA, "
                      "XeSS native AA), quality, balanced, performance, ultra_performance")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(native_upscaler_point, "post", "Native renderer",
                      "Where the upscaler runs: post (before the game's post effects and HUD) or present (at the "
                      "end of the frame, everything upscaled)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_upscaler_dlss_preset, "latest", "Native renderer",
                      "DLSS's render preset: latest (the newest model: DLSS 4.5's M, L for ultra performance), "
                      "default (the DLL's choice per mode), J or K (DLSS 4 transformer), L or M (DLSS 4.5)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(native_upscaler_sharpness, -1.0, "Native renderer",
                      "Sharpening after the upscale, 0 to 1 (FSR's RCAS; DLSS and XeSS have none); negative: off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(native_upscaler_mip_bias, "auto", "Native renderer",
                      "The texture LOD bias of the scene's draws: auto (log2 of input over output, as the SDKs "
                      "recommend) or a number")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(native_upscaler_reactive, 1.0, "Native renderer",
                      "Strength of the reactive mask (what changed after the opaque pass: water, fur, smoke, rain, "
                      "particles), 0 to 1; 0: no mask")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(native_upscaler_debug, 0, "Native renderer",
                     "Upscaler debugging bits: 1 show the motion vectors, 2 the reactive mask, 4 camera motion only, "
                     "8 no jitter, 16 a log line per frame, 32 show the depth, 64 no mip bias, 128 log the camera vote "
                     "and the constants that changed, 512 read the motion vectors back every 150 frames and log them, "
                     "1024 draws on rewritten vertex buffers get the camera's motion only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_upscaler_gpu_time, false, "Native renderer",
                    "Time the upscaler's GPU work and log the average every 600 frames")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(native_upscaler_rules, "", "Native renderer",
                      "A file of upscale point rules: 'post ps=<hash>' starts the post chain, 'scene ps=<hash>' never")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The lighting options (docs/lighting.md). Every default is the game's own picture; the Original
// look keeps them there.
REXCVAR_DEFINE_STRING(native_shadow_quality, "original", "Native renderer",
                      "Shadows: original (the game's 832x832 shadow maps, hard edges), high (shadow maps at twice "
                      "the resolution, soft edges from a wide rotated filter) or ultra (four times the resolution, "
                      "edges that soften with the distance to what casts the shadow). native_shadow_scale, when "
                      "set, still sets the shadow maps' resolution")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(native_water_reflections, "original", "Native renderer",
                      "Water reflections: original (the game's planar reflection and environment colour) or ssr "
                      "(screen-space reflections of the scene, the game's own where a ray finds nothing)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(native_volumetrics, "off", "Native renderer",
                      "Light shafts: off (the game's own), low or high (volumetric light from the sun's shadow map "
                      "added where the game has light shafts or fog)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(native_shadow_softness, 1.0, "Native renderer",
                      "The soft shadows' filter size times this (tuning, docs/lighting.md)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(native_ssr_strength, 1.0, "Native renderer",
                      "The water's screen-space reflections' strength times this (tuning, docs/lighting.md)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(native_volumetric_strength, 1.0, "Native renderer",
                      "The volumetric light's strength times this (tuning, docs/lighting.md)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(native_lighting_census, 0, "Native renderer",
                     "Every this many frames, log the frame's draws with the lighting passes' targets, textures "
                     "and constants (0: off; docs/lighting.md)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_lighting_gpu_time, false, "Native renderer",
                    "Time the lighting options' GPU passes and log the averages every 600 frames")
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
                       "magenta under the back buffer, 32768 residency/upload/frame GPU counters, "
                       "524288 GPU pass batches (every fourth frame)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(native_vk_small_tables, false, "Native renderer",
                    "Force reduced Vulkan descriptor tables for hardware testing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_vk_minimum_tables, false, "Native renderer",
                    "Force minimum Vulkan descriptor tables and stress GPU-completed recycling")
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
    nr::ReportStartupFailure("could not create a supported graphics API");
  }
  return system;
}

extern "C" REX_GPU_PLUGIN_EXPORT void nr_set_startup_failure_handler(nr::StartupFailureHandler handler) {
  nr::startup_failure_handler.store(handler, std::memory_order_release);
}

extern "C" REX_GPU_PLUGIN_EXPORT void nr_set_startup_presented_handler(nr::StartupPresentedHandler handler) {
  nr::startup_presented_handler.store(handler, std::memory_order_release);
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
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t nr_effect_stats(NrEffectStats* stats) {
#if NR_GAME_RENDERER
  return nr::ReadEffectStats(stats) ? 1u : 0u;
#else
  return 0;
#endif
}

// The launcher's upscaler menu (native_upscaler_query.h).
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t nr_upscaler_query(NrUpscalerSupport* out) { return nr::UpscalerQuery(out); }

extern "C" REX_GPU_PLUGIN_EXPORT void nr_upscaler_query_wait(void) { nr::WaitUpscalerQuery(); }

// The graphics panel's view of the same answer: never starts the question (the game's device exists).
extern "C" REX_GPU_PLUGIN_EXPORT uint32_t nr_upscaler_query_peek(NrUpscalerSupport* out) {
  return nr::UpscalerQueryPeek(out);
}

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
