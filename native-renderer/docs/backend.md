# Stream 04: the backend and the `rexgpu-native` plugin

Status, 10 October 2026 (Phase 2): the game renderer draws the game with its own translated shaders, textures,
states, render targets and resolves. On Windows with Vulkan **all 13 golden scenes pass**
(`F:\KK-native-renderer\runs\native-20261010-172553`); see "Phase 2: the game renderer" below. Milestones 1-3
(Phase 1) are described further down; the placeholder pipeline remains behind `--native_game=false`.

## Layout

| Path | What | Depends on |
|---|---|---|
| `CMakeLists.txt`, `cmake/nvrhi.cmake` | standalone project; kk includes it with `KK_NATIVE_RENDERER=ON` | |
| `backend/draw_state.*`, `draw_sink.h` | the draw-state tracker: D3D-level state from the hooked calls, one record per clear / draw / resolve / present | nothing |
| `backend/api_binding.*` | the `NrApi` table (`plugin/native_api.h`) bound to a tracker; the plugin and the tests use the same code | nothing |
| `backend/guest_layout.h`, `guest_memory.h` | guest object layouts (vertex / index buffer, declaration, viewport), big-endian helpers | nothing |
| `backend/primitive.*` | strips, fans, quads, base vertex, reset index to host triangle lists | nothing |
| `backend/renderer.*`, `shaders.*`, `shaders/` | the NVRHI renderer: frame images, the milestone pictures, the placeholder pipeline; the built-in blit and clear shaders | NVRHI |
| `backend/game_renderer.*`, `game_resources.cpp`, `game_passes.cpp` | Phase 2: the game's draws with their own shaders, textures and states; render targets, resolves, clears, Present | NVRHI, `kknr_resources`, `kkshaders` |
| `backend/shader_library.*` | the game's shader objects as NVRHI shaders (the pack, else translated and compiled with DXC) | `kkshaders` |
| `backend/guest_device.h` | the D3D device struct's register images, shadows and fields the game renderer reads | nothing |
| `cmake/kk_libs.cmake` | the resources and shader libraries built into the plugin | the SDK (fmt, xxHash, dxcapi.h) |
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
  version (logged, originals only). Version 3 (Phase 2) passes the device, Clear's rectangles, Resolve's
  arguments and shader creation; version 4 (Phase 2b) adds `vertex_shader_patched`, a hook after the
  library's `sub_82120028` (the rewrite of a vertex shader's fetches for a declaration), for the frame log.

## Phase 2: the game renderer

Branch `nr-integration` (worktree `F:\KK-native-renderer\wt\04-backend`), on top of `native-renderer` with the
three Phase 2 cloud sessions merged in (resolve planning, Linux tests, translator version 4).

### How a frame is drawn

The hooks keep calling the library's originals, so the D3D device struct (20,608 bytes, `guest_device.h`) is
always what the GPU would have been sent. At every draw the game renderer reads it:

