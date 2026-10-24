# Lighting: dynamic shadows, water reflections and light shafts (experimental)

## Graphics presets (Beta 14, task 350b)

Since Beta 14 the graphics presets set all three (owner, 21 Oct; kk/src/graphics_presets.h): Low, Medium and
Steam Deck Original / Original / Off; High High shadows, Screen space water reflections, Volumetric low light
shafts; Ultra Ultra shadows, Screen space reflections, Volumetric high shafts. The defaults stay Original/Off, and
changing one of them by hand shows the preset as Custom.

## Task 101 candidates (15 October 2026)

All three options remain hidden and default to Original/Off after the six permitted runs. The visual
acceptance targets were not established: worthwhile dynamic-shadow edges, generic water coverage,
and a clearly useful shaft improvement still need verification. GI's preset defaults are unchanged.
`codex/REPORT-101.md` records the per-effect HIDE verdicts, pictures, costs and remaining gaps.
This section supersedes the implementation details and timer scope in the historical sections below.

- Shadows retain 2x maps for High; Ultra uses 1.5x maps (Beta 14, task 348a: 2x before; its PCSS penumbra is
  measured in guest texels, so the coarser texels are blurred away; 0.08 ms less) and the finer screen-space mask. High uses four
  bilinear PCF gathers over 0.6 guest texels. Ultra uses four blocker-search gathers over three guest
  texels and eight filter gathers, with a 0.35-2 guest-texel penumbra. Softness still scales the kernels.
  Receiver-plane correction now includes each of the four texels in a gather, in both blocker search
  and filtering. Comparing every texel against the tap centre's depth darkened a sloped plane in the
  new synthetic fixture; per-texel correction keeps it lit. The game's constant bias is retained.
  Replacement identities changed for both qualities. No world casters or baked shadows are added.
- Water has 20 verified `pswater` shader identities: 13 existing reflection-fetch hooks and seven
  non-reflective base-colour output hooks. The additional six have refraction/depth fetches and a
  constant colour, distinguishing them from the mask and additive lighting-only variants. Normal
  map fetch 3 is used where present. This does not identify arbitrary `psgeneric` surfaces as water.
  The pack compatibility stamp now includes the hook list and each hook's mode, as well as its HLSL;
  changing only the list therefore invalidates an incomplete older pack. Both scaled and unscaled
  lighting variants remain in the DXIL/SPIR-V packs and use the existing prewarm path.
- Shafts now integrate sunlight through fog along each view ray at quarter resolution, using 16/32
  samples for Low/High. Each sample is projected into the current frame's first static shadow map
  with the game's matrix and bias. A covered, lit sample contributes; a blocked sample or one outside
  map/depth coverage contributes nothing. A valid camera, map and orthographic light direction are
  required. The last matrix row must match the game's inverse-depth representation; a perspective
  light or degenerate light-depth gradient is rejected. This recovers a light direction, not proof
  that every chapter's first light represents the sun or that its map contains every roof/caster.
  The bright-colour mask and sky-centroid search are no longer used. Sky, zero fog, missing maps,
  unknown coverage, zero strength and Off add nothing. A compact depth-aware filter and four-tap
  depth-aware upsample keep distant fog off foreground silhouettes. The composite only adds light,
  keeps alpha, and caps each channel's addition at 35% of `max(receiver, 0.03)` before headroom.

`native_lighting_gpu_time` now includes shadow-map draws and mask composite/blur, rather than only
`psshadowquad`. Water timing includes preparation when it runs and the game's water draws. These
are total pass costs; On minus Original is the incremental cost. No ROG Ally timing is inferred
from desktop measurements. Explicit GPU profiling keeps the timing tracker alive with all effects
Original/Off, so it can measure that baseline; ordinary Off still parks the temporary resources.
The synthetic shader tests and the real-game checks are separate evidence. In the channel census,
generic normal-mapped shader `E4A4C51DEA2423BB` uses a skinned vertex shader, so it is not assumed to be
the water. No generic material is hooked from texture dimensions or draw order alone.

## Task 062 disposition (15 October 2026)

