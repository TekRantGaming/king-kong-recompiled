# Temporal upscalers

The native renderer can render the scene at a lower resolution and have a temporal upscaler rebuild it at
the output size (or anti-alias it at the same size): NVIDIA DLSS Super Resolution and DLAA, AMD FSR 4 and
FSR 3.1, Intel XeSS Super Resolution. No frame generation. Off by default: with `--native_upscaler=off`
nothing below runs and nothing changes.

The code: `backend/upscale_*.{h,cpp}` (settings, jitter, motion matching, the upscale point; no GPU),
`backend/game_upscale.{h,cpp}` (the frame plumbing in the game renderer), `backend/shaders/upscale.hlsl`
(the camera motion, reactive mask, depth and stretch passes), `shaders/src/motion.cpp` (the vertex shaders'
motion variant), `backend/upscaler*.cpp` (one file per SDK behind `backend/upscaler.h`).

## Settings

All are read at start (init only).

| Cvar | Default | Meaning |
|---|---|---|
| `native_upscaler` | `off` | `off`, `auto`, `fsr3`, `fsr4`, `dlss`, `xess` (`bilinear`: a plain stretch, for comparisons) |
| `native_upscaler_quality` | `quality` | `native` (anti-aliasing at the output size: DLAA, FSR native AA, XeSS AA), `quality` (1.5x a side), `balanced` (1.7x), `performance` (2x), `ultra_performance` (3x) |
| `native_upscaler_point` | `post` | `post`: before the game's post effects and HUD; `present`: at the end of the frame (everything upscaled, HUD included) |
| `native_upscaler_dlss_preset` | `default` | DLSS render preset: `default` (the DLL's choice per mode), `J` or `K` (DLSS 4, transformer), `L` or `M` (DLSS 4.5) |
| `native_upscaler_sharpness` | `-1` | 0 to 1: FSR's RCAS after the upscale (DLSS and XeSS have none); negative: off |
| `native_upscaler_mip_bias` | `auto` | texture LOD bias of the scene's draws: `auto` = log2(input / output), as the SDKs recommend, or a number |
| `native_upscaler_reactive` | `1.0` | strength of the reactive mask (0: none) |
| `native_upscaler_debug` | `0` | bits: 1 show the motion vectors, 2 the reactive mask, 4 camera motion only, 8 no jitter, 16 a log line per frame (and NGX's log), 32 show the depth, 64 no mip bias, 128 log the camera vote and which constants changed, 512 read the motion vectors back every 150 frames and log how many pixels move and how far |
| `native_upscaler_gpu_time` | `false` | time the upscaler's GPU work (the renderer's passes and the SDK's) and log the averages every 600 frames |
| `native_upscaler_rules` | empty | a file of upscale point rules: `post ps=<hash>` starts the post chain, `scene ps=<hash>` never does |

The output size is `native_render_scale`'s; the input size is the output divided by the mode's ratio (as
each SDK reports it for that output size).

### Who gets what

| GPU | `auto` picks | Also offered | Not offered (falls back to FSR 3.1, the reason in the log) |
|---|---|---|---|
| NVIDIA RTX (20 series and later) | DLSS | FSR 3.1, XeSS | FSR 4 |
| NVIDIA GTX | FSR 3.1 | XeSS (where the driver has the shader features) | DLSS (NGX says no), FSR 4 |
| AMD Radeon RX 9000 / RX 7000 on D3D12 | FSR 4 | FSR 3.1, XeSS | DLSS |
| AMD on Vulkan, older AMD | FSR 3.1 | XeSS | FSR 4 (AMD's FSR SDK 2.3 has no Vulkan; FSR 4.1 needs RDNA 3 or 4), DLSS |
| Intel Arc | XeSS | FSR 3.1 | DLSS, FSR 4 |
| Anything else | FSR 3.1 | XeSS where it runs | |

Each upscaler checks at start whether it can run (its DLL beside the game, the vendor, what the SDK
itself says about this GPU and driver) and `CreateUpscaler` falls back: FSR 4, DLSS and XeSS to FSR 3.1,
FSR 3.1 to the plain stretch. `QueryUpscalerSupport` gives the same answers for a settings menu.

## The launcher's menu

**Display** > **Upscaling** > **Upscaler** lists, after Off, AMD FSR 1 and NVIDIA NIS (the presenter's
spatial upscalers, `present_effect`), the temporal ones this PC runs: NVIDIA DLSS, AMD FSR 4, AMD FSR 3.1,
Intel XeSS (`native_upscaler`), only with the native renderer. DLSS is named in text only: no NVIDIA logo
or artwork (see "What a release ships"). No frame generation anywhere.

- **Which are offered.** When the launcher opens it asks the plugin (`nr_upscaler_query`,
  `plugin/native_upscaler_query.h`; `nr::kk::QueryUpscalers`, `hooks/kk_native_ui.h`). The plugin makes a
  Direct3D 12 device of its own on the high-performance adapter, on a thread, and runs
  `QueryUpscalerSupport` on it: each SDK says whether it runs on this graphics card and driver (DLSS: NGX's
  `SuperSampling.Available`; FSR 4: AMD's library's versions; XeSS: its context; FSR 3.1: its DLL). When
  the game draws with Vulkan (the default), FSR 4 is left out (no Vulkan in AMD's FSR SDK), and DLSS and
  XeSS stay only with an SDK that takes the device requests (`sdk-patches/0003`). Until the answer is in
  the row says it is checking. A Vulkan device is not used for the question: vulkan.hpp's dispatcher is one
  for the process, and the launcher's prewarm may have a device of its own. So on Vulkan the answer is the
  same card and driver's on Direct3D 12; the game still checks for itself when it starts and falls back to
  FSR 3.1 (the log says why). The game's start (`NativeGraphicsSystem::CreateProvider`) and the launcher's
  restart and Quit wait for the thread. An upscaler chosen on another PC that this one cannot run stays
  listed as "(not on this PC)".
- **What a choice sets.** A temporal upscaler sets `present_effect` to `bilinear` (no FSR 1 on top),
  `native_upscaler` to its value and `kk_render_quality` to `native`: the upscaler's output is the screen's
  size in the game's 720p steps (`ApplyRenderPreset` uses Native whenever a temporal upscaler is on with
  the native renderer), and the upscaler draws fewer pixels itself. **Upscaler mode** then sets
  `native_upscaler_quality`: Native AA (DLAA with DLSS), Quality, Balanced, Performance, Ultra Performance,
  with the input and output sizes shown. Render quality and Internal resolution are hidden meanwhile. FSR 1
  and NIS keep their rows as before.
- **Restart.** `native_upscaler` is one of the launcher's restart settings: on Vulkan the device must be
  made with DLSS's and XeSS's extensions, and it exists before the launcher opens. Play restarts the game
  when it changed.
- **Defaults.** Off: `native_upscaler` defaults to `off`; the graphics presets set FSR 1 or NIS and turn a
  temporal upscaler off (a preset is matched only with none on); the **Original** look sets
  `native_upscaler` off with its other settings (`OriginalLookSettings`), and the row is disabled there.
  Test runs that want an upscaler over the Original look add `--kk_original_look_keep=native_upscaler`.

## How a frame goes

With an upscaler on, a frame with a scene has two parts (`UpscaleFrameTracker`, `upscale_frame.h`):

1. **The scene phase.** The frame-size render targets are the *input generation*: the upscaler's input
   size. Every depth-tested draw into them is jittered (below), samples its textures with the mip bias, and
   a draw that writes the scene's depth is recorded for the motion pass with its vertex constants.
   Shadow maps and other targets that are not frame-sized keep their own sizes.
2. **The upscale point**, the first of: a post effect's pixel shader (19 of the game's post-effect shaders
   plus the two rain shaders, `kDefaultUpscaleRules`), a draw or clear that covers only part of the frame
   target (the post chain's quarter-size corners), a resolve with clear of the scene target after the
   opaque pass, or Present. There `RunUpscale` (game_upscale.cpp) makes the camera motion, the objects'
   motion, the reactive mask, runs the upscaler, puts the result into the *output generation*'s scene
   target, carries the depth over (upsampled, so the heat shimmer's and the HUD's depth tests still work),
   and makes again from the upscaled picture any full-frame copy of the scene the game took since its last
   draw (the post effects read those).
3. **The post phase.** The frame-size targets are the output generation (a second set of targets at the
   output size, keyed apart from the input ones): the post effects, the HUD and Present at the output size,
   nothing jittered. A target first used there whose picture is still in its input twin gets it stretched
   over (`MigrateTarget`). A frame without a scene (menus, movies, the launcher) is drawn at the output size
   from its start.

`native_upscaler_point=present` puts the point at Present: the post effects and the HUD are drawn at the
input size and upscaled with the scene (cheaper, softer HUD).

The legacy ambient occlusion and the one applied at the resolve moment need the scene at one size from
start to end, so with an upscaler on the AO is GTAO applied to the opaque pass (`backend.cpp`).

## Jitter

A Halton (2, 3) sequence, `JitterPhaseCount` = ceil(8 x ratio_x x ratio_y) phases (at least 8), as the SDKs'
guides ask. The offset is added to the draw constants' `ndcOffset` (the translated vertex shaders add
`ndcOffset * w` to the clip position), for depth-tested draws into frame-size targets in the scene phase
only: the shadow maps, the post effects and the HUD are never jittered. On Vulkan the translated shaders
negate y after the offset, so the y offset has the other sign there.

The convention handed to every SDK: the jitter is the picture's shift in input pixels, +x right, +y down
(FSR's `jitterOffset`, DLSS's `InJitterOffset`, XeSS's `jitterOffset` all mean that). Checked both ways:
with either sign flipped every upscaler's result gets worse (nr_game_upscale_tests on Vulkan, by hand;
nr_upscaler_sdk_tests on both APIs, as a check).

The mip bias (auto) is log2(input / output): -0.585 in quality mode, -1 in performance.

## Motion vectors

The SDKs want, for each input pixel, where its surface was in the last frame minus where it is now, in input
pixels, without the jitter (FSR: `motionVectorScale` 1; DLSS: `MVLowRes`, scale 1; XeSS: low-res motion
vectors, scale 1). An R16G16_FLOAT texture at the input size, made in two passes:

1. **The camera.** From the depth: each pixel is moved back to clip space, through last frame's camera
   and onto the screen. The game gives no view or projection matrix of its own, so the camera's change is
   found from the draws: each recorded draw that is matched with its draw in the last frame (below) offers
   R = WVP_prev x WVP_cur^-1 from its world-view-projection constants, and the most common R among the
   static objects wins (`CameraReprojectionVote`: candidates agreeing within 2e-3 are averaged). Static
   world, sky and everything that does not move on its own is right from this pass alone.
2. **The objects.** Every recorded draw that wrote the scene's depth is drawn again into the motion texture
   with its vertex shader's *motion variant* (`MotionShader`, translator.h): the same shader run twice, with
   this frame's constants and with last frame's (bound at b5, `kMotionConstantsRegister`), the pixel getting
   the difference of the two positions. Depth test EQUAL against the scene's depth, so only what the
   picture shows is written. This carries everything the vertex shader does: skinning (the bone palette is in
   the constants), Kong's and the characters' animation, moving platforms, vehicles.

**Matching a draw with its last-frame self** (`MotionHistory`): a draw is keyed by its vertex shader, vertex
buffers (both streams), offset, index buffer, first index, count, base vertex and primitive type. Draws with
the same key in one frame are matched by order when last frame had as many, else by the closest constants.
A draw with no match (first frame on screen) keeps the camera's motion.

**Coverage** where players look:

| What | Motion from |
|---|---|
| Static world, terrain, sky | camera pass |
| Characters (Jack, Ann, Carl), dinosaurs, Kong's body | object pass (skinned in the vertex shader) |
| Kong's fur | the fur shells that write depth get the object pass; blended shells show the motion of the body under them, and the reactive mask covers what they change |
| Particles, rain, sparks, smoke | their vertex buffers are rewritten every frame (no last-frame vertices): camera motion of what is behind them, and the reactive mask (they are drawn after the opaque pass) |
| Water | object or camera pass for the surface, the reactive mask for its blended layers |
| Heat shimmer, post effects, HUD | after the upscale point: not upscaled through history at all |

`native_upscaler_debug=1` shows the motion vectors over the picture (colour = direction, brightness =
length), `=4` uses the camera pass alone (a moving object then smears: the comparison shows what the object
pass adds).

## Reactive mask

Where the picture changed after the opaque pass (blended water, fur, smoke, rain, particles), the upscaler
should trust the current frame more. A copy of the scene is taken when the opaque pass ends (before the draw
that ends it, or after the fog pass when the fog ends it); at the upscale point the mask is
saturate(2 x strength x max channel |final - copy|), capped at 0.9 (R8 at the input size). It goes to FSR as
its reactive mask, to DLSS as its bias-current-colour mask, to XeSS as its responsive pixel mask (XeSS clamps
it at 0.8 itself).

## Exposure and colour

The game draws gamma-encoded LDR colour (8 bits a channel). FSR gets `FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE`
and `FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB`, DLSS no HDR flag (LDR), XeSS `XESS_INIT_FLAG_LDR_INPUT_COLOR`;
pre-exposure and exposure 1, no exposure texture. Depth is 0 near, 1 far (not inverted), copied to R32_FLOAT.

## The SDKs

| | FSR 3.1 | FSR 4 | DLSS | XeSS |
|---|---|---|---|---|
| SDK | AMD FidelityFX SDK 1.1.4 (FSR 3.1.4) | AMD FSR SDK 2.3.0 (FSR 4.1.1) | NVIDIA DLSS SDK 310.9.1 | Intel XeSS SDK 3.0.2 (XeSS SR 2.0.2) |
| APIs | Vulkan, D3D12 | D3D12 | Vulkan, D3D12 | Vulkan, D3D12 |
| Loaded | DLL at run time | DLL at run time | NGX loader linked in (nvsdk_ngx_d.lib), which loads nvngx_dlss.dll | DLL at run time |
| Headers | vendored (thirdparty/FidelityFX-API, MIT) | vendored (thirdparty/FSR-SDK-API, MIT) | from the SDK folder at build time | from the SDK folder (types only) |
| Code | upscaler_ffx.cpp | upscaler_fsr4.cpp | upscaler_dlss.cpp | upscaler_xess.cpp |
| Ratios | 1, 1.5, 1.7, 2, 3 | same | NGX's optimal settings (1, 1.5, 1.72, 2, 3) | XeSS's legacy factors (1, 1.5, 1.7, 2, 3) |

Every SDK records into the game renderer's open command list: the inputs are put in shader-read state and
the output in unordered-access state through NVRHI, the SDK records its work into the native command
buffer, and NVRHI's cached state is cleared afterwards (the SDKs bind their own pipelines, descriptor sets
and, on D3D12, descriptor heaps).

- **FSR 3.1** runs everywhere. Its Vulkan backend looks some functions up by their extension names and calls
  them unchecked; `FfxGetDeviceProcAddr` gives the core function when the extension name is not there.
- **FSR 4** asks AMD's library which upscaler versions this device can run and takes the newest FSR 4. On
  NVIDIA it reports "FSR 4 does not run on this GPU (the AMD library offers FSR 3.1.5, 2.3.4)" and the choice
  falls back to FSR 3.1. Untested on AMD hardware (none here).
- **DLSS** starts NGX once per device with a project ID for a custom engine (the game has no NVIDIA
  application ID), checks `SuperSampling.Available` (an RTX GPU; a recent enough driver, else it names the
  version needed) and takes the input size from NGX's optimal settings. `native` is DLAA. The preset goes to
  every mode's hint (`DLSS.Hint.Render.Preset.*`).
- **XeSS** makes its context when chosen and builds its pipelines in the background; the first upscale
  initialises it for the output size (a blocking call). It runs on non-Intel GPUs through DP4a.

### Vulkan: extensions and features

DLSS and XeSS work on the game's own Vulkan device, which the SDK creates with a fixed list of extensions.
`sdk-patches/0003-Vulkan-let-libraries-ask-for-instance-and-device-ext.patch` lets a library ask for more
before the instance and the device are made (`SetVulkanInstanceExtensionRequests`,
`SetVulkanDeviceRequestCallback`; requested features are enabled only where supported). The plugin asks
(`PrepareVulkanForUpscalers`, native_graphics_system.cpp) when the upscaler setting may need it: `dlss`,
`xess`, or `auto` (DLSS's needs on NVIDIA, XeSS's on Intel). With `off` or `fsr3` nothing is asked for.
`upscaler_vulkan.h` gathers the lists from the SDKs themselves:

- DLSS (NGX): instance `VK_KHR_get_physical_device_properties2`; device `VK_NVX_binary_import`,
  `VK_NVX_image_view_handle`, `VK_KHR_buffer_device_address` (and the `bufferDeviceAddress` feature),
  `VK_KHR_push_descriptor`.
- XeSS: device `VK_EXT_mutable_descriptor_type` and the features `xessVKGetRequiredDeviceFeatures` returns;
  the structures Vulkan 1.2 and 1.3 cover (float16/int8, integer dot product, subgroup size control, ...) are
  folded into those, so the device's chain never holds both.

Without the patch (an SDK built without it) the plugin cannot ask: DLSS and XeSS then fail to start on
Vulkan and fall back to FSR 3.1; D3D12 is unaffected. The SDK must therefore be rebuilt with every patch in
`sdk-patches/` (docs/backend.md, "Building and testing on Linux", has the loop; on Windows the same with the
`win-amd64` preset).

## Building

The SDKs are found under `tools/upscaler-sdks/` (git-ignored: junctions or copies of the SDKs as
downloaded), each optional; one that is missing is left out and reported "not built in".

| Folder | Cache variable | What is used |
|---|---|---|
| `tools/upscaler-sdks/fsr3` | `NR_FFX_SDK_DIR` | the FidelityFX SDK 1.1.4: `PrebuiltSignedDLL/amd_fidelityfx_vk.dll`, `amd_fidelityfx_dx12.dll` |
| `tools/upscaler-sdks/fsr4` | `NR_FSR4_SDK_DIR` | the FSR SDK 2.3.0's `Kits/FidelityFX`: `signedbin/amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` |
| `tools/upscaler-sdks/dlss` | `NR_DLSS_SDK_DIR` | the DLSS SDK: `include/`, `lib/Windows_x86_64/x64/nvsdk_ngx_d.lib`, `lib/Windows_x86_64/rel/nvngx_dlss.dll` |
| `tools/upscaler-sdks/xess` | `NR_XESS_SDK_DIR` | the XeSS SDK: `inc/xess/`, `bin/libxess.dll` |

Official sources: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (releases v1.1.4 and the FSR
SDK v2.3.0), https://github.com/NVIDIA/DLSS (v310.9.1), https://github.com/intel/xess (v3.0.2). The build
copies the runtime files beside the game (`NR_UPSCALER_RUNTIME_FILES`, plugin/CMakeLists.txt) and beside the
upscaler tests.

## What a release ships

The launcher's updater installs files beside `king_kong.exe` only (plus `glyphs/` and `launcher_art/`), and
the renderer loads every upscaler file from the executable's folder. A release with the upscalers puts these
six files at the top level, beside `king_kong.exe`:

| File | Upscaler | From | Licence and what it asks |
|---|---|---|---|
| `amd_fidelityfx_vk.dll` | FSR 3.1 (Vulkan) | FidelityFX SDK 1.1.4, PrebuiltSignedDLL | MIT: the notice in THIRD-PARTY-NOTICES |
| `amd_fidelityfx_dx12.dll` | FSR 3.1 (D3D12) | FidelityFX SDK 1.1.4, PrebuiltSignedDLL | MIT: the notice in THIRD-PARTY-NOTICES |
| `amd_fidelityfx_loader_dx12.dll` | FSR 4 (D3D12) | FSR SDK 2.3.0, signedbin | MIT (the SDK's licence lists it): the notice in THIRD-PARTY-NOTICES |
| `amd_fidelityfx_upscaler_dx12.dll` | FSR 4 (D3D12) | FSR SDK 2.3.0, signedbin | MIT (the SDK's licence lists it): the notice in THIRD-PARTY-NOTICES |
| `nvngx_dlss.dll` | DLSS | DLSS SDK 310.9.1, lib/Windows_x86_64/rel | NVIDIA RTX SDKs License: object code only, unmodified; see below |
| `libxess.dll` | XeSS | XeSS SDK 3.0.2, bin | Intel Simplified Software License: unmodified, its notice and terms reproduced (THIRD-PARTY-NOTICES has them and the SDK's third-party-programs list) |

Not needed: the SDKs' frame generation, denoiser and radiance cache DLLs, `libxess_dx11.dll`,
`libxess_fg.dll`, `libxell.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll`, the `dev` DLSS DLL (it draws a
watermark). `kk/THIRD-PARTY-NOTICES.txt` already carries all four SDKs' notices and must ship as now.

NVIDIA's licence asks more of the project than a notice; these are for the maintainers, not the code:

- The licence's supplement (7.1 b) asks applications using the DLSS SDK to attribute it and show the NVIDIA
  marks on splash screens, in the about box (the launcher's) and in a game's credits; its trademark terms
  (7.2 c) ask for a sample of each use of the marks to be sent to NVIDIA for approval two weeks before use.
  Until that is settled, mention DLSS in text only and use no NVIDIA logos.
- NVIDIA must be notified before a commercial release that uses DLSS (supplement, section 4: the form at
  https://developer.nvidia.com/sw-notification). This release is free, but notifying costs nothing.
- The application must have material functionality beyond the SDK (it does), may not say it is endorsed by
  NVIDIA, and the SDK may not be redistributed on its own (only `nvngx_dlss.dll` beside the game).

## Tests

- `nr_upscale_tests` (no GPU): settings parsing, Halton and phase counts, the jitter's clip offset on both
  APIs, the mip bias, motion matching, the camera vote, the upscale point's rules.
- `nr_motion_shader_tests`: the motion variant of all 395 vertex shaders of the translator's corpus compiles.
- `nr_game_upscale_tests` (Vulkan, through the game renderer with synthetic guest memory): upscaler off and
  the native mode without jitter give the very same frame; input and output sizes per mode; the jitter
  moves the input picture by the right amount; the motion vectors of a static floor under a moving camera and
  of a moving box (within 0.06 px of the analytic values); camera-only gets the box wrong; the reactive mask
  marks water and a particle but not the opaque floor and wall; the upscale point lands before the post pass;
  then FSR 3.1, DLSS (default and preset M) and XeSS for real: on this PC's RTX 4070 Ti the mean
  difference to the native picture is FSR 3.1 0.0007, DLSS 0.0004, XeSS 0.0007, the plain stretch 0.0009.
- `nr_upscaler_sdk_tests` (the SDKs alone, on a D3D12 device and a Vulkan device of its own): a still
  synthetic picture sampled at the jittered positions, 32 frames, against the picture supersampled at the
  output size. RTX 4070 Ti, the same on both APIs: FSR 3.1 0.0224 (jitter flipped 0.0328), DLSS 0.0138
  (0.0149), XeSS 0.0196 (0.1115), bilinear stretch 0.0319. FSR 4 falls back to FSR 3.1 with its reason (on
  Vulkan: "FSR 4 needs Direct3D 12"). With `NR_UPSCALER_COST=1` it also times each SDK (below).

## Cost

Each SDK's own upscale to 2560x1440 (the game at render scale 2), GPU timer queries around the SDK's
work alone, the median of 60 frames (`nr_upscaler_sdk_tests`, `NR_UPSCALER_COST=1`), RTX 4070 Ti, D3D12 /
Vulkan in ms:

| Mode (input) | FSR 3.1 | DLSS 310.9.1 | XeSS 2.0.2 |
|---|---|---|---|
| native (2560x1440) | 1.07 / 1.57 (noisy: 0.8 at best) | 1.06 / 1.14 (DLAA, preset K) | 1.06 / 1.08 |
| quality (1707x960) | 0.56 / 0.50 | 0.95 / 0.99 (K) | 0.99 / 1.00 |
| balanced (1506x847; DLSS 1485x835) | 0.46 / 0.47 | 0.93 / 0.96 (K) | 0.98 / 0.99 |
| performance (1280x720) | 0.42 / 0.42 | 1.15 / 1.18 (M, the DLSS 4.5 model) | 0.95 / 0.96 |
| ultra_performance (853x480) | 0.36 / 0.36 | 0.94 / 0.94 (L) | 0.96 / 0.96 |

DLSS's default presets per mode are the DLL's (K for DLAA, quality and balanced, M for performance, L for
ultra performance), which is why performance costs more than quality. The renderer's own passes (the camera
and object motion, the reactive mask, the depth carried over) come on top; `native_upscaler_gpu_time` logs
both in the game.

## Open problems

- FSR 4 is untested on hardware that runs it (an RDNA 3 or 4 GPU on D3D12).
- The stencil is not carried over the upscale point (only depth); a draw after the point that tests the
  stencil the scene wrote would see it cleared. None found in the frames logged so far.
- Particles and other per-frame vertex buffers have camera motion only; the reactive mask carries them.
- Objects first seen this frame and draws whose key changes every frame (vertex buffers reallocated) get
  camera motion only.
- The legacy and resolve-moment AO cannot run with an upscaler (GTAO replaces them).
- The launcher has no upscaler choice yet: the setting is the cvar (`QueryUpscalerSupport` is there for a
  menu that offers only what this GPU runs).

## Status (12 October 2026, paused)

Done: the foundation, FSR 3.1, DLSS, XeSS and FSR 4 in the renderer and the plugin, the SDK patch, the
notices, the offline tests above on this PC's RTX 4070 Ti (both APIs) and the SDKs' GPU cost.

Not done yet (paused when the PC was needed; nothing of it has run in the game):

- The game runs, through the fair queue: `--native_upscaler=off` against the golden `vrex_140`; at
  `--native_render_scale=2`, quality mode, V-Rex and Kong to the Rescue for native 2x, DLSS, FSR 3.1 and XeSS
  (one comparison sheet each in `F:\KK-native-renderer\analysis\upscalers\`, with the in-game GPU time from
  `--native_upscaler_gpu_time`); the motion vectors over Kong to the Rescue (`--native_upscaler_debug=1`) for
  the camera, the characters, Kong's fur and the particles; a D3D12 start with DLSS and with FSR 4 (its
  fallback). The scene file is `analysis\upscalers\runs\upscaler_scenes.json`; the build folder has the shader
  packs and DXC beside `king_kong.exe`.
- The launcher's upscaler menu on the Graphics page, offering only what `QueryUpscalerSupport` says this GPU
  runs, DLSS named in text only.