- **Shaders.** `ShaderLibrary` registers every shader when it is created (hooks on `sub_82111CA0` /
  `sub_82111D90`, run after the original so the object is known): the container's microcode hash is looked up
  in the pack (`kkshaders-spirv.pack` or `kkshaders-dxil.pack` beside the game, or `--native_shader_pack`);
  shaders missing from it (the D3D library's own two) are translated and compiled with DXC
  (`dxcompiler.dll` beside the game, or `--native_dxc`). A shader object the hooks did not see is parsed from
  the object (container copy + microcode). The game creates all 6,585 shaders at boot: 4,073 distinct ones
  come from the pack, 2 are compiled.
- **Constants.** Vertex and pixel float constants (+1920, +6016) are byte-swapped into two volatile constant
  buffers (rewritten only when they change, and once per frame); bool and loop constants, texture and sampler
  descriptor indices, the vertex fetch table, clip planes, the viewport transform and the alpha test go into
  the draw constants (`kkshaders::DrawConstants`, ABI 2).
- **Bindless.** Set 0 holds the three constant buffers; sets 1-6 are NVRHI bindless tables (2D, 3D, cube and
  2D-array textures, samplers, raw vertex buffers). Slot 0 of each is a dummy; freed slots are reused after 8
  frames. Vulkan needs the descriptor indexing features: `sdk-patches/0002`.
- **Vertex data** is pulled by the shaders from guest memory as it is (big-endian): each guest vertex buffer
  is one host raw buffer (`kknr::BufferCache`), each declaration element found by usage and index, read with
  **the element's own endian field** (`--native_element_endian`, default on). The fetch constant's endian
  (8in32) scrambled the game's SHORT4N normals and tangents (8in16): Kong's fur shells pointed everywhere.
  Index buffers are converted to little-endian host buffers; quad lists and fans become triangle lists
  (static patterns for non-indexed draws, a per-frame ring for indexed ones).
- **Textures and samplers** come from the fetch constants SetTexture merged into the device (+1152 + 24n):
  `kknr::TextureCache` keys, `ConvertTexture` uploads, a view per (swizzle, dimension, mip range), a sampler
  per (clamp, filters, anisotropy, border, LOD bias). Uploads are redone only after a CPU write: the SDK's
  physical write watches (`EnablePhysicalMemoryAccessCallbacks` + an invalidation callback) feed
  `InvalidateRange`.
- **Pipeline state** from the register images: RB_BLENDCONTROL0-3, RB_COLOR_MASK, RB_DEPTHCONTROL and the
  stencil masks, PA_SU_SC_MODE_CNTL (cull, face, fill, polygon offset), PA_CL_CLIP_CNTL, RB_COLORCONTROL /
  RB_ALPHA_REF (alpha test in the shader). Pipelines are cached by a hash of all of it plus the shaders and
  attachment formats.
- **Viewport.** As the Xenos plugin does: the host viewport is the whole target and the vertex shader applies
  the guest's transform (PA_CL_VTE_CNTL, PA_CL_VPORT_*), the window offset and D3D9's half-pixel offset
  (`ndcScale` / `ndcOffset`); the window scissor is the host scissor. NVRHI flips Vulkan viewports to D3D's
  +Y up and the translated shaders are compiled with `-fvk-invert-y`: the Y scale is negated once on Vulkan.
- **Render targets** are host textures keyed by EDRAM base, pitch (RB_SURFACE_INFO) and format, as tall as
  the tallest surface seen there, so the game's small passes inside the 1280-pitch surface land where the 360
  puts them. Depth is D32S8 on every API (RADV has no D24S8 attachments).
- **Clears** clear the bound targets over the rectangles or the viewport (whole texture, or a triangle under a
  scissor). **Resolves** copy or blit the source rectangle into the destination texture's host copy
  (`kknr::PlanResolveConversion`: raw copy, R / B exchange for A8R8G8B8, depth into R32F) and mark it
  GPU-written; their clears follow. **Present** blits the back buffer (+13964) into the frame image through
  the display gamma ramp (the device's copy of SetGammaRamp's table at +14060), which the Xenos plugin also
  applies at the swap; without it every mid-tone was a few levels too bright.

### Found on the way

- **Pixel interpolators.** About 700 of the database's 2,343 pixel shaders have 0 in the binding table's word 1
  count and read every input as zero (the title's logo was a black rectangle). The count is now also taken
  from the interpolator mask in word 6 (`shaders/src/container.cpp`, translator version 3). The structural
  check passed before because an empty list has nothing to compare.
- **Textures over a resolve's memory.** The light shafts read the depth resolve's k_24_8 memory as
  k_8_8_8_8, copy that into a 320x180 target, resolve it as k_8_8_8_8 and read it back as k_24_8. A texture
  whose base address a resolve wrote through another fetch constant is now a converted copy of the resolve's
  texture (depth to the D24S8 word's bytes, those bytes back to depth, same format copied), redone after each
  new resolve there. Without it Kong to the Rescue had no light shafts.
- **Pipeline creation** on a cold driver cache takes seconds per scene (186 new pipelines on entering Kong to
  the Rescue made the chapter load about 9 s later than on Xenos, so the scripted shots missed the
  cutscene). NVIDIA's own disk cache makes the second run normal (290 ms of pipeline creation for the whole
  chapter). Since then pipelines are made on worker threads, draws wait for them within a budget, and a
  pipeline cache of our own is on by default ("Pipeline cache").

### Settings

`--native_game` (on; off = the placeholder), `--native_shader_pack`, `--native_dxc`, `--native_element_endian`
(on), `--native_flip_front_face`, `--native_dump_frame=N` (every clear, draw with its textures and vertex
fetch, resolve with its destination, of frame N), `--native_debug` (bits: 1 green clears, 2 magenta frame
image under the back buffer, 4 no gamma ramp, 8 the most invalidated pages every 300 frames). The stats line every 300 swaps counts draws, skips, pipelines,
uploads, resolves, aliases and the time spent creating pipelines and uploading textures.

For a run: `kkshaders-spirv.pack` (and `kkshaders-dxil.pack` for D3D12) from `kkshaders db-build ... --split`
beside `king_kong.exe`, `dxcompiler.dll` and `dxil.dll` from the DXC release beside it too (neither in git).
`tests/run.ps1` takes `-ExtraArgs` for extra game arguments.

### Render scale (session E)

`--native_render_scale` (default `1`; `1.5`, `2`, `3`, any number from 0.25 to 8, or a frame size `1920x1080`
that sets each axis to size / 1280 or 720) and `--native_shadow_scale` (0 = like the render scale; `1` keeps the
shadow maps at the console's size). Both are init-only. At `1` nothing below happens: the same paths, the same
constants, the same pipeline layout (a test compares the frame of an explicit 1 with the default's float for float; the
22 images the pre-existing game tests write are byte-identical to those of the commit before this work; the
translator's output and hashes at 1 equal the commit before this work for the 1,557 translatable
corpus shaders, see `shaders.md`).

What scales, and how (`backend/game_renderer.*`, `game_resources.cpp`, `game_passes.cpp`, `render_scale.h`):

- **Render targets** are keyed as before (depth flag, EDRAM base, pitch, format). A `HostTarget` keeps its
  guest size (`width` = pitch, `height` = rows drawn so far) and gets `host_width` / `host_height` =
  `round(guest * scale)` (`ScaleCoord`, the same rounding everywhere) and `scale_x` / `scale_y`. The texture
  is the host size. Everything that reasons about the console's surfaces (the "grow to the tallest" rule,
  rectangle clamps, the key) stays in guest pixels.
- **Clip space stays in guest pixels.** `ComputeViewport` computes `ndcScale` / `ndcOffset` from the guest
  size, so the vertex shaders are untouched; the host viewport is the whole scaled target, and the window
  scissor is clamped in guest pixels, then scaled (`ScaleRect` rounds each edge, so neighbours still tile).
- **Clears**: whole-target clears are the same calls; a rectangle (the call's, or the viewport) is clamped in
  guest pixels and its scissor scaled. **Resolves**: the source rectangle, the destination point and the clear
  rectangles are guest pixels; the copy or blit uses scaled rectangles (origins round on their own, the size is
  `round(size * scale)` on both sides, clamped to what both textures hold).
- **Resolved textures** take their source's scale: the host texture is the guest plan's size times it
  (`HostTexture::scale_x/y`, only for textures a resolve can write, i.e. uncompressed formats). It is
  re-created when the scale differs, and a CPU upload over memory that was a scaled resolve re-creates it at 1:1
  (`UploadTexture`). Aliases (the light shafts read a depth resolve's memory through other formats) are made at
  the source's scale, with scaled blit rectangles. Mip levels above 0 of a scaled texture are the host size
  shifted (not `round(guest_level * scale)`): not checked against the game (a resolve into a higher level of a scaled texture is approximate).
- **Present**: the back buffer's target is blitted 1:1 into the frame image, which `Backend::Initialize`
  creates at `round(display * scale)` (`Renderer::Initialize(width, height)`). `Backend::Present` already
  hands `renderer_->width()/height()` to `RefreshGuestOutput`, and `CopyToGuestOutput` copies whatever size it
  is told, so the presenter gets the scaled image and its upscalers (FSR1, NIS) and FXAA work on it as before
  (they see a larger source and the window's size as the display).
- **Shadow maps**: a colour target of format `k_32_FLOAT` whose pitch is not the frame's (832x832 in the game)
  is a shadow map; its pitch is remembered and every attachment of that pitch (its depth buffer) gets the
  shadow scale. The rule is by pitch because every attachment of a draw has one size. A game `k_32_FLOAT`
  target of another purpose and another pitch would also get the shadow scale (none seen); `guest_frame_width`
  is the backend's display width.

What the shaders see. Three things in a translated pixel shader are in host pixels or host texels, and each
has a rule (the translation is only different for pixel shaders, and only when the renderer scales; vertex
shaders are never changed):

| Case | Decision |
|---|---|
| `VPOS` (the pixel position register, 1,621 database pixel shaders) | **Scaled back to guest pixels**: `iPos.xy * (1/scale) - 0.5`. A host pixel centre becomes the point of the guest pixel grid it covers, so a shader that uses VPOS as a screen coordinate (screen-space lookups, noise or dither tiled by position, the depth of a pixel) reads the same place at every scale. It is fractional now: a shader that takes `floor`/`fmod` of it for a pattern gets a pattern in guest pixels, sampled more finely, as intended. |
| Unnormalised fetches (`texCoordDenorm`), fetch offsets (half texels) and `GetTextureWeights` (the fraction of a texel position) | **The texture size they divide by is the guest's**: after each `GetDimensions` the aware code applies `kk_GuestSize`, the host size times the inverse scale of the texture bound at that fetch constant (1 for textures the guest uploaded, the resolve's scale for resolved ones). A coordinate in guest texels is then normalised by the guest's size, an offset of half a guest texel is still half a guest texel, and the weights are those of the guest's texture. The texture is sampled by its normalised coordinate, so a larger texture with more texels just samples finer. |
| Normalised fetches, and constants that hold texel sizes or pixel steps (`1/width`, blur taps) | **Left alone**. A normalised coordinate is the same at any size. A constant the game sets as `1/1280` (blur taps, the shadow filter's steps) still steps one guest texel; at scale 2 that is two host texels, so the filter samples every other host texel: the same footprint, coarser taps. That is a quality choice, not an error, and changing it would mean knowing every constant (`kkshaders scale-report` lists the constants whose names look like sizes per family). |
| Derivatives, LOD (`CalculateLevelOfDetail`, implicit LOD) | Left alone: they are per host pixel and per host texel, so the chosen mip follows the ratio on screen, as with any higher resolution. |
| `GetDimensions` in vertex shaders (vertex texture fetch) | Not translated aware, and not checked against the database (a vertex shader that fetches a resolved texture with unnormalised coordinates would see the host size). |
| Point sprites and line widths | Not scaled: sizes the rasteriser takes in pixels stay host pixels (a point sprite or line gets thinner relative to the picture). Not checked against what the game draws. |

The scale constants (`kkshaders::ScaleConstants`, `shaders/include/kkshaders/abi.h`): `b3 space0`, a
volatile buffer written per draw: the draw's render scale and its inverse, and for each of the 32 texture fetch
constants the inverse scale of the texture bound there. `BindTexture` reports the scale of what it bound
(`bound_scale_x_/y_`, alias or resolve copy). The binding exists in set 0's layout only when `scaled()`;
the 1:1 layout still has b0 to b2. Aware pixel shaders declare it, plain ones never mention it.

Shader packs. A pack made with `kkshaders db-build ... --also-scaled` holds the 1:1 and the aware variant of
every pixel shader (different translation input hashes, one pack). At a scale the library asks for the aware
variant and does **not** fall back to the plain translation of the same microcode (it would be wrong at that
scale); a shader missing from the pack is compiled at startup (needs DXC). Without `--also-scaled` every
pixel shader the game uses is compiled with DXC the first time it is seen: a stutter for the first minutes, and
nothing at all without `dxcompiler`. The plain pack keeps working at scale 1.

Known limits, decided, not fixed:

- The window's size is not followed: `1920x1080` is typed by hand. Targets and the frame image are made at init,
  and re-making them when a window is resized is future work (a reset of `targets_`, `resolved_by_base_` and
  the frame images).
- A texture partly written by a resolve (a rectangle) keeps the scale of its source; the rest of it holds what
  it held before (as at 1:1).
- Non-integer scales round rectangle edges to host pixels: a one-pixel row can differ from the exact position.
  Integer scales are exact on the guest grid.
- Rendering that depends on sub-pixel alignment of 1:1 targets (the half pixel offset stays in guest pixels,
  as it must) is the same picture, not the same pixels, at another scale; the tests state the tolerance below.
- Memory and time grow with the area: a scale of 3 is 9 times the pixels of every target (3840x2160 colour,
  depth and the post chain), and the resolves copy that much.

Tests (`nr_game_tests`, all on a CPU Vulkan device; the scene is drawn at scale 1 and at the other scales and
the frame is reduced to the guest's size by an area average before comparing):

| Test | Checks |
|---|---|
| `Scale_ParseSetting` | the setting's forms and refusals, rounding, clamping, never zero |
| `Scale_UnitModeIsTheSameRenderer` | an explicit scale 1 (and shadow scale 1) is not `scaled()` and gives the same frame, float for float |
| `Scale_FlatSceneMatchesAtScales` | quadrants, a clear rectangle, a slanted triangle, a draw under a smaller viewport at 2, 3, 1.5 and 2x1.5; the frame image has the scaled size |
| `Scale_ClearRectsAndViewportClearAreExact` | clear rectangles and viewport clears land on the exact host pixels at 2 and 1.5 |
| `Scale_ResolveToTextureAndSample` | draw, resolve into a texture, draw the texture: the same quadrants at 2, 3, 1.5; the targets and their sizes |
| `Scale_ResolveRectAndDestinationPoint` | a resolve rectangle to a destination point at 2 and 1.5 |
| `Scale_ResolveClearsColourAndDepth` | the resolve's colour and depth clears (with a depth buffer) at 2 |
| `Scale_PostPassWithVposAndUnnormalizedFetch` | a full-screen pass that reads VPOS and fetches a resolved texture with unnormalised coordinates and a half texel offset: the picture of scale 1 at 2, 3, 1.5 |
| `Scale_PostPassOverAnUnscaledTexture` | the same pass over a guest-uploaded texture (not scaled) at 2, 3, 1.5, and the control: with the plain pixel shaders at scale 2 the picture is wrong |
| `Scale_ShadowMapsHaveTheirOwnScale` | a 32x32 `k_32_FLOAT` target follows the shadow scale (like the render scale, 1, or 3) and the main target the render scale |

Stated tolerance of the comparison against scale 1: flat regions and edges on the guest grid to the existing
colour tolerance (2.5/255); over a frame the mean absolute difference per channel is at most 1% and at least
96% of the pixels are within 10% (the slanted edge's pixels are partly covered on one grid and whole on the other).
The translator side has its own test (`kkshaders_tests scale`, `shaders.md`).

### Scores (Vulkan, `kk-dev-vulkan`)

`tests/run.ps1 -Plugin native -Exe kk\out\build\kk-dev-vulkan\king_kong.exe -NoFrameLog`, commit 0adac16
(`F:\KK-native-renderer\runs\native-20261010-172553`). Worst frame (outside-range share, MAE); the baseline
is Phase 1's placeholder (`native-20261010-073756`).

| Scene | Phase 2 | Frames | Baseline (placeholder) |
|---|---|---|---|
| video | pass 0.0000 / 0.10 | 3/3 | pass (black) 0.0000 / 0.00 |
| title | pass 0.0000 / 0.01 | 3/3 | fail 0.1255 / 11.3 |
| save_menu | pass 0.0000 / 1.42 | 3/3 | fail 0.9426 / 102.7 |
| main_menu | pass 0.0001 / 2.22 | 3/3 | fail 0.9434 / 100.8 |
| chapter_select | pass 0.0001 / 2.04 | 3/3 | fail 0.9335 / 100.9 |
| loading | pass 0.0000 / 0.01 | 3/3 | pass (black) 0.9810 / 116.6 |
| vrex_110 | pass 0.0003 / 3.61 | 5/5 | fail 0.9404 / 144.9 |
| vrex_140 | pass 0.8566 / 17.63 (a lightning flash; 3/5 needed) | 3/5 | fail 0.9404 / 91.4 |
| vrex_170 | pass 0.0001 / 3.17 | 5/5 | fail 0.9404 / 91.3 |
| pause | pass 0.0000 / 0.00 | 3/3 | fail 0.9404 / 163.6 |
| venture | pass 0.0082 / 4.65 | 4/5 | fail 0.0906 / 19.0 |
| kong_cutscene | pass 0.0005 / 5.48 | 3/3 | fail 0.8277 / 62.6 |
| kong | pass 0.0059 / 9.89 | 4/5 | fail 0.7676 / 94.2 |

13/13 pass (the Xenos self-check also passes 13/13). The earlier run of the same build before the alias fix
(`native-20261010-165855`) passed 10/13: Venture missed on timing, both Kong scenes on the cold pipeline cache
and the missing light shafts.

### D3D12

The same build with `--native_backend=d3d12` (commit 9167e9a, `kkshaders-dxil.pack` beside the game). Two
fixes were needed: NVRHI's D3D12 backend bound the sampler table from the CBV/SRV/UAV heap (patched, see
`thirdparty/NOTES.md`), and the sampler table's 2,048 slots did not fit NVRHI's default sampler heap (now
1,024 slots, heaps 65,536 views and 2,048 samplers). Full run `native-20261010-173555`: 10/13 pass, with the
first D3D12 visit to Kong to the Rescue compiling its pipelines cold (4.4 s; the cutscene shots came before
the chapter had loaded) and Venture on a lightning flash. Run again with warm caches
(`native-20261010-174427`, `native-20261010-174811`): kong_cutscene pass 0.0005 / 7.85, kong pass 0.0076 /
8.96, venture pass 0.0000 / 3.49 (5/5). Every scene has therefore passed on D3D12 as well:

| Scene | D3D12 worst frame (outside, MAE) |
|---|---|
| video | 0.0000 / 0.10 |
| title | 0.0000 / 0.01 |
| save_menu | 0.0000 / 1.20 |
| main_menu | 0.0000 / 1.34 |
| chapter_select | 0.0000 / 1.21 |
| loading | 0.0000 / 0.01 |
| vrex_110 | 0.0001 / 3.86 |
| vrex_140 | 0.0006 / 3.73 |
| vrex_170 | 0.0005 / 4.26 |
| pause | 0.0000 / 0.00 |
| venture | 0.0000 / 3.49 (second run) |
| kong_cutscene | 0.0005 / 7.85 (warm caches) |
| kong | 0.0076 / 8.96 (warm caches) |

SV_VertexID includes the base vertex on D3D12 as on Vulkan for these draws (the indexed scenes match).

### Phase 2b: after the cloud merges (render scale, pipeline cache, frame log)

Scores with frame logs (no `-NoFrameLog`), Vulkan, worst frame (outside-range share / MAE) and the draw-list
ratio where the scene has a frame log. Runs `native-20261011-011849` (the first eleven scenes) and
`native-20261011-013000` (Venture and Kong to the Rescue, run again after a demo of the user's ran beside the
first run's Venture):

| Scene | Result | Frames | Draw list |
|---|---|---|---|
| video | pass 0.0000 / 0.10 | 3/3 | 1.000 |
| title | pass 0.0000 / 0.01 | 3/3 | 1.000 |
| save_menu | pass 0.0001 / 1.25 | 3/3 | 1.000 |
| main_menu | pass 0.0002 / 1.49 | 3/3 | |
| chapter_select | pass 0.0008 / 1.39 | 3/3 | |
| loading | pass 0.0000 / 0.01 | 3/3 | |
| vrex_110 | pass 0.2433 / 27.32 (a lightning frame; 3 of 5 needed) | 4/5 | |
| vrex_140 | pass 0.0006 / 3.42 | 5/5 | 0.986 |
| vrex_170 | pass 0.0002 / 3.20 | 5/5 | |
| pause | pass 0.0000 / 0.00 | 3/3 | |
| venture | pass 0.0024 / 3.79 | 5/5 | 0.975 |
| kong_cutscene | pass 0.0004 / 7.99 | 3/3 | |
| kong | pass 0.0035 / 9.17 | 5/5 | 0.997 |

13/13. Again after the AO merge (AO off, the default) and the pipeline cache's version 2:
`runs
ative-p2b-final1` (eleven scenes; vrex_170 4/5 with a lightning frame, kong_cutscene 0.0007 / 8.3)
and `runs
ative-p2b-kong-nocache` (kong_cutscene 0.0005 / 7.6, kong 0.0025 / 10.4, draw list 0.992); with the
cache on Kong's timeline moves, see "Pipeline cache". What it took after the merges (the first run with frame logs, `native-20261010-235005`, had the
pictures of twelve scenes and draw lists far off):

- **Frame log parity** ("The frame log" below): the library's Clear is its own draw, the vertex shader hash
  comes from a hook after the library's fetch patching (NrApi 4), a draw's line waits for its draw to return,
  resolve lines carry the library's copy command and depth format, the library's shader survey draws are
  left out by `compare.py`. Title and menus 1.000, V-Rex 0.986, Venture 0.975 (the Xenos self-checks give
  Venture 0.988 to 0.995: rain and lightning differ from run to run), Kong 0.997.
- **The V-Rex loading screen** keeps the last frame of a fade on screen while the chapter loads, and that
  frame's darkening draws had pipelines nobody had made yet: skipped, the frozen picture was too bright
  (MAE 0.62, tile MAE 12.9). Draws now wait up to 30 ms a frame for their pipelines
  (`--native_pipeline_wait_budget_ms`, "Pipeline cache" below). Which fade frame freezes still depends on
  timing: one run in three froze a frame later (MAE 0.21, tile MAE 4.2 against a limit of 2).
- **A crash in the NVIDIA driver** at the start of Venture (`nvoglv64.dll`, inside `vkCmdPipelineBarrier2` at
  the frame command list's `close()`): textures and buffers the caches released were destroyed at once,
  while the open command list still held their state (NVRHI's Vulkan clears do not keep a reference) and
  frames in flight could still sample them through the bindless tables. They are now kept for 8 frames
  (`RetireResource`). Found from the crash dump by relinking the plugin with a map file (the build has no
  PDB): Renderer::OnPresent, CommandList::close, commitBarriersInternal.
- **Venture ran at 18 frames a second** in gameplay (Xenos: 30), every run since Phase 2 began (about 370
  hitches per Venture run against 4 on Xenos; V-Rex about 170 against 4). Two causes in the guest memory
  watches:
  - Every resolve re-armed the watch on its destination (`TextureCache::MarkGpuWritten`): about 0.8 ms each
    for a 1280x720 target, 13 ms a frame. Entries now remember that they are watched (`ResidentEntry::watched`,
    cleared by an invalidation) and are not armed again.
  - The game rewrites a 7 MB vertex buffer every three frames and each 4 KB page faulted on its own (about
    600 faults a frame; `--native_debug=8` logs the most invalidated pages and which cache owns them). The
    invalidation callback now returns the whole watched buffer range that the write touched
    (`GameRenderer::OnGuestWrite`), so the SDK unwatches it at once: one fault per buffer. Texture ranges are
    never widened, and no widening may reach over a texture: a first version that widened every watched
    range invalidated textures a resolve had written and replaced their picture with stale guest memory
    (Venture's hull too bright, MAE 14).

  Venture's gameplay now runs at 30 frames a second (about 65 frames over 50 ms per 10 s remain, 200
  hitches per run; V-Rex 7). The statistics line every 300 swaps now also gives the game thread's time in the
  renderer's draws, clears, resolves and presents and in the watches.

**Render scale in the game.** The game's own captures are the presenter's image, so at `--native_render_scale 2`
they are 2560x1440 and at 1.5 1920x1080 (`docs/testing.md` said window size; corrected there). `compare.py` then
fails every frame on `size`; the pictures were judged shot by shot instead (MAE, box-downscaled to the
reference's size) against the Xenos plugin at `--draw_resolution_scale_x/y=2` (Xenia scales by integers only)
and, for 1.5, against the golden set:

| Scene | native 2x vs Xenos 2x | native 1x vs golden (same shot, for the noise) | native 1.5x vs golden |
|---|---|---|---|
| title, video, loading, pause | 0.00 to 0.02 | 0.00 to 0.10 | 0.1 to 0.9 (resampling) |
| save, main, chapter select | 1.2 to 1.5 | 0.8 to 1.0 | 1.8 to 2.3 |
| vrex_110 / 140 / 170 | 3.1 to 3.7 (lightning frames 9 to 21) | 4.3 to 5.1 | 3.2 to 4.0 (lightning 30 to 71) |
| venture | 7.7 to 10.1 (one lightning frame 52) | 4.5 to 10.0 | 3.8 to 10.0 |
| kong_cutscene / kong | 9.5 to 13.7 | 8.0 to 12.4 | 6.5 to 12.9 |

Post effects (Kong's light shafts, the bloom, the V-Rex fog and rain lighting), shadows, the menus and the
HUD (Venture's "Right stick to aim") look the same as at 1x and as the Xenos plugin's at 2x; crops side by side
in `F:\KK-native-renderer\analysis\phase2\sbs_*.png`. The animated scenes differ by their timing as much at 2x
as at 1x. Runs: `runs\native-scale2-v2`, `runs\xenos-scale2-ref`, `runs\native-scale15`.

The first scale 2 run (`native-20261011-014649`) had a bright haze over V-Rex: not the scale but the pipeline
cache's wrong shader translations (above), which showed at scale 2 first because the cache had just been
turned on. From the hunt these stay: `--native_debug` 16 (plain pixel shaders at a scale), 32 (only
frame-wide targets scale), 64 and 128 (watch toggles), and `--native_show_texture=<address>` (shows the
texture a resolve wrote there instead of the frame).

**Tests on Windows.** The standalone build runs on Windows too, on the real GPU (no lavapipe):
`cmake -S native-renderer -B <dir> -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
-DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=tools/rexglue-vulkan/win-amd64 -DNR_BUILD_TESTS=ON
-DNR_DXC_DIR=<dxc release>/bin/x64` from a Visual Studio x64 prompt with its Clang on the PATH. On the RTX 4070 Ti
(Phase 2b, after the AO merge): `nr_game_tests` 45 tests, `nr_ao_tests` 12, `nr_render_tests` 6, `nr_draw_tests`
14, `nr_frame_log_tests` 8, `nr_pipeline_cache_tests` 6, all pass (Khronos validation not installed there).
Three test fixes came out of the first Windows run: the harness test called `python3` (now CMake's Python), and
two counts had not followed this phase (the clear as a draw in the frame log, the hook table's 42nd entry).

### Four more chapters (Phase 2b)

New golden scenes `chap04`, `chap12`, `chap20` and `chap33` (`tests/scenes.json`): chapter-select entries 4,
12, 20 and 33, the opening skipped as in the V-Rex run (six START, two B), five shots 0.5 s apart once Jack
stands still (a jungle gate at dusk, a dark path to a lit gate, a mossy rock wall over mist, a cave with a water
channel). Found with a probe scene file (shots every 5 s, `run.ps1 -ScenesFile`); golden set captured with the
Xenos plugin, and a second Xenos run repeats it closely (chap12 MAE 0.01, chap04 0.32).

| Scene | Native (default options) | Xenos self-check |
|---|---|---|
| chap04 | pass 0.0020 / 1.24 | pass 0.0004 / 0.32 |
| chap12 | **fail** 0.0029 / 2.33 (outside-range share above the 0.002 limit) | pass 0.0000 / 0.01 |
| chap20 | pass 0.0000 / 1.41 | pass |
| chap33 | pass 0.0000 / 0.71 | pass |

chap12's native frames are the golden picture moved up by exactly one pixel (chap04's by half a pixel; the
other scenes, menus and V-Rex by none), measured by the best shift between the frames. Same textures, same
brightness (channel means equal to 0.01). The Xenos plugin repeats its own frames exactly there, so the game's
camera is not random: something the game reads back from the GPU differs. The native renderer reports every
occlusion query as visible and does not write resolves back to guest memory; either could move a camera that
settles on what it sees. Open.

### Frame rate (Phase 2b)

Measured with the port's own frame statistics (`Frame times` every 10 s after `KK dev: gameplay reached`, or the
last minute of the V-Rex run), `--kk_original_look_keep=kk_frame_rate --kk_frame_rate=N`, RTX 4070 Ti, 1280x720
window. Average FPS / 1% low; the Xenos plugin at `--draw_resolution_scale_x/y`, the native renderer at
`--native_render_scale`. Runs `runs\perfA-*` (Xenos) and `runs\perfG-*` (native with the four options below);
tables in `analysis\phase2\perfA_table.txt`, `perfG_table.txt`.

| Cap, scale | V-Rex: Xenos | V-Rex: native | Venture: Xenos | Venture: native | Kong: Xenos | Kong: native |
|---|---|---|---|---|---|---|
| 60, 1 | 60.0 / 59.6 | 60.0 / 48.3 | 60.0 / 59.8 | 60.0 / 57.6 | 57.8 / 10.5 | 57.6 / 10.3 |
| 60, 2 | 60.0 / 59.8 | 60.0 / 59.9 | 60.0 / 59.9 | 60.0 / 59.9 | 57.6 / 9.5 | 57.5 / 10.3 |
| 120, 1 | 109.9 / 92.2 | 120.0 / 111.7 | 110.3 / 96.7 | 120.0 / 103.3 | 105.2 / 13.7 | 113.8 / 14.3 |
| 120, 2 | 110.0 / 92.2 | 120.0 / 110.6 | 110.3 / 104.7 | 120.0 / 106.5 | 104.8 / 12.1 | 113.8 / 14.1 |
| unlimited, 1 | 110.0 / 92.9 | 161.0 / 84.0 | 110.0 / 104.8 | 223.8 / 75.7 | 105.0 / 13.6 | 176.3 / 17.2 |
| unlimited, 2 | 110.0 / 92.2 | 159.8 / 91.9 | 109.9 / 89.2 | 232.4 / 88.8 | 105.2 / 11.9 | 173.0 / 16.9 |

(Kong's 1% low is the chapter load inside the measured minute, on both plugins. The Xenos plugin tops out at
110: its command processor waits for the presenter the way the native renderer did before the fourth option.
Unlimited on native means the game runs faster than the presenter shows frames; the presenter takes the newest.)

Where the time went, and the options that fix it (all off by default, to keep today's behaviour; turn all four
on for the numbers above):

| Option | What it fixes | Venture, unlimited |
|---|---|---|
| (none) | | 66 FPS (Xenos 110) |
| `--native_present_unlocked` | the swap held the renderer's lock through the presenter's refresh, blocking the game's draws (6.5 ms a frame in presents, 2.3 ms in clears at 60 FPS) | |
| `--native_async_submit` | the game's thread submitted each frame and waited about 6 ms for the queue, which the presenter holds; a submission thread takes three frame command lists in turn | 70 |
| `--native_dynamic_vertex_buffers` | a 4 MB particle vertex ring rewritten every frame faulted per 4 KB page (340 write faults a frame); such buffers stop being watched and each draw uploads the vertices it reads (7 to 13 MB a frame) | 107 |
| `--native_async_present` | the ring skimmer waited the presenter's refresh (7.7 ms a swap) at every swap, and the game waited for the skimmer through the ring; a present thread takes the newest swap | 224 (V-Rex 162, 1% low 84 to 90) |

Also, without an option: a draw builds its pipeline description only when the pipeline is not made yet, and
compares the constants as guest bytes before converting them. The statistics line every 300 swaps now also
gives the hooks' time and calls, the Present's parts (copy, close, submit, frame end), the queue waits, the
swap's flush and refresh times, the ring skimmer's busy and waiting time, and the slowest frames of the window
with their renderer, present, pipeline and texture time (`rexgpu-native: slowest frames:`).

With all four options the 13 golden scenes pass except `loading`, whose frozen fade frame is one fade step
later (MAE 0.21, tile MAE 4.2 against a limit of 2): the timing the frozen frame depends on moves with the
frame rate (`runs\native-fast-full2`).

### The launcher's settings and the native renderer (Phase 2b, launcher v1.9.11)

| Setting (cvar) | With `--gpu_plugin=native` |
|---|---|
| Frame rate cap (`kk_frame_rate`) | works since d4bd455: the native vblank ticks at 1000 Hz when the port turns `vsync` off, like the SDK's graphics system; 30, 60, 120 and unlimited measured below |
| Render quality / resolution (`kk_render_quality`, `resolution_scale`, `draw_resolution_scale_x/_y`) | not followed by default: the native renderer draws at `--native_render_scale` (default 1). `--native_render_scale=auto` (new) takes the scale the Xenos plugin would draw at, which the presets set. Not the default yet, to keep today's behaviour |
| Ambient occlusion (`ao_mode`, `ao_strength`) | works: read at every resolve; the same picture as the Xenos plugin's ("Ambient occlusion") |
| Upscalers (`present_effect`: FSR 1, NIS, bilinear) | work: the presenter's (`rex/ui/flags.h`), which paints the native renderer's frame image like the Xenos plugin's. Not checked in captures: the game's captures are the frame before the presenter |
| Anti-aliasing (`swap_post_effect`, FXAA) | **not applied**: FXAA is the Xenos command processor's own swap pass (`rex/graphics/command_processor.h`, `SwapPostEffect`), not the presenter's. The native renderer would need its own FXAA pass over the frame image before `CopyToGuestOutput` (to do) |
| Texture filtering (`anisotropic_override`) | wired now: every native sampler takes the override's AnisoFilter (-1, the default and the Original look's, keeps the game's own) |
| Fullscreen, window size, monitor | the app's (window and swap chain); the native frame image is made at the display size the plugin is given at start, so a resize after start keeps the old render size (scaled by the presenter) |
| Motion blur, fog, big blur, field of view (`kk_motion_blur`, `kk_fog`, `kk_big_blur`, `kk_fov`) | the game's own code (patched by the port), so the same with either plugin |
| Original look (`kk_original_look`) | sets all of the above to the console's values when the game starts; `kk_original_look_keep` (new, for tests) lists settings it leaves alone |

**Latest full run** (`runs\native-final-default`, default options, after the main merge, FXAA, the frame-rate
work and the four new chapters): 15 of 17 pass with the loading scene's new tolerance (title, menus, video,
pause, loading, the three V-Rex scenes, Venture, both Kong scenes, chap20, chap33; draw lists title and menus
1.000, V-Rex 0.994, Venture 0.989, Kong 0.985); chap04 and chap12 fail on their camera offset (below).

### The render thread (Phase 2c)

**The chap04 / chap12 offset was the game's own timing, not a readback.** Runs `runs\p2c-*`, dumps and
snapshots in `analysis\phase2c`:

- Not resolves: the Xenos plugin does not write resolves to guest memory either (the SDK's
  `readback_resolve` defaults to none); guest memory dumps at 126 s show every resolve destination of the frame
  as zeros under both plugins. Resolve writeback would therefore have made native differ from the Xenos plugin.
- Not occlusion queries or memexport: the game sends no EVENT_WRITE_ZPD, MEM_WRITE or REG_TO_MEM packets (the
  ring skimmer's statistics now count packets by opcode); the Xenos plugin with `--occlusion_query_enable=false`
  or `--readback_memexport=false` still matches the golden frames exactly. The CPU reads no viz-query status
  register (the recompiled code's only GPU register reads are vblank, viewport size, RB_BC_CONTROL and a hang
  report).
- Not the floating-point state: the hooks restore MXCSR around every handler and log a change
  (`hook_table.h`, `CallHandler`, kept); no handler ever changed it.
- Not the frame rate: the Xenos plugin at `--kk_frame_rate=60`, with the chapter start moved by 0.5 s, or with
  synchronous shader compilation gives the golden picture.
- What it is: Jack lands 0.152 units higher. Static-data snapshots (`KK_DEV_SNAP_AT`) show his height at
  0x82D64254 settling to -7.6450 under the Xenos plugin in every run and to -7.4931 under native in every run,
  with any option, the pipeline cache, without watches or pipeline waits. His first position after the spawn
  already differs in z (-174.4717 against -174.4783): he lands beside a step and that decides the side. The
  camera follows him: the picture moves up by one pixel (chap12) or half a pixel (chap04).
- What decides it (`--native_debug` bits kept for this: 256 no write watches, 512 no draws, 1024 no clears,
  2048 no resolves, 8192 a 10 µs spin per draw instead of drawing, bits 16-18 end each draw at a stage):
  draws that stop before the pipeline lookup give the Xenos value, draws that reach it or do the full work give
  the native one, `--native_draws=false` a third (-7.6754). A steady cost per draw (the spin) does not move it;
  the game thread's stalls in the chapter's first frames do (pipeline waits, texture and buffer uploads, the
  queue waits of Present), all of which the Xenos plugin does on its command processor thread.

**The render thread** (`--native_render_thread`, off by default; `backend/render_thread.{h,cpp}`). The hooks
keep the draw-state tracker on the game's thread; `ThreadedSink` sits between it and the renderer:

- At each clear, draw, resolve and Present it copies the D3D device struct (20,608 bytes) and the small objects
  the call names (render target and depth surfaces, the vertex declaration, the 16 streams' vertex buffer
  objects, a resolve's destination texture, the back and front buffers) and queues the call. A call shares the
  previous call's device copy when the parts the renderer reads are unchanged (rare: the constants change at
  almost every draw); copies hold only those parts and come from a pool. The game may run one frame (at most
  8,000 calls) ahead; two frames made unlimited frame rates more uneven.
- The render thread runs the calls through the renderer as before. Its guest object reads go through
  `SnapshotGuestMemory` (the call's copies first); vertex, index and texture data are read from guest memory
  when the call runs, as the Xenos plugin's command processor reads them.
- That is safe because the ring skimmer now waits for the render thread before it writes anything the CPU can
  see as the GPU's progress (a scratch-register fence, EVENT_WRITE_SHD, MEM_WRITE, REG_TO_MEM, COND_WRITE, the
  extent and sample-count events, an interrupt, a swap): it waits until the calls queued before that ring
  kick have run (the kick's ticket, taken at the CP_RB_WPTR write). The game reuses fenced memory (dynamic
  buffers, Lock of a busy resource) only after such a fence, so a queued draw still reads the data it was given;
  CPU writes outside that discipline reach the renderer through the write watches, as before.
- The library's vertex shader patch is hashed at once on the calling thread (the frame log's vertex shader
  hash; the library patches the microcode again for the next declaration). Shader creation stays on its thread.
- Statistics every 300 swaps: `rexgpu-native: render thread: N calls (device copies), at most M waiting; game
  thread ms copying, ms waiting for room; ring skimmer waited`. In the 17-scene run about 1 µs per call on the
  game's thread (V-Rex: 4.2 million calls, 5.0 s in 190 s).

Results (Vulkan, frame logs on):

| Run | Result |
|---|---|
| chap12, render thread only (`runs\p2c-probe-rt1`) | Jack -7.6450 as on Xenos; pass, outside 0.0000, MAE 0.033 (was a fail at 2.3) |
| 17 scenes, render thread + the four speed options (`runs\p2c-rt-fast-full`) | 15/17: chap04 pass 0.0020 / 1.09, chap12 pass 0.0000 / 0.03, draw lists 0.988 / 0.989; Venture 2/5 (two lightning frames), the loading screen's frozen fade frame a step later |
| V-Rex + Venture, render thread only (`runs\p2c-rt-only-vv`) | 6/6, loading MAE 0.011, Venture 5/5 |
| V-Rex + Venture, render thread + four options, again (`runs\p2c-rt-fast-vv2`) | 6/6, loading MAE 0.011 |
| 17 scenes, render thread + four options, again (`runs\p2c-rt-fast-full2`) | **17/17**; chap04 0.0019 / 1.07, chap12 0.0000 / 0.89 (later frames 0.03) |
| 17 scenes, final build: one frame ahead, partial copies, three frames in the submitter (`runs\p2c-rt-fast-full3`) | **17/17**; chap04 0.0015 / 0.83 (draw list 0.984), chap12 0.0000 / 0.028 (0.995), loading 0.011, Kong 0.991 |

The two misses of the first full run were a lightning flash in two of Venture's five frames and the loading
screen's sweep one step further (both timing; neither came back in the next three runs).

**Frame rate** (Vulkan, the measurement of "Frame rate (Phase 2b)": `--kk_original_look_keep=kk_frame_rate
--kk_frame_rate=N`, the port's 10 s `Frame times` windows after `KK dev: gameplay reached` without its first
window, V-Rex's last minute; average FPS / mean of the windows' 1% lows / lowest window's 1% low). Final
build, runs `runs\perfC-final-*`, table in `analysis\phase2c\perfC_final_vulkan_table.txt`:

| Cap | V-Rex: four options | V-Rex: + render thread | Venture: four | Venture: + RT | Kong: four | Kong: + RT |
|---|---|---|---|---|---|---|
| 60 | 60.0 / 59.8 / 59.4 | 60.0 / 59.9 / 59.8 | 60.0 / 59.5 / 59.4 | 60.0 / 59.8 / 59.8 | 60.0 / 56.5 | 60.0 / 59.9 |
| 120 | 120.0 / 109.9 / 94.6 | 120.0 / 117.2 / 114.0 | 120.0 / 112.9 / 106.3 | 120.0 / 117.7 / 115.8 | 119.8 / 95.5 | 120.0 / 113.3 |
| unlimited | 164.2 / 115.5 / 108.4 | 216.2 / 98.4 / 80.7 | 225.6 / 92.4 / 77.3 | 236.7 / 130.3 / 127.4 | 199.5 / 17.8 | 215.2 / 19.0 |

(Kong's single window holds the chapter load; its 1% low is the load on every configuration.) With a cap the
render thread is better everywhere: the 1% lows come within 1 to 6 FPS of the cap. Unlimited, it is 3 to
32% faster on average and Venture's 1% low is much higher; V-Rex's 1% low is lower (98 against 116 on
average over the windows). There the frame rate is the GPU queue's: the submission thread waits for the
queue 85% of the time, which the presenter's refreshes share (92 s of 210 s), and the render thread's slowest
frames (9 ms) are its waits for the submitter. The first measurement of the four options here
(`runs\perfC-four-r0`: V-Rex 155 / 84) was before the submitter kept three frames instead of two; that
change alone raised V-Rex's unlimited 1% low to 110-120 (`runs\perfC-four3-r0`).

### Open problems (Phase 2)

- **chap04 and chap12 cameras**: solved by the render thread (`--native_render_thread`, "The render thread
  (Phase 2c)" above).
- **Defaults.** The four frame-rate options and `--native_render_scale=auto` are off by default (today's
  behaviour); turning them on is the next decision. The 13 golden scenes pass with all four on, except the
  loading screen's fade frame before its new tolerance.
- **FXAA** matches the Xenos plugin's in kind but changes fewer pixels on small text.

- **Cold pipeline caches.** Pipelines are made on four worker threads (Vulkan) and draws wait up to 30 ms a
  frame for them, then skip; the pipeline cache (on by default) makes the descriptions of earlier runs at
  start. A truly cold driver cache was not measured again (see "Pipeline cache"). On D3D12 pipelines are
  still made on the draw thread (NVRHI's D3D12 backend caches root signatures without a lock).
- **Write watches.** Venture's gameplay still has about 10,000 invalidations a second and its movie about 360
  a frame (texture pages; textures are never unwatched whole, see Phase 2b above). The movie planes could
  skip the watches (their Lock / Unlock can be hooked).
- **The Xenos plugin leaves a draw's pixel shader out of its log** when the draw writes no colour (no target,
  no kill or depth output, Xenia's "pixel shader not needed"); the native log keeps it. One line in Venture.
- **Linux tests and the endian default.** `nr_game_tests` (Linux, lavapipe) were written with the buffer's
  endian; with `element_endian` on by default, a test whose elements' declared endian differs from the
  buffer's would now read differently. Not run here (no Linux).
- **Stencil in depth resolves seen as colour** is written as 0 (the D32S8 host depth keeps it, the blit does not
  read it).
- **Conditional rendering and the occlusion surveys** are ignored: everything is drawn (same picture, more work).


## Ambient occlusion (session F)

The native renderer has the same screen-space AO as the Xenos plugin (patches 0004 to 0007 in
`tools/rexglue-patches`). Code: `backend/ambient_occlusion.{h,cpp}`, shaders:
`backend/shaders/ambient_occlusion.hlsl`, tests: `backend/tests/ao_tests.cpp` (`nr_ao_tests`).

**Settings.** `ao_mode` (0 off, 1 on, 2 show the AO only) and `ao_strength` (1 to 3) are read each
frame with `rex::cvar::GetFlagByName`, the way the launcher sets them. They are defined by the
Xenos plugin (rexgpu-xenos), which the launcher preloads, so the Graphics page keeps working. Off
(the default) costs nothing and changes nothing.

**Detection** (in `GameRenderer::Resolve`, the same rules as the plugin). A full-screen D24S8 depth
resolve (at least 1280 x 720, from the top-left corner) is noticed and the depth is copied at once,
because the game clears it right after. The next full-screen 8:8:8:8 colour resolve of that frame
gets the occlusion; later colour resolves to the same destination get it too, the final
post-processed copy goes elsewhere so the HUD is untouched. The two AfterEffects fog pixel shaders
(ucode hashes 0x1B65AC7429B67FE9 and 0x9F699E763A41FE07) are recognised in `Draw` and their
constants read, so the fog colour is kept: `colour * ao + fog colour * fog * (1 - ao)`.

**Passes** (all full-screen pixel shaders, no compute): depth copy, linear half-size depth, 10-sample
occlusion on a normal rebuilt from depth, depth-aware 9-tap blur each way, edge-aware 4-tap upscale
with the distance fade, then the apply. The apply writes into a scratch copy of the colour target
and the resolve reads that copy, so the game's own target is never changed (the plugin dumps a
fresh copy per resolve; this matches it without compounding).

**Render scale.** Every image is sized from the host texture it is given, so a bigger target gives
a bigger half-size image. The blur taps and the sample radius keep their size in the picture. The
scale is worked out from the texture size against the guest size, so this works with or without
session E's `render_scale.h`. Since the merge (Phase 2b) `AoResolve` width and height in `Resolve` come
from E's `ScaleRect` of the guest rectangle with the source target's scale, the rounding every other
scaled rectangle uses.

**Tests.** `nr_ao_tests` compares the GPU output with a CPU reference of the whole pipeline on
synthetic scenes (a box on a wall, a floor, sky, a far strip, fog), and checks detection rules,
the untouched game target, the depth copy, off, and 1x against 2x. Run on SwiftShader (lavapipe
was not installable in the cloud container): GPU and reference agree to a few levels. On Windows on
the RTX 4070 Ti (Phase 2b): 12 tests pass (scale 2 against 1: mean 0.008, 893 pixels off by more than 0.1).

**In the game (Phase 2b).** `--kk_original_look=true` (every golden run) forces `ao_mode=0`, so AO given on
the command line was silently off; `--kk_original_look_keep=ao_mode,ao_strength` (new, `kk/src/settings.cpp`)
keeps them. (Turning the Original look off instead changes the menus the scripts walk: the "vrex" run lands in
Venture and the "rescue" run stays black, on both plugins.) Both plugins, the same shots, MAE between the
plugins' frames (V-Rex / Venture; the animation's timing alone gives about 3 to 5 at 1x without AO):

| Setting | V-Rex | Venture | Kong to the Rescue |
|---|---|---|---|
| ao_mode 2 (AO only), strength 2 | 1.5 to 2.5: the same occlusion image (crops `analysis\phase2\sbs_ao3_m2_*.png`) | 0.1 | no AO on either plugin (no full-screen D24S8 depth resolve is detected there) |
| ao_mode 1, strength 1 | 3.1 to 4.8 | 3.0 to 6.3 | as off |
| ao_mode 1, strength 2 | 4.1 to 7.0 | 2.9 to 5.1 | as off |
| ao_mode 1, strength 3 | 2.3 to 2.7 | 5.1 to 6.1 | as off |

(One V-Rex frame of strengths 1 and 2 has a lightning flash on one side, 28 to 32.) Strength 3 against AO off
on the native renderer is 5 in V-Rex, so the AO itself is visible and matches the Xenos plugin's by eye: the
same darkening under the beams, in the grass and around the rocks. Runs `runs\ao3-native-*`, `runs\ao3-xenos-*`.

**Shaders.** `compile_shaders.sh` / `.ps1` build `ao_*` SPIR-V and DXIL. The DXIL is now the official
DXC release's (v1.9.2609, regenerated on Windows with `compile_shaders.ps1 -Dxc <dxc.exe>` after the merge;
the cloud session's source build could not sign it). The SPIR-V came out byte-identical.

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

Packages: `cmake ninja-build clang-20 lld-20` (or g++), `libvulkan-dev mesa-vulkan-drivers
vulkan-validationlayers spirv-tools`, and for the plugin `libx11-dev libx11-xcb-dev libwayland-dev
libxkbcommon-dev`. The game renderer (Phase 2) also needs DXC: the Linux SDK bundle has no `dxcapi.h`, so
`NR_DXC_DIR` names a DXC release folder (`include/dxc/dxcapi.h`, `lib/libdxcompiler.so`, `lib/libdxil.so`;
v1.8.2505.1 here). Without it the game renderer is left out (`NR_GAME_RENDERER OFF`, the placeholder only).

The SDK, built from source with every patch (the official bundle lacks the Vulkan features NVRHI needs, see
above):

```
# from the repository root (the patch paths below are relative to it)
git clone --branch v0.10.0 https://github.com/rexglue/rexglue-sdk sdk-src && cd sdk-src
git submodule update --init --recursive
for p in ../tools/rexglue-patches/0*.patch ../native-renderer/sdk-patches/0*.patch; do git am -3 "$p"; done
cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
      -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_STANDARD=23 \
      -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_INSTALL_PREFIX=/opt/rexsdk/linux-amd64
cmake --build out/build && cmake --install out/build
```

Then native-renderer against it:

```
cmake -S native-renderer -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
      -DCMAKE_PREFIX_PATH=/opt/rexsdk/linux-amd64 -DNR_DXC_DIR=/opt/dxc
cmake --build build
ctest --test-dir build --output-on-failure
```

The build type must be the SDK's: the runtime's plugin loader adds the postfix of the configuration
`rexruntime` was built in (`d` Debug, `rd` RelWithDebInfo, none for Release), so a RelWithDebInfo plugin
(`librexgpu-nativerd.so`) next to a Release SDK is "not found" and `nr_plugin_tests` fails.

Without the SDK on the prefix path the plugin is skipped and the libraries and GPU tests still build
(Vulkan-Headers 1.3.318 or newer are needed: the bundle's, or `-DNR_VULKAN_HEADERS=<dir>`).

| Test | Checks |
|---|---|
| `nr_draw_tests` | the hook table (sorted, complete, matches `kk_native_hooks.cpp`), log parsing, the per-draw sequence from `d3d-api-map.md` replayed as a trace and every field of the resulting records, constants copied at call time, every render / sampler state setter, nesting, an inactive or absent plugin, dropped draws, primitive expansion, the decoders |
| `nr_render_tests` | the test host: its own Vulkan 1.3 device on lavapipe, NVRHI on it (plus NVRHI's and Khronos' validation, which must stay silent). M1: clear colour read back on every pixel; the per-frame colour. M2: the triangle's centroid, vertex colours (orientation), outside pixels, coverage. M3: the recorded frame through hook table, NrApi, tracker and renderer: what reached NVRHI (pipeline, framebuffer, viewport, index buffer, draw arguments, push constants) and the pixels (triangle placed by c0..c3 with the translation, the render-to-texture draw skipped on frame 2); the viewport; the transpose option |
| `nr_plugin_tests` | with the SDK: the runtime's `LoadGpuPlugin` loads `librexgpu-native*.so`, `nr_get_api` is found as the hooks find it and is inactive before setup; `d3d12` is refused; NVRHI on the SDK's own `VulkanProvider` (headless, lavapipe) renders M1, M2 and the M3 frame correctly. Run under the Khronos layer; fails on any validation error, so it passes only with the SDK patch |
| `nr_spirv_val` | the checked-in SPIR-V validates for Vulkan 1.2 |
| `nr_game_tests` | the game renderer without the game (needs `NR_GAME_RENDERER`): a 20,608-byte device struct in synthetic guest memory, shader objects and containers from the shader tests' microcode assembler and container writer (`shaders/tests/xenos_asm`, `container_writer`), textures, vertex and index buffers laid out as `d3d-structs.md` says. A helper plays the D3D library: each setter writes the register images and D3D fields the real one writes, then calls the hook (hook table, NrApi, tracker, Renderer, GameRenderer; shader creation to the ShaderLibrary, DXC at run time). Checked by reading pixels back from lavapipe: clears (whole target, rectangles, the viewport), vertex colours and orientation (D3DCOLOR, +y up), indexed draws (16 / 32-bit, base vertex, start index), point / quad / fan / strip, stream offsets, vertex and pixel constants per draw, blend (add, alpha, reverse subtract), depth (less, always without writes, greater, depth-only clear, test off), cull (front, back, both faces' windings), colour mask, MRT with a per-target mask, render targets keyed by EDRAM base / pitch / format (shared by two surface objects, a new one per base, format and pitch, depth apart, re-created taller), resolves into k_8_8_8_8 endian-none, A8R8G8B8 (8in32, R and B exchanged) and k_8 textures sampled back, resolve rectangle and destination point, resolve colour and depth clears, a shader parsed from its object without the create hook, a tiled guest texture uploaded, sampled, rewritten and re-uploaded after `InvalidateRange`. Every test checks that the Khronos and NVRHI layers stayed silent |

`NR_TEST_IMAGES=<dir>` makes `nr_render_tests` and `nr_game_tests` write the read-back images as PPM files.
`NR_TEST_SYNC_PIPELINES=1` makes `nr_game_tests` create pipelines on the draw thread (the renderer's default skips
a draw while its pipeline is built on a worker, and the tests read the first frame back), and
`NR_TEST_VERBOSE=1` prints the renderer's draw and skip counts after each test.

**No lavapipe (the Phase 2b cloud container).** The container has no `mesa-vulkan-drivers` (its apt mirrors are
refused) but Playwright's Chromium ships SwiftShader (`vk_swiftshader_icd.json`, `VK_ICD_FILENAMES=...`). It
works for `nr_draw_tests` and `nr_render_tests`, but reports `maxBoundDescriptorSets = 4` while the game renderer
uses 7 sets (set 0 plus the six bindless tables), and writing past its fixed array corrupts the heap, so
`nr_game_tests` crashes in `vkCmdBindDescriptorSets` (glibc "malloc(): invalid" aborts, not a validation error).
SwiftShader built from source with `MAX_BOUND_DESCRIPTOR_SETS = 8` (`src/Vulkan/VkConfig.hpp`, the only change, as
`tests/swiftshader-8-descriptor-sets.patch`;
`cmake -DREACTOR_BACKEND=Subzero -DSWIFTSHADER_BUILD_TESTS=OFF ... && ninja vk_swiftshader`, about 25 minutes on
three cores) runs the whole game suite: 27 tests, 0 failed checks, with `NR_TEST_SYNC_PIPELINES=1`. It has no
Khronos validation layer, so the "validation stayed silent" half of the checks did not run there; lavapipe with
the layer remains the reference run.

The test device (`backend/tests/vk_test_device.cpp`) enables what the SDK's device enables for the plugin:
timeline semaphores, synchronization2 and dynamic rendering, the descriptor indexing features
(`sdk-patches/0002`), and the Vulkan 1.0 features the SDK turns on for GPU emulation that the game renderer
uses (clip distances in the translated vertex shaders, independent blend for MRT, depth clamp, fill mode,
anisotropy). Without them the layer reports `VUID-VkShaderModuleCreateInfo-pCode-08740` (ClipDistance) and
`VUID-VkPipelineColorBlendStateCreateInfo-pAttachments-00605`.
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
  render targets are pooled. (`nr_game_tests` now checks both against synthetic scenes on Linux, plus
  A8R8G8B8; a frame of the game is still the real proof.)
- D3D12 and the sampler table (found on Linux, not tried on Windows): the game renderer's set 5 is an
  immutable bindless layout holding samplers. NVRHI's D3D12 backend gives the root parameter a SAMPLER range
  but binds every immutable table from the CBV/SRV/UAV heap (`d3d12-resource-bindings.cpp`,
  `SetGraphicsRootDescriptorTable(..., shaderResourceViewHeap.getGpuHandle(...))`; only `MutableSampler`
  layouts use the sampler heap), and NVRHI's validation layer refuses the layout outright on D3D12. Run the
  game renderer on Windows with `--native_validation` and the D3D12 debug layer to see whether it works
  there; if not, the samplers need the sampler heap (a `MutableSampler` layout with `SamplerDescriptorHeap[]`
  in the shaders, or static samplers).

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
`F:\KK-native-renderer\analysis\04\sheet_vulkan_final.png`, `sheet_vulkan.png`. (That run predates the native frame log.)

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

## The frame log

`REX_DEV_FRAME_LOG=<seconds>[,<frames>]` (same meaning as on the Xenos plugin) makes the native plugin log one
`Frame log: ...` line per draw and resolve at warning level, in the format `compare.py framelog` reads. Code:
`backend/frame_log.*` (formatting, clock, frame selection) and `backend/game_frame_log.cpp` (the hooks).
Checked against the Xenos logs of the golden scenes (Phase 2b):
- A game Clear is the library's own draw on the Xenos plugin: pixel shader 2B4F1D2C5EDCC366, vertex shader
  6DD9DD041F740550, three vertices (a rectangle list), depth test and write on when it clears depth or stencil,
  and the colour targets that are both flagged and bound. The native log writes the same line.
- The pixel shader hash is the database (template) hash.
- The vertex shader hash is XXH3 of the microcode the library patched for the draw: inside its draw it copies
  the shader's microcode into the command ring (`sub_82120608`, an IM_LOAD_IMMEDIATE packet) or uses the
  object's copy (`sub_82120A70`), and `sub_82120028` rewrites the fetches for the declaration and strides. A hook
  after `sub_82120028` (NrApi 4, `vertex_shader_patched`) hashes the patched bytes and keeps the hash per
  (shader, declaration, strides at device +12688); the library only patches a new combination. Since the patch
  runs inside the draw, after the draw's hook, a draw's line is held until the next draw, clear, resolve or
  present (`FlushPendingDraw`). Hashing the object's microcode at the draw (the first attempt) gave the
  previous draw's patch for a shader drawn with two declarations (Venture's shadow pass: draw list 0.915);
  the patch hook gives the Xenos hash for every draw of the title, menus, V-Rex and Venture. Fallbacks: the
  object's microcode, then the reproduced patching below (the tests' synthetic device has no library).
- Resolve lines: the copy command is the library's (convert for every colour resolve, raw for depth, as the
  Xenos logs show it whatever the formats), and a depth resolve's destination format is k_8_8_8_8 (6).
- The library draws a few shaders with colour writes off, no depth and no targets, one or no vertices (its own
  survey of shaders; the Xenos log has them, the native plugin never sees them). `compare.py` leaves such draws
  out on both sides.
- The Present front-buffer copy is a resolve line; other resolves use command 0/1 from `PlanResolveConversion`.
- `--native_dump_frame=N` also logs three vertex hashes per draw: database, object memory and the reproduced patch.

### The vertex shader hash

The Xenos log hashes the vertex microcode after the library patched its vfetch instructions for the declaration.
`kkshaders/vertex_patch.*` reproduces it from `d3d-api-map.md` and `d3d-structs.md`: per full fetch the format,
integer and signed bits, offset/4, fetch slot (95 - stream) and stride/4; mini fetches keep the previous slot and
stride. The hash is XXH3-64 of the big-endian bytes. Fields not confirmed by the game are listed by
`kkshaders vfetch-check`, which patches the first container of each database entry with the dummy declaration of
the second and prints the differing bits. If they differ, the alternatives are the PM4 IM_LOAD path or hashing
the in-place patched object memory (already logged under `--native_dump_frame`).

## Pipeline cache

Options: `--native_pipeline_cache` (on by default since Phase 2b), `--native_pipeline_cache_dir=<dir>` (default
`<cache root>/native-pipelines/<TITLE8>.pipelines`, set through `InitializeShaderStorage`: the game calls it,
`555307D3.pipelines` under `--cache_root`), `--native_pipeline_wait` (off), `--native_pipeline_wait_budget_ms`
(30).

Measured on Kong to the Rescue (`rescue` run, Vulkan, Phase 2b): without the cache the chapter start has 317
draws skipped while their pipelines were made (139 drawn after waiting within the budget, 185 ms waited in all,
241 pipelines, 510 ms of creation on the workers); with the cache recorded by one run, the next run queues the
238 descriptions at start and no draw is skipped or waits (0 and 0; 2,888 ms of creation on the workers,
spread over the start). Both pass; the hitches of the chapter load are the same (worst 200 ms, as on
Xenos). The cold stall that made Kong miss its shots in Phase 2 (about 9 s) is gone with the workers alone:
the draws no longer wait for the driver. `__GL_SHADER_DISK_CACHE=0` does not switch NVIDIA's Windows driver
cache off (creation time unchanged), so a truly cold driver was not measured again; on a cold driver the
budget caps the wait at 30 ms a frame and the rest of the draws skip a frame or two. The cache is on by
default: a second session of a chapter then draws everything from its first frame.

**Found with the cache on (record version 2).** Version 1 named a record's shaders by their microcode hash
only, and the prewarm asked the pack for "any translation of this microcode". A pack made with
`--also-scaled` holds two or more translations of many pixel shaders (render scale aware or not, other
interpolators: 8,929 translations of 6,828 microcode and input pairs), and the shader library also kept one
shader per microcode, so the prewarm's pick became the draw's shader as well. V-Rex then drew its fog and rain
lighting with the wrong translation: a bright haze over the left of the picture, at scale 1 once the cache
had records (MAE 90 against 3.4 with `--native_pipeline_cache=false`) and at every render scale. Records now
carry both translations' input hashes (`vs_input`, `ps_input`; 144 bytes, version 2, older files are
ignored), the library keys shaders by microcode and translation inputs, and the prewarm asks the pack for
exactly that translation.

The test harness turns the cache off for native runs (`docs/testing.md`): with every pipeline made in
advance, Kong to the Rescue's chapter loads faster and its scripted animation is ahead of the golden frames
(0/5 frames twice, 5/5 with the cache off).
- Every pipeline description is appended once to the file as a 128-byte `PipelineRecord` (16-byte header: magic,
  version 1, record size, byte-order marker). A torn tail is repaired on open; a version mismatch starts a new file.
- At start all records are queued on the pipeline worker threads. Shaders come from the pack by hash, or the
  record waits until the game creates the shader (retried at the end of each frame).
- Without `pipeline_wait` a draw whose pipeline is not ready waits within the frame's budget
  (`--native_pipeline_wait_budget_ms`, default 30 ms for all draws of a frame together) and is skipped when the
  budget is spent (`stats().skipped_pending`); with `pipeline_wait` the draw always waits
  (`stats().pipeline_waits`, `pipeline_wait_ms`). The budget is there for the V-Rex loading screen: the game leaves
  the last frame of a fade on screen during the load, and that frame needs pipelines nothing drew before. With
  skipping alone (budget 0) the frame missed its darkening draws and the loading scene failed (MAE 0.62, tile MAE
  12.9); the workers make a pipeline in about 4 ms (1351 ms for 341 at the chapter start), so the budget lets
  most new pipelines draw in their first frame and caps a burst at one slower frame.
- Prewarm needs Vulkan with async pipelines; the D3D12 backend is not supported for it.