All three options are hidden from the launcher Graphics page and F8 panel for Beta 4. Their code and
explicit cvars remain for the coordinator's run; their defaults remain Original/Off. The historical
design and measurements below are superseded as follows:

- Ultra uses 2x maps, like High, and 8 blocker-search plus 12 filtering taps (previously 4x and 16+24).
  Its replacement tag changes so cached pipeline identities do not reuse the old variant. The improvement
  still affects only the game's dynamic shadow passes. Its value in the sparse-shadow chapters is unproven.
- The census's non-reflective `pswater` variant `805E38F150347CFA` now receives an output reflection hook
  with its normal map, and output-only hooks trigger SSR preparation. Its alpha/depth mask stays untouched.
  No generic shader is assumed to be water: the existing census does not establish a safe generic-water
  identity. Broad chapter coverage is therefore unresolved and the option stays hidden.
- The inherited quarter-resolution bright-sky mask, centroid and radial gather now compile (the reserved
  HLSL identifier `centroid` is fixed), with all required DXIL/SPIR-V blobs checked in. The composite adds
  light only, scales with fog at both source and receiver, and uses remaining colour headroom. It no longer
  subtracts shadowed fog or performs a full-resolution 16/32-step shadow march. No bright source or no fog
  produces no change. Because the game's sky can draw after the fog-pass insertion point, synthetic shafts
  do not prove useful in-game source coverage. Placement must be verified before exposing the option.

`nr_lighting_tests` uses synthetic images and the production reflection HLSL and shaft passes. A water
plane reflects a bright orange wall (+0.173 peak red, 1056 visibly changed pixels at 192x128; 960 with the smooth
trace's wider edge fade). `Lighting_FacetedWaterReflectionStaysWhole` gives the water low-poly waves (a triangulated
height grid) and a striped wall: the faceted trace adds 0.083 of hard steps over the reflection, the smooth trace
0.036 (ripple 0: none), without the plane it keeps the triangles' normals; `Lighting_WaterPlanesFromTheDrawsMatrix`
checks the planes from a world-view-projection (even and uneven model scale, a projective texture matrix before or
after it). Dense fog
receives +0.0899 mean red, thin fog +0.00558; clear fog and no bright source are unchanged. No shaft pixel
darkens, and alpha is unchanged. GPU checks use NVRHI validation; the Khronos validation layer is absent
on this host. See codex/REPORT-062.md for costs, limits and the coordinator's chapter checks.

