# Stream 04: the backend and the `rexgpu-native` plugin

Status, 10 October 2026: milestones 1, 2 and 3 run in the game on Windows, on Vulkan (the primary API, built
against a Vulkan-enabled SDK from source) and on D3D12, proven with the game's own frame captures; see
"Windows: status and how it was proven" below. The cloud session (branch `cloud-04-backend`) built and
verified the pieces on Linux without the game first.

## Layout

| Path | What | Depends on |
|---|---|---|
| `CMakeLists.txt`, `cmake/nvrhi.cmake` | standalone project; kk includes it with `KK_NATIVE_RENDERER=ON` | |
| `backend/draw_state.*`, `draw_sink.h` | the draw-state tracker: D3D-level state from the hooked calls, one record per clear / draw / resolve / present | nothing |
| `backend/api_binding.*` | the `NrApi` table (`plugin/native_api.h`) bound to a tracker; the plugin and the tests use the same code | nothing |
| `backend/guest_layout.h`, `guest_memory.h` | guest object layouts (vertex / index buffer, declaration, viewport), big-endian helpers | nothing |
| `backend/primitive.*` | strips, fans, quads, base vertex, reset index to host triangle lists | nothing |
| `backend/renderer.*`, `shaders.*`, `shaders/` | the NVRHI renderer: frame images, the milestone pictures, the placeholder pipeline for game draws | NVRHI |
| `backend/vulkan_dispatch.*` | the vulkan.hpp dispatcher NVRHI's static Vulkan backend needs | Vulkan-Headers |
| `backend/host_device.h`, `vulkan_host.cpp`, `d3d12_host.cpp`, `backend.*` | NVRHI on the SDK provider's device, frame hand-over to the presenter | SDK |
| `hooks/hook_table.*` | the 39 hooked entry points, their argument mapping, the nesting rule | nothing |
| `hooks/trace_replay.*` | replays `KK_DEV_D3D_TRACE` logs through the hook table | nothing |
| `hooks/kk_native_hooks.cpp` | the game-side `REX_HOOK_RAW` wrappers (linked into king_kong) | SDK, the game's code |
| `plugin/` | `rexgpu-native`: `IGraphicsSystem`, ring skimmer, vblank, exports | SDK |
| `sdk-patches/` | SDK changes the plugin needs (apply after `tools/rexglue-patches`) | |
| `backend/tests/` | tests and the standalone test host | |

## How the pieces connect

```
engine -> sub_8211xxxx (hooked, kk_native_hooks.cpp)
            |-- hooks::Run: forward if the plugin is active (nested draws not forwarded)
            |     -> hook table handler: r3..r10 / f1 -> NrApi call
            |          -> (plugin) ApiBinding -> DrawTracker -> Renderer (NVRHI) -> command list
            '-- the game's original (__imp__sub_...): the library keeps running
the library's packets -> ring skimmer (fences, interrupts, VdSwap) -> Backend::Present -> presenter
```

At the game's Present the renderer closes and submits the frame's command list. At the guest's swap the
backend copies the last submitted frame into the presenter's guest output; if no game frame has arrived yet
(hooks not built in, or `--native_draws=false`), it draws the test picture instead (until the Windows runs it
also did so whenever a swap came before its frame's Present, which flickered the test picture into the game's
frames)
(`--native_test=clear` for milestone 1, `triangle` for milestone 2).

## Decisions (made in the cloud session; revisit freely)

- **The game's library keeps running.** Hooks forward and then call the original. The device struct, fences
  and swaps stay correct, and the ring skimmer drains the packets. Replacing originals is Phase 2.
- **Nesting.** The library calls its own hooked functions (SetRenderTarget applies the viewport through
  SetViewport; Present resolves). Nested state calls are forwarded (they are real state changes, such as the
  viewport reset); nested draws, clears, resolves and presents are not.
- **Render states** come from hooking the setter-table functions (`sub_82109788` and the rest in
  `hook_table.cpp`), one handler per state since the setter does not receive the state number. The device's
  register images would also work while the originals run; hooking is what Phase 2 needs anyway.
- **Guest data passed by pointer** (constants, viewports) is copied at call time; buffers and declarations
  are read at draw time.
