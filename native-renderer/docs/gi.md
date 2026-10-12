# Screen-space global illumination (beta)

Status, 12 October 2026: branch `beta-ssgi` (from `beta` = `nr-integration` 1ba7339). Spatial only (no
temporal accumulation yet: the per-pixel motion vectors come with branch `beta-upscalers`). Off by default
(`--native_gi=off`); the launcher's Graphics page has the row "Global illumination: Off / Low / Medium / High"
next to Ambient occlusion. Ray-traced GI is for later; this is screen space only.

## Why

King Kong (2005) lights nearly all its world with one shader family, `psgeneric` (`ao.md`, "The lighting
shaders"): a sun light with shadow maps, per-vertex baked light ("RLI") and one flat constant,
`g_vAmbientColor`, for all the indirect light. In the shade a surface is its texture times that constant, the same
in a crevice as in the open, whatever stands next to it: shaded areas look flat. A screen-space GI adds one bounce
of light from the surfaces on the screen (sunlit rock tints the shadow beside it, the jungle's green reaches Jack)
and takes the flat ambient light away where the surroundings hide the sky (better local occlusion).

## What is where

| Path | What |
|---|---|
| `backend/gi_config.*` | the settings (`GiConfig`), the levels (`GiParamsFor`) and their names |
| `backend/shaders/ssgi.hlsl`, `generated/ssgi_*` | the GI's compute passes and its composite; SPIR-V and DXIL like every renderer shader |
| `backend/ssgi.*` | `Ssgi`: the passes with NVRHI (images, dispatches, the composite) |
| `backend/gtao.*` | `Gtao::Prepare`: XeGTAO's first three passes (the depth, its mips, the normals), shared with the AO |
| `backend/scene_gi.*` | `SceneGi`: when in the frame the light is gathered and where it goes, the frame's ambient light |
| `backend/game_gi.cpp` | the game renderer's hook bodies (draw, clear, resolve, Present) |
| `backend/shader_library.*` | `GameShader::ambient_constant`: the register of `g_vAmbientColor` in each pixel shader's constant table |
| `backend/tests/game_gi_tests.cpp` | the whole path through the game renderer on the GPU (`nr_game_gi_tests`) |
| `tests/gigen.py`, `tests/gi_tour.json` | the chapters the GI is judged on (one launch per setting) |
| `kk/src/launcher.cpp`, `launcher_text_table.inc`, `settings.cpp` | the launcher's row, its text, the Original look turning it off |

Hook points in the renderer's files are one line each and marked `GI hook (game_gi.cpp)`: `game_renderer.cpp`
(Initialize; Draw, right after the AO's hook), `game_passes.cpp` (Clear, Resolve, Present). The AO's hook code
was only touched to share the draw description (`AoDescribeDraw`). The GI keeps its own copies of the AO's rule
table, frame tracker and camera finder, so turning one on never changes the other.

## The design

### When in the frame

The same moment as the ambient occlusion's `apply=opaque` (`ao.md`, "When in the frame"): at the end of the
scene's opaque pass, which in King Kong is the draw before its AfterEffects fog pass (the classifier's `end`
rule), else the first transparent or effect draw into the scene's targets. At that moment the scene's colour
target holds every lit opaque surface of this frame, and nothing drawn later (water, Kong's fur, smoke, fire,
rain, light shafts, the sky, the HUD) is in it yet. So:

- the bounce light comes from this frame's lit colour: no lag, no reprojection, no motion vectors needed;
- everything drawn after the moment is drawn over the result and never touched;
- a frame the rules do not cut gets it at Present when the scene target still holds the scene alone (as the AO).

### The gather

On the depth XeGTAO already prepares (`Gtao::Prepare`: the stored depth into a working image, view-space depth
with four smaller mips, normals rebuilt from the depth; full or half resolution of the scene target):

1. `cs_radiance`: the scene colour over the picture into a working-size image (RGBA16F), the mean of each
   working pixel's host pixels; `cs_radiance_down` makes two smaller mips.
2. `cs_gather`: a visibility bitmask over GTAO's slices (Therrien, Levesque and Gilet, "Screen Space Indirect
   Lighting with Visibility Bitmask", 2023). Per working pixel, `slices` directions on the screen (with a fixed
   Hilbert/R2 noise, as XeGTAO's), `steps` samples on each side within the radius (world units, clamped to a
   fifth of the picture's height on the screen). Each slice keeps 32 sectors of the pixel's hemisphere. A sample
   covers the sectors between its front (the surface on the screen) and its back (that surface pushed
   `thickness` away from the camera); only sectors no nearer sample has covered let its light through, and only
   if the sample's surface faces the pixel. The sectors are spaced evenly in the slice's own measure of light
   (GTAO's integrand: the cosine to the normal times the sine of the angle from the view vector), so each one is
   the same share of the slice's light; slices are weighted by that measure's total times the projected normal's
   length, as GTAO does. The result: rgb = the light that reaches the pixel from what hides its sky (in the
   scene colour's units), a = the share of its hemisphere nothing hides (the visibility). Flat surfaces do not
   hide their own sky (sector edges are rounded to the nearest boundary, so a sample grazing the horizon covers
   nothing).
3. `cs_denoise`, 2 or 3 times: a 5x5 a-trous blur with steps 1, 2, 4, weighted by depth and normal (edges stay).
4. `ps_composite` (a full-screen triangle over the picture, the colour target's alpha left alone), reading a
   copy of the colour: the result upsampled by depth (as GTAO's `cs_output`) and added to the surface.

### The composite: telling surface colour from light

The lighting shaders give a surface colour = albedo x (ambient + direct light). The bounce light has to be
multiplied by the albedo, which a forward renderer does not keep. The composite estimates it from the colour and
the frame's flat ambient light `A`:

- the frame's `A` is read from the scene's lit draws: the renderer finds `g_vAmbientColor` in each pixel
  shader's own constant table (`GameShader::ambient_constant`) and reads that constant at every draw of the
  scene (the mean of the frame; the last frame's when a frame has none);
- where a surface is in the shade its colour is about albedo x `A`, so colour / `A` is its albedo;
- where it is lit its colour is brighter than (albedo limit) x `A`, and its light is taken as its brightness over
  the limit (`--native_gi_albedo`, 0.6): the estimate never exceeds the limit and is exact in the shade, which is
  where indirect light matters;
- out = colour + albedo x (bounce x intensity - `A` x (1 - visibility) x occlusion).

The second term is the "better local occlusion": the flat ambient light taken away where the surroundings hide
the sky, and replaced by the light those surroundings send. With the ambient occlusion setting on (either
method) that term is left out (occlusion 0): the AO already darkens there, and darkening twice would be too much.

Water, fur, smoke, fire, rain, light shafts, the sky and the HUD are drawn after the moment: untouched. The sky's
pixels (stored depth at the far plane) are left alone, and the sky does not send bounce light (its depth is out
of every radius; the flat ambient constant stands for the sky).

### Levels

| Level | Slices x steps per side | Blur passes | Resolution | Radius (world units) |
|---|---|---|---|---|
| Low | 2 x 4 | 2 | half | 1.5 |
| Medium | 3 x 6 | 3 | half | 2.0 |
| High | 4 x 8 | 3 | full up to a render scale of 1.5, half above | 2.5 |

Half resolution is of the scene target at the render scale (at scale 2, 1280 x 720 working pixels), which is
also the path for slower GPUs (the Steam Deck class): at 1280 x 800 Low works on 640 x 400 pixels.

## Settings

| Setting | Values (default first) | What it does |
|---|---|---|
| `native_gi` | `off`, `low`, `medium`, `high` | the launcher's row; off creates nothing of it |
| `--native_gi_intensity` | 1, 0 to 8 | the bounce light times this |
| `--native_gi_quality` | `auto`, `half`, `full` | the working resolution (auto: the level's) |
| `--native_gi_radius` | 0, world units | how far light is gathered from (0: the level's) |
| `--native_gi_thickness` | 0, world units | how deep a surface on the screen is taken to be (0: a quarter of the radius) |
| `--native_gi_albedo` | 0.6, 0.05 to 1 | the brightest albedo the composite assumes |
| `--native_gi_occlusion` | 1, 0 to 1 | with the AO off: how much of the hidden ambient light is taken away |
| `--native_gi_debug` | 0 to 3 | 1 the bounce light alone, 2 the change on grey (+ brighter, - darker), 3 the visibility |
| `--native_gi_gpu_time` | false, true | GPU timer queries around the GI's passes, the average logged every 600 frames |

All are read at start (`kInitOnly`). The launcher writes `native_gi` into the settings file only when it is not
`off`; a build without the native renderer does not know the setting, warns "unknown cvar" once and ignores it.
The Original look turns it off (`OriginalLookSettings`); runs over the Original look keep it with
`--kk_original_look_keep=native_gi`.

`--native_dump_frame=N` (or `--native_ao_dump_every`) logs a line `GI: bounce light added before draw ...` with the
ambient light used; every 600 frames one line says where the light went (`GI last 600 frames: ...`).

## Tests

`nr_game_gi_tests` (the whole path through the hooks, the tracker, the game renderer and translated shaders, on
the RTX 4070 Ti; lavapipe on Linux). The scene: a white floor (albedo 0.8) in the shade and a red wall in the sun
standing on it, a depth pre-pass, the lit pass (its pixel shader's constant table names `g_vAmbientColor` at c0,
set to 0.3), then an alpha-blended quad over the floor by the wall. The tests' radius is 1 world unit.

| Test | Checks |
|---|---|
| `GameGi_RedWallBleedsOntoTheFloor` | Low, Medium, High: the floor 0.25 units from the wall gains red (0.239 to 0.33-0.35) and almost no green or blue; the floor 1.25 units away and the sky do not change; the ambient light came from the constant |
| `GameGi_TransparentDrawAfterIsUntouched` | the quad is half its colour plus half the lit scene behind it (within 0.002), where painting over the finished picture would move it by 0.03 |
| `GameGi_WithAoItOnlyAddsLight` | with GTAO at the opaque moment the GI adds red and takes nothing away |
| `GameGi_VisibilityTakesTheHiddenAmbientAway` | with the AO off, by the wall green and blue get darker (the wall hides the sky) while red rises; the open floor is unchanged; the debug view shows the red bounce |
| `GameGi_RenderScaleTwoMatchesOne` | Medium and High: mean difference 0.0012 to 0.0018 after an area average |
| `GameGi_OffChangesNothing` | off: nothing made, the frame equal to the bit |

The AO's tests (`nr_gtao_tests`, `nr_game_ao_tests`, `nr_ao_rules_tests`) pass unchanged after the `Prepare`
split.

## Decisions

- The composite at the fog-pass moment, not the lighting shaders' ambient term, for the beta: it needs no
  motion vectors (this frame's lit colour), covers every opaque draw (foliage, characters and the walls drawn
  with depth writes after the pre-pass included, which the ambient-only hook misses), adds no shader variants to
  the pack and no layout to the game's pipelines, and works the same on Vulkan and D3D12. Its cost is the albedo
  estimate above. The ambient-term version is written down under "Later".
- The gather is a visibility bitmask, not XeGTAO's own bent normal: XeGTAO's horizons give a visibility and a
  direction but not which surfaces send light; the bitmask gives both from the same slices and depth.
- Built on GTAO: the same prepared depth, mips and normals, the same slice parametrisation and noise.
- Light only when the AO is on; light and ambient occlusion when it is off.
- The frame's ambient light from `g_vAmbientColor` by name in the shaders' own constant tables: no game data in
  the code, no list file.
- No temporal noise (the same pattern every frame, nothing crawls); spatial blur only until motion vectors exist.
- Off by default until the user has seen the comparisons.

## Later

- **Temporal accumulation** once `beta-upscalers` has working per-pixel motion vectors: reproject the last
  frame's result, reject by depth and normal, blend (fewer samples, less blur).
- **The lighting shaders' ambient term** (the `ambient` hook of `ao.md` extended to `ambient x ao + bounce`):
  the right albedo and the right place relative to the per-object fog, but the bounce must exist before the lit
  draws, so it would come from the last frame's colour, reprojected (motion vectors); the hooked shader variants
  must be built into the player's pack (`kkshaders pack-build` reading the list made from the player's own
  database); foliage and characters drawn with depth writes after the pre-pass would read the bounce of the
  surface behind them.
- Ray-traced GI.
