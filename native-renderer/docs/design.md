# King Kong native renderer: design (Phase 0 draft)

Status: Phase 0 investigation, 9 October 2026. This document grows as findings come in. Addresses are in the
game's own (Xbox 360) address space; `sub_XXXXXXXX` names are the recompiled functions in `kk/generated/default`.

## Goal

Replace the emulated Xenos GPU (`rexgpu-xenos`, which translates the game's GPU command stream) with a renderer
of our own that sits at the game's Direct3D layer: the game calls the Xbox 360 XDK's Direct3D library, and we
implement those calls on D3D12 and Vulkan directly. This is what Unleashed Recompiled and Zelda 64: Recompiled
did. It gives: knowledge of every texture, mesh and render target by identity (replacement packs, model swaps),
real resolution independence, a native Linux build, frame interpolation for high frame rates with the game
logic at 30 Hz, motion vectors for DLSS / FSR 3, and room for modern lighting later. The original 360 mode stays
available behind the launcher's Look switch.

## Findings so far

### The game draws only through the XDK Direct3D library

- The XDK Direct3D library lives at about `0x82108000`-`0x82128000` in the game image (~622 functions). Every
  call to the kernel's video functions (`Vd*`) comes from there. XAPI (ReadFile, XInput) sits just below it; the
  Jade engine's code is above.
- GPU draw packets (PM4 `DRAW_INDX` / `DRAW_INDX_2`) are built only inside the library: `sub_82116178` (the
  draw and state flush, ~2000 lines, called from 34 engine sites), `sub_821148F8` and `sub_82114D10` (resolve /
  clear through the resolve path), with `sub_8211FC68` as the packet helper. No engine code builds packets.
- The engine does not record and replay command buffers: the only `INDIRECT_BUFFER` packet is the library's
  own segment kick (`sub_82119FE0`, reached only from `sub_8211AB20`).
- The engine calls 152 of the library's functions (list: `analysis/d3d_entry_points.json`). Most are called from
  fewer than 10 sites. Top: `sub_821091C8` (45 sites), `sub_82116178` (34), `sub_821241A8` (33, a leaf),
  `sub_82115418` = Clear (16).
- Engine code does not write the D3D device struct directly (a scan for stores through the device pointer loaded
  from GDI+4 found none), so the 2005-era XDK here exposes state setting as library calls rather than the later
  inline register writes. The library surface is the whole hook surface.

### Objects

- Jade's graphics interface block ("GDI") is at `0x82D62320`. `GDI+4` holds the D3D device pointer. The device
  is created by `sub_82108BC0` (Direct3D_CreateDevice: allocates 20,608 bytes, 128-aligned, then
  `sub_8211B838` initialises EDRAM, the engines and the ring buffer; `sub_82108B18` shuts down). The engine's
  creation call is in `sub_82797988`.
- `sub_82108B70` / `sub_82108BB0` are AcquireThreadOwnership / ReleaseThreadOwnership (owner thread id at
  device+10376). The engine wraps them in its own lock (owner at GDI+284, count at GDI+288, `sub_820D0000` /
  `sub_820D32D8`); the loading-screen deadlock fix in the port hooks around this.
- The engine calls two function pointers held in the device struct, at device+944 and device+476 (from the
  loading-screen thread and `sub_827253E0`). Who fills them is not found yet (no constant store in the library;
  possibly copied from a template). To be read from a device dump.
- Known engine-side per-frame flow (from the loading-screen work): present = `sub_827977E0` (engine) ->
  `sub_821147B8` (library present) -> `sub_821141D8` (swap, calls `VdSwap`; the port's frame counter hooks it).

### The API, measured

`docs/d3d-api-map.md` has the full map. In V-Rex gameplay a frame is 641 DrawIndexedVertices + 259 DrawVertices
+ 25 Resolves + 8 Clears, driven by about 30 library functions: stream / index / texture / shader / declaration /
constant setters, blend control, render-target and viewport changes, occlusion surveys with conditional
rendering (64 per frame), and resource creation for 12 render-to-texture passes (shadow maps, the screen-space
shadow mask, post effects). Every state the engine sets goes through a library call; constants are copied into
device-struct shadows by those calls. So the renderer replaces those functions and owns all state itself.

### Reusable work

- XenosRecomp (hedge-dev, MIT): Xbox 360 shader microcode to HLSL. Usable for the shader translator with
  attribution. We can check its output against the disc's own shader sources in `Shaders/xeshaders.bin`
  (37 HLSL sources, 6,606 compiled shaders).
- Unleashed Recompiled (hedge-dev, GPL-3.0): the renderer (`video.cpp`, its RHI) is GPL. Its design can be
  studied; its code cannot be copied into this project unless the project adopts the GPL. This repository has
  no LICENSE file yet; decide before any reuse.
- Shader containers: the disc's `xeshaders.bin` (6,606 entries) holds XDK 2005 containers. Their header words
  are not in the order XenosRecomp's `ShaderContainer` expects (pixel: {0x102A0E00, virtualSize, physicalSize,
  0x18, constantTableOffset, definitionTableOffset, ...}; vertex entries carry an 8-byte prefix and
  0x102A0E01), but the parts XenosRecomp needs are standard: a D3DX-style `ps_3_0` / `vs_3_0` constant table
  (CTAB) and Xenos microcode. The translator workstream starts with a container adapter checked against the
  37 HLSL sources shipped in the same file.
