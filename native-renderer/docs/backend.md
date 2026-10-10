# Stream 04: the backend and the `rexgpu-native` plugin

Status, 10 October 2026 (cloud session, branch `cloud-04-backend`): milestones 1 and 2 and milestone 3's
call path are built and verified on Linux without the game; nothing has run inside the game yet.

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
            |          -> (plugin) ApiBinding -> DrawState -> Renderer (NVRHI) -> command list
            '-- the game's original (__imp__sub_...): the library keeps running
the library's packets -> ring skimmer (fences, interrupts, VdSwap) -> Backend::Present -> presenter
```

At the game's Present the renderer closes and submits the frame's command list. At the guest's swap the
backend copies the last submitted frame into the presenter's guest output; if no game frame arrived since the
last swap (hooks not built in, or `--native_draws=false`), it draws the test picture instead
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
  shader byte-swaps the position (three floats at the declaration's POSITION offset) and multiplies by
  c0..c3 as rows (`mul(wvp, pos)`, which is what the guest's `dp4 oPos.x, v0, c0` computes; the draft had
  `mul(pos, wvp)`). `Renderer::Options::transpose_wvp` flips it. Colour: a hash of the shader pair.
- **Only the main surface is drawn.** Render target 0 at the last Present is the frame's surface; draws to
  other targets (shadow maps, post effects) are skipped until there is a render-target pool. The first frame
  draws everything. `--native_all_targets=true` draws them all.
- **Push constants** for the placeholder's per-draw data (96 bytes; root constants on D3D12).
- **Shaders** are compiled by DXC (Linux release 1.8.2505.1, with `libdxil.so`, so the DXIL is signed) to
  both DXIL and SPIR-V; `compile_shaders.sh` / `.ps1`; the headers are checked in.
- **Clear's stencil argument** is read from r9 (`hooks::kClearStencilRegister`), on the assumption that the
  float Z takes a GPR slot as in the 64-bit PowerPC ELF ABI. Unconfirmed; see open questions.
- **Clear flags**: bit 0 or bits 4-7 colour, bit 1 depth, bit 2 stencil (traced values 1, 0xF, 0x3F, 0x30).
  Unconfirmed.
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

- Clear's stencil register (r8 or r9) and the clear flag bits: look at Clear calls in `analysis/d3dtrace2.log`
  (r8 / r9 next to f1) and at `sub_82114D10`.
- Whether the recompiled code's indirect calls (the render-state setter table, `*(dev + 96 + state)`) reach
  `REX_HOOK_RAW` overrides. They should (the function table maps addresses to the `sub_` symbols), but if
  `SetRenderState(...)` log lines never appear, they do not.
- Whether c0..c3 are really the world-view-projection rows for most draws (`--native_wvp_transpose=true`
  reads them as columns).
- `d3d12_host.cpp` has never been compiled. It matches the current `HostDevice` interface and the D3D12
  presenter's contract by reading.
- The presenter wraps are cached by resource pointer and size (D3D12) or image and version (Vulkan); a freed
  and reallocated resource at the same address and size would reuse a stale NVRHI handle.
