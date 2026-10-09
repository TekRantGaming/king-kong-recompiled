# Brief 04: the `rexgpu-native` plugin and backend

## Goal

A GPU plugin the runtime loads instead of `rexgpu-xenos`: `rexgpu-native.dll` (and `.so`) exporting
`rex_gpu_abi_version` / `rex_gpu_create`, implementing `rex::system::IGraphicsSystem`, reusing the SDK's
`D3D12Provider` / `VulkanProvider` and presenters, with NVRHI created on the provider's device. First milestone:
the game runs with the plugin and the screen shows a clear colour that changes each frame; second: a triangle
drawn through NVRHI appears in the presenter's guest output; third: the D3D hook skeleton (brief 01's names)
routes DrawIndexedVertices into the backend. Output: `native-renderer/plugin/` and `native-renderer/backend/`.

## Inputs

- The SDK (`C:\rexsrc`, read-only; copy headers you need): `include/rex/system/gpu_plugin.h`,
  `include/rex/system/interfaces/graphics.h`, `include/rex/graphics/graphics_system.h` (the Xenos plugin's
  base class, a model to follow), `src/graphics/plugin_main.cpp`, the providers (`include/rex/ui/d3d12/
  d3d12_provider.h`: `GetDevice()`, `GetDirectQueue()`; `include/rex/ui/vulkan/provider.h`, `device.h`:
  `device()`, `physical_device()`, `instance()`, `queue_family_graphics_compute()`, `AcquireQueue`), and the
  presenter (`include/rex/ui/presenter.h`: `RefreshGuestOutput(width, height, display_w, display_h,
  refresher)` with the backend-specific `GuestOutputRefreshContext` giving the image to render into; see
  `IssueSwap` in `src/graphics/vulkan/command_processor.cpp` around line 2460 and the D3D12 equivalent).
- NVRHI (NVIDIA-RTX, MIT): `nvrhi::d3d12::DeviceDesc{pDevice, pGraphicsCommandQueue}` and
  `nvrhi::vulkan::DeviceDesc{instance, physicalDevice, device, graphicsQueue, graphicsQueueIndex, ...}` create
  on existing devices; `createHandleForNativeTexture` wraps the presenter's image. Vendor it under
  `native-renderer/thirdparty/nvrhi` with its licence file (permission per `docs/design.md`).
- The port's build: `kk/CMakeLists.txt`, `kk/CMakePresets.json`; the plugin is a new CMake target next to the
  game, installed beside `king_kong.exe`; the game selects it with `--gpu_plugin=native` (the app sets
  `gpu_plugin` to `xenos` when empty: `kk/src/king_kong_app.h`, and preloads `rexgpu-xenos*.dll` in
  `kk/src/launcher.cpp`, which needs a `native` case).

## Method

1. Plugin skeleton: copy the Xenos plugin's `GraphicsSystem` scaffolding minus the ring buffer: `SetupPresentation`
   creates the provider and presenter, `SetupGuestGpu` records memory and kernel state, `InitializeRingBuffer` and
   the interrupt callback become no-ops (but keep vblank interrupts firing: the game waits on them; see how
   `GraphicsSystem::MarkVblank` and the vsync worker do it).
2. NVRHI device on the provider's native device; a command list per frame; on the game's Present hook (brief
   01: `sub_821147B8`), `RefreshGuestOutput` with a refresher that renders into the given image.
3. Milestone 1: clear colour. Milestone 2: a triangle from a static buffer with a hand-written HLSL pair
   compiled by DXC for both backends. Milestone 3: the hook table: a C++ file mapping each named entry point
   to a handler (`REX_HOOK_RAW` style, as `kk/src/*.cpp` do), with state tracking for the per-draw calls, and
   DrawIndexedVertices reaching NVRHI with a placeholder pipeline.
4. Keep the Xenos plugin working: everything is behind the plugin choice; `--gpu_plugin=xenos` must be
   unchanged.

## Done when

Milestones 1-3 run on D3D12 and on Vulkan (Windows; Linux through the AppImage build later), with the
plugin selectable from the command line and the old plugin untouched.