- The port's existing tooling: `REX_DEV_FRAME_LOG` (per-draw log, D3D12 and Vulkan), `CaptureFrame`
  (`KK_DEV_SHOTS`), `KK_DEV_AUTOSKIP` / `KK_DEV_SCRIPT` (scripted play into any chapter), the AI name table,
  the Vulkan `/dev/shm` and loading fixes.
- The user's DRM-free PC Gamer's Edition (`F:\KK-native-renderer\The PC Game Files\...`): the same engine on
  D3D9 (`KingKong8.exe`), `XenonTexture.DLL` (360-format texture conversion on PC) and the PC shaders. A second
  reference for naming the 360 D3D layer and for texture formats.

## Architecture (draft)

### Where it plugs in

The runtime loads the GPU as a plugin: `rexgpu-<name>.dll`, chosen by the `gpu_plugin` setting, exporting
`rex_gpu_create` (ABI 1) which returns a `rex::system::IGraphicsSystem`. Ours is `rexgpu-native`. It implements
the interface the runtime expects (`SetupPresentation`, `SetupGuestGpu`, `InitializeShaderStorage`, `Shutdown`;
the ring-buffer calls become no-ops, since nothing writes GPU packets any more) and reuses the SDK's graphics
providers and presenters (`rex::ui::d3d12::D3D12Provider`, `rex::ui::vulkan::VulkanProvider`): they own the
window surface, the swap chain, the upscalers (FSR 1, NIS), FXAA and the ImGui overlay. At Present we render
the final frame into the presenter's guest-output image through `Presenter::RefreshGuestOutput`, exactly as the
Xenos command processor does at swap, so the launcher's display settings keep working unchanged.

### Hooks

The port already hooks guest functions by address (`REX_HOOK_RAW`). The renderer hooks the ~45 Direct3D entry
points the engine uses (the API map) and never calls the originals, except where the original is harmless and
useful (resource allocation can stay in guest memory: textures, vertex and index buffers and shader microcode
live in the 512 MB guest space, written by the engine through Lock / Unlock and streaming reads). The device
struct keeps existing (the engine reads four fields and calls two function pointers in it); creation stays
original and our hooks read what they need from it.

### Rendering model

- Resources: a host texture / buffer per guest resource object, created on first use from guest memory and
  re-uploaded when the engine writes it (Lock / Unlock marks it; streaming into locked memory is covered by
  the Unlock). Guest texture formats (tiled, big-endian, DXT / 8888 / 10:10:10 / depth) are converted on
  upload; the Xenos plugin's texture code and `XenonTexture.DLL` document them.
- Shaders: at CreateVertexShader / CreatePixelShader the container is translated to HLSL (XenosRecomp, MIT),
  compiled with DXC to DXIL and SPIR-V, cached on disk by hash. Vertex declarations become input layouts.
- Draws: DrawIndexedVertices / DrawVertices bind the current state (pipeline keyed by shaders, declaration,
  blend, depth / stencil, render targets) and issue the draw. Constants go into a per-draw constant buffer
  from the device's shadows.
- Render targets: EDRAM surfaces become pooled host render targets keyed by size and format; Resolve becomes a
  copy (or a clear) into the destination texture, with the resolve flags' clears.
- Occlusion: conditional surveys and rendering are an optimisation on the 360 (skip occluded draws). First
  version: ignore them and draw everything (identical picture). Later: host occlusion queries.
- Present: resolve of the frontbuffer -> our final image -> `RefreshGuestOutput`.
- Interpolated high frame rates come later: the renderer sees every object's transform (the world matrix set
  before each draw), so it can draw in-between frames while the game logic stays at 30 Hz.

### Backend

NVRHI (MIT): one API over D3D12 and Vulkan, creates on the SDK provider's existing device and queues
(`nvrhi::d3d12::DeviceDesc{pDevice, pGraphicsCommandQueue}`, `nvrhi::vulkan::DeviceDesc{instance,
physicalDevice, device, graphicsQueue...}`), bindless descriptor tables, automatic barriers, resource lifetime
tracking, DXC shaders. It has no occlusion queries (timer and event queries only), which the first version
doesn't need. Alternative: the SDK's own thin device layers (what the Xenos plugin uses), more work.
Unleashed Recompiled's renderer is GPL-3.0 and is studied, not copied.