- **Object layouts** follow the brief 03 draft's census (`nr-03-textures`, `resources/include/kknr/
  buffers.h`): vertex fetch constant at +12, index address +12 / size +16, bit 31 of Common = 32-bit indices,
  declaration count at +8 and elements from +36. `d3d-api-map.md` says the resource type is in Common bits
  13-15, the census says 16-18; nothing here depends on it.
- **Placeholder pipeline (milestone 3).** Every draw is expanded to a triangle list on the CPU (base vertex
  applied, indices rebased), its vertices are copied raw (big-endian) into a per-frame ring, and the vertex
  shader byte-swaps the position (three floats at the declaration's POSITION offset) and multiplies by a
  matrix used as rows (`mul(wvp, pos)`, which is what the guest's `dp4 oPos.x, v0, c0` computes).
  Colour: a hash of the (vertex shader, pixel shader) pair.
- **Which matrix (Windows, from the game's frames).** c0..c3 is the world-view-projection only for the
  static world; skinned meshes (strides 32 and 56) keep other rows there and their projection at c36, c48,
  c68, c72, c108 or c120. The tracker keeps all 256 vertex constants and the draw takes the first four
  consecutive registers that look like a perspective matrix (`LooksLikePerspectiveRows`: the z row a
  multiple 0.8-1.25 of the w row, the w row not affine). Skinned meshes are therefore drawn in their bind
  pose at about the right place and size, not animated. `--native_wvp_transpose=true` uses c0..c3 as
  columns.
- **Which draws (Windows).** Placeholder draws are the main pass only (`--native_main_pass_only`, default
  on): depth-tested draws (compare function other than always) that write depth or are opaque
  (RB_BLENDCONTROL0 colour part ONE, ADD, ZERO). The game draws a depth pre-pass, resolves, then the colour
  pass with depth writes off; full-screen post effects, the fog, particles (sprites the vertex shader
  expands) and the HUD would otherwise cover the frame with flat colour. Positions that are not 32-bit
  floats are skipped (none seen). Each frame image has a D32 depth buffer, cleared at the frame start and by
  the game's depth clears; the pipelines follow the draw's depth test, write and compare function.
- **Only the main surface is drawn.** Render target 0 at the last Present is the frame's surface; draws to
  other targets (shadow maps, post effects) are skipped until there is a render-target pool. The first frame
  draws everything. `--native_all_targets=true` draws them all.
- **Push constants** for the placeholder's per-draw data (96 bytes; root constants on D3D12).
- **Shaders** are compiled by DXC (Linux release 1.8.2505.1, with `libdxil.so`, so the DXIL is signed) to
  both DXIL and SPIR-V; `compile_shaders.sh` / `.ps1`; the headers are checked in.
- **Clear's stencil argument** is r9 (`hooks::kClearStencilRegister`): confirmed, `sub_82115418` passes r9
  on as the stencil (r8) of the clear path `sub_82114D10`.
- **Clear flags** (confirmed from `sub_82114D10`: `clrlwi 28`, `rlwinm 0,27,27`, `rlwinm 0,26,26`): bits 0-3
  render targets 0-3, 0x10 Z, 0x20 stencil. The cloud guess (bit 1 = Z) made the colour-only clear before
  the colour pass wipe the depth pre-pass.
- **The shader setters are the other way round from `d3d-api-map.md`**: `sub_821108B8` is SetPixelShader and
  `sub_82110C28` SetVertexShader. Proof (frame dump, `--native_dump_frame`): every object passed to
  `sub_82110C28` has the 592-byte header with container flags 0x102A0E01 at +592 and is never null; every
  object passed to `sub_821108B8` has the 52-byte header with 0x102A0E00 at +52 and is null in the depth
  pre-pass. With the shader stream's finding (the vs_3_0 container at 0x8203E800 goes to `sub_82111D90`),
  the D3D map's create functions and setters are each swapped, and so are the header sizes in
  `d3d-structs.md` (the vertex shader object has the 592-byte header). The hook table is fixed.
- **Vertex buffer address**: the fetch constant at +12 holds a CPU physical-view address like the index
  buffer's, so it goes through `CpuToPhysical` (the 0xE0000000 view is 4 KB ahead); masking read every
  vertex 4 KB early and drew large random triangles.
- **NrApi version 2**: adds `set_render_state` and `set_sampler_state`. The hooks refuse a plugin of another
  version (logged, originals only).

## NVRHI needs more from the SDK's Vulkan device

NVRHI is a Vulkan 1.3 library: its queues use timeline semaphores, its render passes dynamic rendering and its
barriers `vkCmdPipelineBarrier2`. The SDK (v0.10.0) enables `dynamicRendering` (with GPU emulation) but not
`timelineSemaphore` or `synchronization2`. Found by running the plugin's Vulkan host on the SDK's own
`VulkanProvider` under the Khronos validation layer: the pixels came out right on lavapipe, with 15
validation errors (`VUID-VkSemaphoreTypeCreateInfo-timelineSemaphore-03252`,
`VUID-vkCmdPipelineBarrier2-synchronization2-03848`). A real driver may not forgive that.

`sdk-patches/0001-Vulkan-enable-timelineSemaphore-and-synchronization2.patch` enables both whenever
supported (harmless for the SDK's own code), adds them to `VulkanDevice::Properties` and defines
`REX_UI_VULKAN_DEVICE_HAS_SYNC_FEATURES`; the plugin then checks them at start and refuses a device without
them. It applies cleanly on v0.10.0 after `tools/rexglue-patches`. D3D12 is not affected.

Verified: v0.10.0 + this patch built from source on Linux (clang-20, the `linux-amd64` preset's
`-march=x86-64-v2`, C++23), then `nr_plugin_tests` against that install: all checks pass, zero validation
errors. Against the official bundle the same test fails on the validation errors, by design. A Linux build of
the game with this plugin therefore needs an SDK built from source with the patch
(`REXGLUE_SDK_DIR` in `tools/build_appimage.sh`).

## Building and testing on Linux

Packages: `cmake ninja-build g++` (or clang-20), `libvulkan-dev mesa-vulkan-drivers vulkan-validationlayers
spirv-tools`, and for the plugin `libx11-dev libx11-xcb-dev libwayland-dev libxkbcommon-dev`.

```
unzip rexglue-sdk-0.10.0-linux-amd64.zip -d /opt/rexsdk      # or a patched source build's install
cmake -S native-renderer -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_PREFIX_PATH=/opt/rexsdk/linux-amd64
cmake --build build
ctest --test-dir build --output-on-failure
```

Without the SDK on the prefix path the plugin is skipped and the libraries and GPU tests still build
(Vulkan-Headers 1.3.318 or newer are needed: the bundle's, or `-DNR_VULKAN_HEADERS=<dir>`).

| Test | Checks |
|---|---|
| `nr_draw_tests` | the hook table (sorted, complete, matches `kk_native_hooks.cpp`), log parsing, the per-draw sequence from `d3d-api-map.md` replayed as a trace and every field of the resulting records, constants copied at call time, every render / sampler state setter, nesting, an inactive or absent plugin, dropped draws, primitive expansion, the decoders |
| `nr_render_tests` | the test host: its own Vulkan 1.3 device on lavapipe, NVRHI on it (plus NVRHI's and Khronos' validation, which must stay silent). M1: clear colour read back on every pixel; the per-frame colour. M2: the triangle's centroid, vertex colours (orientation), outside pixels, coverage. M3: the recorded frame through hook table, NrApi, tracker and renderer: what reached NVRHI (pipeline, framebuffer, viewport, index buffer, draw arguments, push constants) and the pixels (triangle placed by c0..c3 with the translation, the render-to-texture draw skipped on frame 2); the viewport; the transpose option |
| `nr_plugin_tests` | with the SDK: the runtime's `LoadGpuPlugin` loads `librexgpu-native*.so`, `nr_get_api` is found as the hooks find it and is inactive before setup; `d3d12` is refused; NVRHI on the SDK's own `VulkanProvider` (headless, lavapipe) renders M1, M2 and the M3 frame correctly. Run under the Khronos layer; fails on any validation error, so it passes only with the SDK patch |
| `nr_spirv_val` | the checked-in SPIR-V validates for Vulkan 1.2 |

`NR_TEST_IMAGES=<dir>` makes `nr_render_tests` write the read-back images as PPM files.
`nr_trace_replay <log>` replays a `KK_DEV_D3D_TRACE` log and prints per-frame counts.

## Open questions

Settled on Windows: Clear's stencil register (r9) and flag bits (above); the recompiled code's indirect calls
through the render-state setter table do reach the `REX_HOOK_RAW` overrides (the draws carry the depth states
the game sets, 1/1/3, 1/0/3, 0/0/3 and so on); c0..c3 is the world-view-projection for the static world only
(above); `d3d12_host.cpp` compiles and works.

Still open:

- Skinned meshes are drawn in their bind pose (the placeholder does no skinning), so characters are at about
  the right place but not posed. Real vertex shaders (stream 02's translator and packs) replace this.
- The Venture opening (the ship's hull close up) comes out black with the main-pass filter: its hull draws
  are not drawn (no perspective matrix found, or not counted as main pass). Look at a `--native_dump_frame`
  of that scene.
- V-Rex (and the pause menu over it) shows one large shape over most of the frame: probably geometry close
  to the camera (the cave roof) or positions that the real vertex shader scales or offsets with other
  constants. A `--native_dump_frame` of that scene, or the real shaders, settles it.
- The launcher preloads `rexgpu-native.dll` into every run of a native-enabled build (for its cvars); since
  f83d351 the hooks check `gpu_plugin` and stay disconnected unless it is `native`.
- The presenter wraps are cached by resource pointer and size (D3D12) or image and version (Vulkan); a freed
  and reallocated resource at the same address and size would reuse a stale NVRHI handle.
- Stream 03's resolve paths into a k_8 texture and an 8:8:8:8 with endian none need a frame comparison once
  render targets are pooled.

## Windows: status and how it was proven

Work done on the Windows PC on 10 October 2026, branch `nr-04-backend` (worktree
`F:\KK-native-renderer\wt\04-backend`). Run folders with logs and captures: `F:\KK-native-renderer\analysis\04\runs\`
and `F:\KK-native-renderer\runs\` (harness), never in git.

### Builds

| Preset | SDK | Graphics | Output |
|---|---|---|---|
| `kk-dev` (with `-DKK_NATIVE_RENDERER=ON` set once in its cache) | `tools/rexglue` (the shared bundle) | D3D12 only | `kk/out/build/kk-dev` |
| `kk-dev-vulkan` (new; native renderer on) | `tools/rexglue-vulkan`, a junction to `F:\KK-native-renderer\sdk-vulkan` | Vulkan and D3D12 | `kk/out/build/kk-dev-vulkan` |

`tools/rexglue-vulkan` is a local junction (git-ignored, like `tools/rexglue`); to recreate it:
`mklink /J tools\rexglue-vulkan F:\KK-native-renderer\sdk-vulkan`. Build with `kk\build.bat kk-dev-vulkan`.

First-compile fixes on Windows: NVRHI's D3D12 backend needs the preview DirectX-Headers (v1.717.0-preview, now
vendored in `thirdparty/DirectX-Headers`, see `thirdparty/NOTES.md`; the shim that forwarded to the Windows SDK
is gone); the tracker class is `DrawTracker` (windows.h defines `DrawState` as `DrawStateA`); the plugin is
copied beside the game after every plugin build (a POST_BUILD step on the game's target left a stale copy
when only the plugin changed). `d3d12_host.cpp` and `vulkan_host.cpp` compiled and worked unchanged.

### The Vulkan-enabled SDK

`F:\KK-native-renderer\sdk-src`: rexglue-sdk tag v0.10.0 (f5337cdc) with its submodules, `tools/rexglue-patches`
0001-0012 applied with `git am` in order, then `sdk-patches/0001` (timelineSemaphore, synchronization2); HEAD
471c628, version string 0.10.0.12-dev.g471c628. Configured with the user's `build_win.bat` recipe (preset
`win-amd64`, VS 2022 Build Tools clang) plus `-DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=ON
-DCMAKE_INSTALL_PREFIX=F:/KK-native-renderer/sdk-vulkan/win-amd64`, built Release and installed (about 20
minutes, no source changes). One snag: git on Windows checks symbolic links out as small text files, so
libmspack's `cabextract/mspack/*` (and two MoltenVK headers, unused on Windows) were replaced by copies of
their targets before building. The install registered itself in the CMake user package registry
(`HKCU\Software\Kitware\CMake\Packages\rexglue`, value `56c053aa55b612f5f5de379a820459d8`, next to the user's own
`C:/rexsrc/out/install/win-amd64`). kk's presets set `CMAKE_PREFIX_PATH`, so they are not affected, but the
user may want to delete that value.

The plugin takes Vulkan whenever the SDK has it (`--native_backend=auto`); `--native_backend=d3d12` forces
D3D12 in the Vulkan build. No validation layer is installed on this PC, so the run has not been checked
under the Khronos layer on Windows (the Linux test does that); NVRHI's own layer is `--native_validation`.

### Milestones, proven with the game's own captures

Launch: `king_kong.exe --gpu_plugin=native --game_data_root=<kk/assets> --user_data_root=<private copy of
F:\KK-native-renderer\userdata> --cache_root=F:/KK-native-renderer/cache --kk_launcher=false --kk_frame_rate=30
--fullscreen=false --window_width=1280 --window_height=720`, `KK_DEV_AUTOSKIP=1`, `KK_DEV_SHOTS`
(+ `KK_DEV_SHOTS_FROM=boot`); the scenes through `tests/run.ps1 -Plugin native [-Exe <vulkan build>]`.

| Milestone | D3D12 (`kk-dev`) | Vulkan (`kk-dev-vulkan`) |
|---|---|---|
| 1: `--native_draws=false --native_test=clear` | shots at 5, 6, 12, 30 s: one uniform colour each, changing ((123,217,47), (251,89,81), (4,166,174)); the game runs at 30 FPS to gameplay | the same colours at the same times; log `rexgpu-native: created (Vulkan)` |
| 2: `--native_test=triangle` | the RGB triangle over the cycling colour, green vertex at the top | the identical picture |
| 3: default | log `native renderer: D3D hooks connected to rexgpu-native (39 entry points)`; the Kong to the Rescue cutscene shows Kong, the V-Rex, trees and the ground as flat silhouettes where the golden frame has them | identical frames |

The first in-game run hung after one frame: the library's fences are type 0 writes to SCRATCH_REG0-7 that the
GPU mirrors to memory at SCRATCH_ADDR, and the skimmer did not mirror them (a WAIT_REG_MEM on that memory never
matched). The skimmer now does what the SDK's `CommandProcessor::WriteRegister` does (scratch writeback,
COHER_STATUS_HOST marked pending) and logs any WAIT_REG_MEM stuck for 2 s.

Logging aids (all off unless set): `--native_dump_frame=N` logs every clear, draw (shader object kinds, matrix
register, first vertex and its clip position, blend, depth states), resolve and present of game frame N
(frames count Presents, about 30 a second); the backend logs the hook and renderer counts every 300 swaps
(`rexgpu-native: swap ...`, in the core category, since the app keeps only warnings of the gpu one);
`--native_log_packets=true` logs every PM4 packet with its first data words.

### Xenos unchanged

- `tests/run.ps1 -Plugin xenos` with the native-enabled `kk-dev` build (D3D12 bundle, commit d42804d): 13/13
  scenes pass against the golden set (`F:\KK-native-renderer\runs\xenos-20261010-070111`), the same as the
  golden self-check in `testing.md`.
- No `--gpu_plugin` flag (the app picks xenos): the title frame at 9 s equals the golden one (MAE 0.0), and
  since f83d351 the hooks no longer connect to the preloaded native plugin in such runs.
- `KK_NATIVE_RENDERER=OFF` (`cmake --preset kk-dev -B out/build/kk-dev-off -DKK_NATIVE_RENDERER=OFF`, commit
  6db14b3, no plugin built, `dev_d3d_trace.cpp` in): 12/13 in the full run
  (`F:\KK-native-renderer\runs\xenos-20261010-121738`); venture failed on 2/5 frames (MAE 50, its timeline
  drifted) and passed 5/5 when run again alone (`xenos-20261010-122554`). The Venture scene is timing-sensitive
  for Xenos too.

### Test harness baseline (expected to fail)

`tests/run.ps1 -Plugin native` at commit 6db14b3, all 13 scenes. Vulkan:
`F:\KK-native-renderer\runs\native-20261010-073756` (`-Exe kk\out\build\kk-dev-vulkan\king_kong.exe`); D3D12:
`F:\KK-native-renderer\runs\native-20261010-075125`. Worst frame of each scene: share of pixels outside the
golden range and MAE (0-255).

| Scene | Vulkan | D3D12 | What the native frames show |
|---|---|---|---|
| video | pass 0.0000 / 0.000 | pass 0.0000 / 0.000 | black (the movie is not drawn); passes only because the golden burst holds black frames |
| title | fail 0.1255 / 11.3 | fail 0.1255 / 11.3 | black: the title is 2D, not main pass |
| save_menu | fail 0.9426 / 102.7 | fail 0.9427 / 102.7 | the menu's 3D backdrop as silhouettes: moon, the ship, Skull Island's cliffs, the sea, in their golden places |
| main_menu | fail 0.9434 / 100.8 | fail 0.9434 / 100.8 | the same backdrop |
| chapter_select | fail 0.9335 / 100.9 | fail 0.9335 / 100.9 | the same backdrop |
| loading | pass (2/3) 0.9810 / 116.6 | pass 0.0000 / 0.000 | black; the scene's limits are loose |
| vrex_110 | fail 0.9404 / 144.9 | fail 0.9375 / 144.4 | one large shape over most of the frame (open problem) |
| vrex_140 | fail 0.9404 / 91.4 | fail 0.9375 / 91.2 | as vrex_110 |
| vrex_170 | fail 0.9404 / 91.3 | fail 0.9375 / 91.0 | as vrex_110 |
| pause | fail 0.9404 / 163.6 | fail 0.9375 / 163.1 | the V-Rex frame under the (undrawn) pause menu |
| venture | fail 0.0906 / 19.0 | fail 0.0906 / 19.0 | black (open problem) |
| kong_cutscene | fail 0.8277 / 62.6 | fail 0.8360 / 63.3 | Kong, the V-Rex, trees and the ground as silhouettes roughly where the golden frame has them |
| kong | fail 0.7676 / 94.2 | fail 0.7826 / 97.6 | the V-Rex, Kong and the rocks; characters in bind pose |

2/13 pass on both, and both "passes" are artefacts of the scene limits, not matches. Vulkan and D3D12 give the
same pictures (the small differences are timing). Contact sheets (golden left, native right):
`F:\KK-native-renderer\analysis\04\sheet_vulkan_final.png`, `sheet_vulkan.png`. No frame logs on the native side
(`REX_DEV_FRAME_LOG` is a Xenos plugin feature), so every native run waits for its time limit.

## What the Windows side had to do (the cloud session's list; done, see above)

1. Fetch `cloud-04-backend` into the worktree (`F:\KK-native-renderer`), branch `native-renderer/04-backend`.
2. Turn the option on in the dev build's cache once (it stays; `build.bat` re-runs the preset without
   clearing it): `cmake -S kk -B kk/out/build/kk-dev -DKK_NATIVE_RENDERER=ON`, then `kk\build.bat kk-dev`.
   With `KK_DEV_TOOLS` on (the kk-dev preset), `dev_d3d_trace.cpp` is left out, since it hooks the same
   functions. This builds `rexgpu-native*.dll` (D3D12 through NVRHI's D3D12 backend and the DirectX-Headers
   shim) and copies it beside `king_kong.exe`. It is the first time this code meets the Windows toolchain and
   the first time `d3d12_host.cpp` is compiled at all: expect a round of compile fixes.
3. Milestone 1: run with `--gpu_plugin=native --native_draws=false --native_test=clear` (plus the usual
   `--game_data_root ... --kk_launcher=false`). Expected: the window cycles through colours, the game logic
   runs (audio, vblank-driven waits), the log shows `rexgpu-native: created (Direct3D 12)`,
   `NVRHI renderer ready` and `guest GPU ready`.
4. Milestone 2: the same with `--native_test=triangle`: the RGB triangle over the cycling colour.
5. Milestone 3: `--gpu_plugin=native` (native_draws defaults to on). Expected log line: `D3D hooks connected to
   rexgpu-native (39 entry points)`. The screen shows flat-coloured silhouettes of the main pass, over the
   game's clear colour. If they are garbage, try `--native_wvp_transpose=true`; if they are missing, try
   `--native_all_targets=true`.
6. Check `--gpu_plugin=xenos` (and no flag) is unchanged, with `KK_NATIVE_RENDERER=ON` and with it off.
7. Settle the open questions below from a trace: build once with `KK_NATIVE_RENDERER=OFF -DKK_DEV_TOOLS=ON`,
   record `KK_DEV_D3D_TRACE`, and replay it with `nr_trace_replay` (build `native-renderer` standalone with
   the Windows SDK bundle on `CMAKE_PREFIX_PATH`, or on Linux).
8. Vulkan on Windows: the Windows bundle has no Vulkan, so not before a Vulkan-enabled SDK build; then
   `sdk-patches/0001` is required there too.
