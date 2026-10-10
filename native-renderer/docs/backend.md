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
  version (logged, originals only).

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
  chapter). A pipeline cache of our own, or creating pipelines off the render thread, is still to do.

### Settings

`--native_game` (on; off = the placeholder), `--native_shader_pack`, `--native_dxc`, `--native_element_endian`
(on), `--native_flip_front_face`, `--native_dump_frame=N` (every clear, draw with its textures and vertex
fetch, resolve with its destination, of frame N), `--native_debug` (bits: 1 green clears, 2 magenta frame
image under the back buffer, 4 no gamma ramp). The stats line every 300 swaps counts draws, skips, pipelines,
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

### Open problems (Phase 2)

- **Cold pipeline caches.** Pipelines are created on the render thread when a draw first needs them; the
  first visit to a chapter on a machine whose driver has not seen them stalls for seconds (Kong to the
  Rescue: about 9 s on Vulkan, 4 s on D3D12). Creating pipelines on a worker (and skipping the draw until it
  is ready, as the Xenos plugin can), or recording the pipeline descriptions and creating them at start,
  would fix it.
- **Write watches during movies.** The movie planes are rewritten every frame; each page write faults once
  per frame (about 360 invalidations a frame in the Venture opening's movie, 3 texture uploads of 3 ms each).
  Fine at 30 frames a second, worth a look for higher frame rates (the planes could skip the watches: their
  Lock / Unlock is hooked-able).
- **Draw-list comparison.** The native plugin does not write `REX_DEV_FRAME_LOG` lines yet, so the harness
  compares pictures only (runs use `-NoFrameLog`). The Xenos log hashes the vertex shader microcode after the
  library patched it for the declaration, which the native side would need to reproduce.
- **Linux tests and the endian default.** `nr_game_tests` (Linux, lavapipe) were written with the buffer's
  endian; with `element_endian` on by default, a test whose elements' declared endian differs from the
  buffer's would now read differently. Not run here (no Linux).
- **Stencil in depth resolves seen as colour** is written as 0 (the D32S8 host depth keeps it, the blit does not
  read it).
- **Conditional rendering and the occlusion surveys** are ignored: everything is drawn (same picture, more work).


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