Status, 12 October 2026: built on branch `beta-lighting` (from `beta` = nr-integration 1ba7339). Three options of
the native renderer, each off by default (the game's own picture) until the user has seen the comparisons; the
Original look keeps all three at the game's. Raster and screen-space techniques only (ray tracing is for later).
"In the game" below has what the runs showed.

| Option | Values (default first) | Launcher (Graphics page) |
|---|---|---|
| `--native_shadow_quality` | `original`, `high`, `ultra` | Shadows: Original / High / Ultra |
| `--native_water_reflections` | `original`, `ssr` | Water reflections: Original / Screen space |
| `--native_volumetrics` | `off`, `low`, `high` | Light shafts: Original / Volumetric low / Volumetric high |

All apply while the game runs (the graphics panel, F8, and the in-game settings pages): the passes' pipelines
are made on a worker when an option is first turned on, the shadows' and the water's shader variants are made
on a worker too (the water's come from the pack when it has them), and until they are ready a draw uses the
game's own shader. Turned off, the passes are kept for 300 frames (an A/B switch back is instant). Tuning
without a rebuild, live too:
`--native_shadow_softness` (the PCF and PCSS kernels' size, 1), `--native_ssr_strength` (0 to 1 of the game's
reflection the hits replace, 1), `--native_ssr_ripple` (0 to 1, how much of the wave triangles' own tilt the
smooth reflections keep, 0.25; F8 "Reflection ripple"), `--native_ssr_smooth` (true; false traces from each wave
triangle as the first version did, a comparison switch), `--native_ssr_soft_edge` (true; false keeps the hard
edge where rays miss, a comparison switch), `--native_ssr_debug` (false; a developer view of what each reflected
pixel comes from), `--native_volumetric_strength` (1). Tools: `--native_lighting_census=N` (every N
frames, every draw of the frame with its family, targets and their host sizes, state, the textures its pixel shader
reads with their guest and host sizes, and the constants of the lighting passes; the frame's resolves too) and
`--native_lighting_gpu_time` (GPU time of the screen-space shadow, of the water with its reflections and of the
volumetric light, averaged over 600 frames in the log).

## What is where

| Path | What |
|---|---|
| `backend/lighting_config.*` | the settings and their names |
| `backend/lighting_shaders.*` | the game's pixel shaders the options act on, by microcode hash and family; psshadowquad's constant layout; pslightshaft's modes |
| `backend/game_lighting.cpp` | the game renderer's hooks (draw, Present, b5, the census, the shadow mask's scale, the replacements) |
| `backend/scene_lighting.*` | when in the frame each option works and its own passes (follows the frame with the AO's classifier, tracker and camera) |
| `backend/shaders/lighting_shadow.hlsl` | the soft shadows: the replacement for psshadowquad (6 blobs: 1, 2, 3 lights x PCF, PCSS) |
| `backend/shaders/lighting_ssr.hlsl` | the water reflections' preparation: view depth pyramid, colour copy with mips |
| `backend/shaders/lighting_volumetric.hlsl` | the volumetric light pass |
| `shaders/hlsl/kk_lighting.hlsli`, `shaders/include/kkshaders/lighting.h`, `shaders/src/lighting.cpp` | the KKLighting buffer (b5), the water reflection hook's functions, the list of water shaders |
| `thirdparty/XenosRecomp/.../shader_recompiler.*` | the translator's water reflection hook |
| `shaders/tests/lighting_tests.cpp` | the hook on the generated corpus, the list, the pack stamp |
| `tests/lightgen.py`, `tests/lighting_tour.json` | the chapters the options are judged on |
| `kk/src/launcher.cpp` (`LightingItems`), `kk/src/settings.cpp` (Original look) | the launcher's rows |

Hook points in existing files are one or two lines, marked `Lighting hook (game_lighting.cpp)`.

## Finding the passes: the census

One launch through 14 chapters with `--native_lighting_census=450` (`runs\lighting-census1`, summaries in
`F:\KK-native-renderer\analysis\lighting\census*.txt`, made by `analysis\lighting\tools\census.py`). Per gameplay
frame, in this order:

| Part | Draws | Target |
|---|---|---|
| planar water reflection (V-Rex, Ann, The Cave, To the Lair) | `psreflection`, 6 to 64 | the scene's target, resolved to 320x180 |
| shadow maps | `psshadow` (19 to 300 draws), six resolves of 832x832 `k_32_FLOAT` | 880-pitch target |
| depth pre-pass, opaque with depth writes | | the scene |
| light shaft volumes (Ann, The Cave, Call Kong, Kong to the Rescue, Chased by Kong) | `pslightshaft`: resample of the depth to 320x180, the cookie volumes (additive, depth-tested against it), a blur | the scene's target as scratch, each resolved to 320x180 |
| screen-space shadow | `psshadowquad` 9A0FBB044BB1BD85 (3 lights) twice (two sets of shadow maps: static and moving casters), `pscompositeshadow` (min of the two), `psblurshadow` twice | **a 640-pitch target, 640x480 used** (half the frame's width, two thirds of its height) |
| lit opaque pass | `psgeneric` reads the mask at its pixel | the scene |
| fog | `1B65AC7429B67FE9` / `9F699E763A41FE07` | the scene |
| water | V-Rex: `pswater` AC2239A7 (the mask: alpha only, depth writes) then C28D09A8 (planar reflection 320x180 + environment cube, refraction from a copy of the scene); Ann, The Cave, To the Lair: only the mask shader (their visible water is drawn by other families); Venture: `psocean` with colour writes off | the scene |
| light shafts | `pslightshaft` 16C954AF, additive, the blurred 320x180 shafts over the whole frame | the scene |
| post effects | `psaftereffects` | |

Facts the design rests on:

- **The shadows' blockiness has two causes**: 832x832 maps compared once per pixel, and the screen-space mask the
  comparison writes, at 640x480 for a 1280x720 frame, then blurred and read by `psgeneric` with bilinear filtering.
- **psshadowquad's math** (pixel constants c0..c3 per light: `g_mCamToLightMatrix`, `g_vShadowParams[i].z` the
  bias, `g_fShadowColor` per channel): the view position is `(x d, y d, d, 1)` with `x, y` the quad's clip position
  and `d` the stored depth from the depth resolve; the matrices' w row is `(0, 0, -20, 20)`, so the light's w is the
  inverse view depth, which is also how the fog pass reads the same depth (`g_vFogNormalizing` = (-20, -20)).
- **Water with a reflection of its own is rare**: of the 26 `pswater` shaders, 13 read a planar reflection (fetch 1)
  and 6 of those an environment cube too (fetch 2); at the tour's moments only V-Rex draws one (C28D09A8). The game's
  planar reflection is drawn at 320x180.
- **The game's shafts are not shadowed** (the render variants drawn have no shadow-map fetch): cookie volumes with
  noise, depth-tested against a 320x180 depth, blurred.

## Shadows

**Resolution.** `high` sets the shadow maps' scale to 2 (1664x1664), `ultra` to 4 (3328x3328), through the existing
shadow scale (`--native_shadow_scale`, which still wins when set; a shadow scale of exactly 1 gives way to the
quality's, because the soft shadows need the render scale aware shaders). Both also draw the screen-space shadow's
targets (the pitch psshadowquad draws into, recognised at its first draw) at 1.5 times the render scale (with an
upscaler, its input scale): the mask becomes 960x720 at render scale 1, as tall as the frame (Beta 14, task 348a;
twice the render scale before, 1280x960: after the game's blur of the mask no visible difference, and 0.4 ms less
GPU time at 4K DLSS Quality). Everything that reads the mask samples it with normalised
coordinates or VPOS (aware), so a finer mask is only finer.

**Filtering.** The game's three psshadowquad shaders are drawn with replacements of the renderer's own
(`lighting_shadow.hlsl`, compiled with the translator's prelude and DXC flags into the plugin: nothing compiles at
run time, nothing to add to the pack). The shader library knows them by microcode hash, keeps the translation's
bindings (from the pack) and gives them an input hash of their own (`ShaderLibrary::SetReplacements`). The
replacement does the game's arithmetic, then instead of one shadow-map texel per light:

- **high, PCF**: 16 taps on a Vogel disc of 1.5 guest texels (832 per side) radius, turned per pixel by interleaved
  gradient noise; each tap a bilinear comparison of the four texels around it (`GatherRed` with the game's sampler,
  so its address mode stays). The noise is averaged by the game's own blur of the mask.
- **ultra, PCSS**: a 16-tap blocker search over 5 guest texels; no blocker, fully lit; else the penumbra is the
  distance from the blockers' mean depth to the receiver times a light size (300 per unit of light-space depth),
  between 0.6 and 6 guest texels, filtered with 24 taps. Shadows harden where an object touches the ground and soften
  away from it.
- **Acne and peter-panning**: the game's constant bias is kept (nothing floats more than in the game); the wider
  kernel's taps follow the receiver's plane (the light-space depth's slope across the shadow map from the pixel's
  screen derivatives, Isidoro 2006), limited to 0.01 of depth per guest texel so a 2x2 quad across two surfaces does
  not throw it off.

Radii are in guest texels, so the footprint is the same at every resolution and only the sampling gets finer.

## Water reflections

**Where.** The water shaders that reflect (`pswater` with fetch 1 and / or 2, 13 shaders) are translated with a
reflection hook (`RecompilerInput::waterReflection`): `kk_lighting.hlsli` is put into the shader, `kl_WaterTrace`
runs at the top of main (uniform control flow: it takes the surface's normal from screen derivatives), and the
reflection fetch's texel goes through `kl_WaterReflection`. Everything the game does with it afterwards stays: its
fresnel (`g_fReflectionIntensity` times the water's depth factor), the alpha that says where the planar reflection is
empty, refraction, lighting and fog. Where the ray finds nothing the game's own reflection (the planar one or the
environment cube) stays.

**The scene at the fog-pass moment.** Before the frame's first water draw (the mask, which writes the water's depth,
comes first), `SceneLighting::PrepareSsr` turns the scene's depth target into view depth (the camera from the AO's
camera finder and viewport mapping) with a pyramid of the nearest depth per 2x2, and copies the scene's colour into an
RGBA16F image with 5 mips. The water shaders read both through the KKLighting buffer (b5) and the bindless tables.
Done only when the frame before drew a hooked shader, so frames with water masks only pay nothing.

**The march.** The ray is reflected about the surface's normal (from its own geometry, tilted by the normal map's
slope where the shader has one), from the water's view position up to three times its view depth, clipped to the
picture. It is traced in screen space with perspective-correct depth over the min-depth pyramid: a texel the ray
passes in front of is skipped and the next coarser mip taken; a texel it reaches is refined down to mip 0; at mip 0 a
hit is within 3 % of the surface's depth (thicker objects are passed behind). 64 iterations at most. The hit fades
over the outer 8 % of the picture, over the last quarter of the ray, and for rays turning back towards the eye; the
game's ripple offset (the planar reflection's coordinate minus the pixel's) moves the hit too; the normal map's slope
picks a blurrier mip (rough water). `--native_ssr_strength` scales how much of the game's texel the hit replaces.

### Smooth water reflections (task 196)

The game's wave meshes are low-poly and move. Traced from each triangle's own normal (above), neighbouring triangles
send their rays to different places, so the reflection broke into jagged shards along the triangle edges (tester
screenshots, swamp chapters, 17 Oct). With `--native_ssr_smooth` (on by default whenever the reflections are on):

- **The flat plane.** Per water draw, `WaterPlanesFromConstants` (scene_lighting.cpp) takes the draw's
  world-view-projection (the first vertex constants that look like a perspective matrix with the frame camera's depth
  rows; one whose off-centre terms match too is preferred, so the planar reflection's projective texture matrix is
  passed over) and takes the camera's projection out: model-to-view. The normals of the model's three planes (yz,
  zx, xy) go to `kl_SsrAxis0-2` (b5, per draw; the buffer is rewritten only when a water draw brings new ones). In
  the shader `kl_WaterPlane` picks, per pixel, the plane whose normal is nearest the triangle's (waves tilt a triangle
  far less than 45 degrees; none within 45 degrees, or no matrix: the triangle's own normal, as before). In the game
  (Swamps) the water's model is z-up, the xy plane is the one picked.
- **Ripple.** The ray's normal is the plane's plus `--native_ssr_ripple` (0.25) of the triangle's tilt within it;
  the tilt left out becomes blur (8 colour mips per unit of it). The normal map (a smooth texture, animated by game
  time) still tilts it as before, so the ripples move at the same speed at any frame rate (nothing is kept between
  frames).
- **Soft, not hard.** The picture-edge fade is wider (12 %) and smooth, the far-ray fade is smooth, the colour gets one
  more mip along the whole ray, and a hit that only grazes the back of a thin object (grass, a splash: the ray was
  already behind its front when it reached it) keeps less (down to a quarter at the 3 % thickness limit). Where no ray
  hits, the game's own reflection stays, as before.
- **No temporal pass.** The trace runs inside the game's own water pixel shaders, which cannot keep a history image;
  a reprojected accumulation would need the water's reflection drawn separately and resolved in a pass of its own
  (left out; the stable plane removes most of the frame-to-frame change already).

Measured (17 Oct, RTX 4070 Ti, 1920x1080, frozen game clock; details in the task's kk\out\task196\details.md):
the stream in To Save Ann (entry 22, 30 s) is the clearest spot. Hard steps (> 12/255 between neighbours) over the
reflected water, the game's own water / faceted trace / smooth trace: Vulkan 0.0069 / 0.0163 / 0.0108, D3D12
0.0009 / 0.0094 / 0.0056, so the steps the reflections add drop by 59 % and 45 %; the rock's reflection is one
piece instead of shards. Cost of a full-screen water trace (synthetic, NR_WATER_BENCH_WIDTH): 1080p 0.100 ->
0.102 ms, 4K 0.453 -> 0.459 ms. The water's animation keeps running under the developer freeze, so frames are
compared by their own hard steps, not pixel by pixel. Where a ray misses (the reflected silhouette of a rock
against the far gorge) the edge to the game's own reflection was still hard: see the next section.

### Soft miss edge (task 201)

Where the rays stop finding the rock (they pass beside its outline into the far gorge, beyond their reach, or go
behind it), the screen-space reflection gave way to the game's own reflection in one pixel: a hard cut outline.
With `--native_ssr_soft_edge` (on whenever the smooth reflections are) the hand-over fades over a few pixels, all
inside the same trace (the water shaders keep no history, so nothing depends on the frame rate):

- **Coverage.** Around the point a ray reached, 17 taps of the depth (the point, eight at a radius, eight at half
  of it; radius `max(2.5, picture height / 180)` pixels: 6 at 1080p, 12 at 4K) count how much of the surroundings
  a ray could have hit there: depths within the range the ray sweeps over that radius (plus the 3 % thickness), and
  above the water's plane (the water itself never counts). Deep inside an object that is 1, at a straight
  silhouette about a half, a radius outside it 0. The confidence is multiplied by it and the colour is taken where
  those texels are (one more blur mip towards the edge), so hit and miss meet at about a half from both sides.
- **Near passes.** A ray that hits nothing still remembers the finest pyramid cell where it came within a few
  texels of a surface at its own depth (within four thicknesses; not while it is still so close to the water that
  the cell's nearest texel can be the water) and the first object it went behind. Beside an outline the coverage
  there gives the outer half of the fade; behind an object the ray keeps a quarter of it (as a hit at the
  thickness limit does), fading out over two radii of how far behind its depth it already was.
- **Holes.** Fixed on the way: a near-vertical ray whose step was a hair below zero took its cell boundary for one
  behind it, crept by tiny steps and ran out of steps (one-pixel holes in the reflection at power-of-two columns).
- **Developer view** (`--native_ssr_debug=true`, live): hits blue (at a silhouette) to red (inside), near passes
  green, behind objects cyan, misses with nothing near yellow (out of steps) or magenta (to the ray's end).

In To Save Ann the misses beside the rocks are mostly rays that reach their end (three view depths) in front of the
far gorge; the outline of each rock's reflection now fades instead of cutting. Thin objects (branches) are
reflected a little fainter and softer (their coverage stays under 1). Measured (17 Oct, frozen, entry 22 at 30 s,
1920x1080; band = 3 px around where the soft edge changes the picture): neighbour steps over 12/255, game / hard /
soft: Vulkan 0.014 / 0.052 / 0.030, D3D12 0.013 / 0.046 / 0.024 (the steps the reflections add: -58 % and -66 %);
over 24/255: Vulkan 0.0018 / 0.0074 / 0.0037, D3D12 0.0032 / 0.0060 / 0.0034. Flicker over the band (frozen
camera, frames 0.25 s apart, hard / soft): 60 FPS 1.35 / 1.36, 120 FPS 1.56 / 1.34, D3D12 60 FPS 1.36 / 1.26 /255.
Cost (synthetic full-screen water with the water's depth in the pyramid): 1080p 0.127 -> 0.148 ms, 4K 0.65 ->
0.71 ms. `Lighting_WaterMissEdgeFades` checks it on a synthetic wall: the largest step at its reflected sides drops
from the whole reflection to 0.27 of it, over about 6 pixels per side, with the middle and the open water unchanged.

**Pack.** The hooked shaders are in the player's pack when it is built for the setting (`PackBuildOptions::lighting`,
the stamp's `lighting <hash>`: a pack without them, or with another version of the hook, is rebuilt). Nothing compiles
on first use.

## Light shafts (volumetric light)

The game's fog pass adds its fog colour by an amount that grows with the distance, as if all the air were lit. Right
after it, `lighting_volumetric.hlsl` marches each pixel's view ray through the part of it the fog covers (from where
the game's fog starts to where it is whole, or the surface), 16 (low) or 32 (high) steps with per-pixel jitter, and
tests each step against the first light's shadow map of the frame's first psshadowquad, in psshadowquad's own space
and with its bias. The fog that lies in shadow is taken away again, down to an ambient share (55 % low, 35 % high),
with a blend of destination minus source. So:

- shafts of lit fog show between shadowed fog, in the fog's own colour;
- only as much as the game has fog: a scene without fog is untouched (the pass is skipped when the fog's density or
  distance is zero, and its effect is proportional to the game's fog amount at each pixel);
- the game's own shaft sprites still draw afterwards (augmented, not replaced); in frames that have them the effect
  is a quarter stronger;
- frames without a screen-space shadow before the fog (no shadow map) are left alone.

## Launcher

`LightingItems` in `kk/src/launcher.cpp`: three rows after Ambient occlusion on the Graphics page, only while the
native renderer draws the game, set by the Original look (whose settings now include the three at the game's
values, and Reset page resets them). The settings are the native renderer's own cvars (`native_*`, category "Native
renderer"), which the stable version does not have (the beta-launcher branch resets them before a switch to stable).

## Tests

- `kkshaders_lighting_tests`: the water list; without the hook every corpus translation and hash is unchanged and
  vertex shaders never change; for every corpus pixel shader with a 2D or cube fetch the hooked translation is the
  plain one plus exactly the hook (2D, cube and output hooks, with and without the normal map), with its own input
  hash, built to signed DXIL and SPIR-V in turn; the pack stamp. 822 pixel shaders, 818 built, 0 failed. The other
  translator tests (20) still pass, `ambient` included.
- The real database: `kkshaders pack-build --scaled --lighting` builds every hooked water shader (6853 shaders, 0
  failed).

## In the game

Paused on 12 October 2026, 16:35, at the coordinator's request (the PC is needed). State:

- Built and committed: all three options, the translator hook with its pack variants, the launcher rows, the
  tests (kkshaders_lighting_tests 0 failed, the 20 translator tests, 
r_ao_rules_tests 14 with the new
  Lighting_SettingsAndShaders). kkshaders pack-build --scaled --lighting builds all 6853 shaders.
- One launch so far (
uns\lighting-s1-high, lsmoke: Skull Island, V-Rex, Ann). **The options did not apply**:
  the harness runs with --kk_original_look=true, and the Original look now sets the three options to the game's
  values (as intended for players). Test runs must add
  --kk_original_look_keep=native_shadow_quality,native_water_reflections,native_volumetrics. So that run measured
  the game's own passes with --native_lighting_gpu_time: the screen-space shadow 0.045 to 0.10 ms a frame, the
  water 0.033 to 0.041 ms (RTX 4070 Ti, scale 1).
- In that run the game exited by itself (code -1) in Ann, after Skull Island and V-Rex were captured. Not yet
  known whether it is the lighting code (only the GPU timers and the per-frame tracking ran) or something else;
  the next run should tell (look at the end of its game.log; run once without --native_lighting_gpu_time).

Next (the reduced verification bar: one sheet per feature, render scale 1, GPU cost, defaults unchanged in one
scene):

1. nalysis\lighting\lightrun.ps1 -Scenes lsmoke three times, each with the --kk_original_look_keep above:
   all original with --native_lighting_gpu_time=true; high + ssr + volumetrics high; ultra + volumetrics
   low. Sheets with nalysis\lighting\tools\sheets.py --tour native-renderer\tests\lighting_tour.json --run
   lsmoke (shadows: Skull Island, V-Rex; water: V-Rex's C28D09A8 if it is in view, else a chapter where it is;
   volumetric light: Ann).
2. Defaults change nothing: lightrun.ps1 -GoldenScenes -Scenes vrex -Compare with no lighting arguments.
3. Tune from the sheets (PCSS light size, slope limit, SSR thickness and ray length, the volumetric ambient
   share), fill in this section, commit each feature when it looks right.

## Open problems

- The visible water of most chapters (The Cave, Ann, To the Lair, the sea of Venture and Kong Struck Down) is not
  drawn by a `pswater` shader with a reflection: its reflections stay the game's. Reaching it means finding which
  `psgeneric` shaders draw water and hooking their environment reflection.
- The volumetric light uses the first light of the screen-space shadow; which of the three is the sun is not known
  for every chapter.
- The shadows' noise relies on the game's own blur of the mask; there is no temporal accumulation (motion vectors are
  on beta-upscalers).