### Graphics API: Vulkan first (decided 10 October 2026)

The user chose Vulkan as the native renderer's primary API: one code path for Windows, Linux and the Steam Deck,
and the Linux port is a main goal. NVRHI keeps D3D12 available behind the same interface, so D3D12 is a
secondary backend for Windows, built and tested after Vulkan and only kept if it earns its place (for example
a driver problem on some Windows machines). OpenGL is not considered. Bring-up, milestones and golden-frame
comparisons are done on Vulkan first.

### Licence

The repository has no LICENSE file. Reusing MIT code (XenosRecomp, NVRHI) is fine under any licence we pick;
adopting GPL code would force GPL on the whole port. Decision needed before Phase 1 (recommendation: MIT or
BSD-3, matching the SDK).

## Plan

### Phase 0 (serial, this session)

1. Map the D3D layer: the 152 entry points by role (draw, clear, resolve, set texture / sampler / render state /
   shader / constants / stream / indices / render target, create / lock / unlock resources, present, queries),
   their argument conventions, and the device struct fields the engine reads. Tooling: `dev_d3d_trace.cpp`
   (`KK_DEV_D3D_TRACE=<seconds>[,<frames>]`): per-frame call counts and a call-by-call argument log.
2. Answer-key tooling: golden frames and per-draw logs from today's renderer for a fixed set of scenes
   (scripted), and a comparison tool. The new renderer must reproduce them.
3. Decide on reuse (XenosRecomp yes; RHI: write our own on top of the SDK's D3D12/Vulkan device code or a
   permissively licensed RHI) and on the project's licence.
4. Design the modules and interfaces (below), write the agent briefs for Phase 1.
5. A small standalone build of the renderer so Phase 1 agents test in seconds, not minutes.

### Phase 1 (parallel agents, worktrees)

| Workstream | Deliverable | Checked against |
|---|---|---|
| D3D layer map (brief 01) | Names, roles and argument structs for all entry points; device struct layout | The trace logs, draw logs |
| Shader translator (brief 02) | Every shader in `xeshaders.bin` and all cached ones converted and compiled on D3D12 and Vulkan | The disc's HLSL sources; today's renderer's output |
| Textures and resources (brief 03) | 360 texture / vertex / index formats, tiling, endianness; resource identity and replacement hooks | Today's texture cache |
| Backend plugin (brief 04) | `rexgpu-native` on NVRHI over the SDK's providers: clear colour, triangle, hook skeleton | The game running with the plugin |
| Test harness (brief 05) | Golden-frame runs, diff reports, per-chapter scripts | n/a |

The briefs are in `docs/briefs/`. Streams 01, 02, 03 and 05 are independent; 04 needs the NVRHI clone and
the SDK headers only. Each runs in its own worktree of `kk-recomp` on a `native-renderer/<stream>` branch.

### Phase 2 (serial, integration)

Bring-up in the game: menus first, then one chapter, then every chapter against the golden frames. Then the
port's own features move over (AO, fog switch, upscalers, HDR, Linux). Then the new things (interpolated high
frame rates, DLSS / FSR 3, replacement packs).

### Phase 0 status (10 October 2026)

1. D3D layer map: done to the level Phase 1 needs (`d3d-api-map.md`): every per-frame call named with its
   arguments and device fields; the render-state and sampler-state tables decoded; the register shadow
   anchored; no direct device writes, no command-buffer replay, no raw packets from the engine.
2. Answer-key tooling: the capture side exists (`KK_DEV_SHOTS*`, `REX_DEV_FRAME_LOG`, `KK_DEV_SCRIPT`); the
   compare tool and scene set are brief 05's first task.
3. Reuse decided: XenosRecomp (MIT) as the shader translator base, NVRHI (MIT) as the RHI, both pending the
   user's permission to clone; licence for the repository pending the user's decision.
4. Modules, interfaces and briefs: `docs/briefs/`.
5. Standalone build: folded into brief 04's milestones 1 and 2 (the plugin runs inside the game, but the
   clear-colour and triangle milestones are the fast test loop; a game-free build is not needed because the
   plugin links only against the SDK's UI / provider code).

## Open questions

- Whether the engine reads back resolved surfaces on the CPU (would need a readback path).
- How the engine handles the 360's tiling / predicated tiling, if at all (one 1280x720 surface, no MSAA, per
  the frame logs: likely none).
- `sub_82110D90` (two calls per frame from the present path, eight register packets).
- Which render states beyond the traced ones the menus, videos and Kong chapters use (brief 01 covers it).

Answered: the device's +944 / +476 pointers are entries of the render-state getter / setter tables; the
engine never writes shader constants or state into the device directly (everything goes through library
calls, so the hooks see all of it).
