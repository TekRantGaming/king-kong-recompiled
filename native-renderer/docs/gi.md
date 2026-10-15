# Screen-space global illumination (beta)

Status, 12 October 2026: branch `beta-ssgi` (from `beta` = `nr-integration` 1ba7339, with `beta-upscalers`
038dfe7 merged for its camera reprojection vote). Spatial and temporal (camera reprojection). Off by default
(`--native_gi=off`); the launcher's Graphics page has the row "Global illumination: Off / Low / Medium / High"
next to Ambient occlusion. Ray-traced GI is for later; this is screen space only.

## Task 073 self-light rejection and cost (15 October 2026)

The gather now requires both `dot(N_source, -dir) > 0` and `dot(N_receiver, dir) > 0`,
with a smooth grazing-angle weight on each. Samples closer than `max(0.15, 2 * pixelSize)`
in view space cannot send light. This rejects coplanar/convex self-light and nearby depth-normal
noise; depth alone cannot identify an object, so this is geometric rejection rather than an object-ID test.
After history, filtering, receiver albedo, multi-bounce and the retained x3 calibration, the composite
bounds positive addition to 75% of the receiver's own linear RGB in every channel. One common scale
preserves the addition's hue; the maximum encoded lift is about 29%. Higher intensity saturates at
this same bound. The experimental occlusion override's negative term is bounded separately as before.

The source pass rejects every sky host texel, takes an encoded-colour block mean with sky contributing
black, and linearises/restores once per working pixel. This deliberately approximates the old mean
of individually linearised texels to lower cost. Let `E = max(mean, 0)^2.2`,
`a = luminance(ambient) * albedoLimit`, `lo = a^2.2`, `hi = (a + max(0.15, a))^2.2`:
`source = E + 0.65 * smoothstep(lo, hi, luminance(E)) * E / max(maxChannel(E), 0.0001)`.
The linear classification and 0.65 restoration keep dim outdoor sources useful after stricter geometry
rejection. Bright textures can still be misclassified as sunlight; no actual sun/material recovery is claimed.

Medium now uses 2 slices x 6 steps per side and 2 blur passes (formerly 3 x 8 and 3).
Low and Medium use compact 3x3 a-trous kernels; High retains 5x5. All other default radii,
resolutions, temporal weight, off/additive defaults, and the x3 intensity calibration stay.
The new GPU tests reject self-light on striped planes/convex surfaces, reject sub-0.15-unit transport,
and check the receiver envelope across the image at intensity 8. Bright-fixture Medium red gains
are 0.086 at 0.6 m and 0.070 at 2 m. The former stronger almost-black-floor expectations were
incompatible with the new receiver bound and are replaced by bounded, coloured-lift expectations.
The live-intensity test uses unsaturated values; zero/off, sky, alpha and temporal bounds remain.

Vulkan 1440p benchmark before the final game runs: Medium 0.651 ms, High automatic half 1.273 ms.
A post-game repeat measured 0.795 / 1.376 ms; another measured 1.942 / 3.598 ms with variable
desktop load/clocks. These are RTX 4070 Ti results, not a stable 0.6 ms guarantee. See
`codex/REPORT-073.md` for the full timing evidence, three-run visual evidence and limitations.
No translated game shader variants or pack entries changed. Radiance, gather, denoise and composite
are embedded in both SPIR-V and DXIL and use the existing initialization/prewarm paths.

## Task 071 sunlight source estimate (historical, superseded by 073)

The source pass now uses the task's scene-colour fallback, rather than trying to capture diffuse
textures or identify a sun constant without verified shader coverage. For each opaque host texel:
`C = max(scene, 0)^2.2`, `a = luminance(frame ambient) * albedoLimit`,
`s = smoothstep(a, a + max(0.15, a), luminance(scene))`,
`source = C + 0.45 * s * C / max(maxChannel(C), 0.0001)`.
The classification uses the game's display-space ambient; the gather transports linear energy.
The added source is bounded, smoothly selected and preserves source chromaticity. It estimates
sunlit-ness, not the actual diffuse texture, sun direction or shadow visibility. A bright material
or baked highlight can be mistaken for sunlight. Ambient-only sources retain the previous energy.

Full-resolution stored depth excludes sky texels BEFORE the half-resolution colour mean and mips.
Checking only one working-depth sample per block allowed sky energy into silhouettes in a new
source test; the final implementation rejects each host texel. No new passes, game shader variants,
translator hooks or shader-pack entries are introduced. The updated radiance shader is embedded in
both DXIL and SPIR-V, using the existing compute-pipeline initialization/prewarm path.
The current temporal sample weight is 0.20 (formerly 0.15), to reduce stronger-source lag; the
existing moving-camera 0.02 display-error bound passes unchanged. The x3 composite calibration stays.

The GPU source test reads RGBA16F gathered light before receiver albedo/multi-bounce: a dim brown
wall yields about eight times the red energy when above ambient; a dark wall is bit-identical under
both ambient classifications. Existing additive, transparency, sky, alpha, off/zero, render-scale,
camera and live-toggle tests pass. Source preparation now requires the full scene depth binding.

Visual acceptance is incomplete. Three launches were used (one invalid chapter argument, one beach
close-up and one brighter dusk shore). The beach has conspicuous concentrated under-face light;
the shore gains some rock bounce at Medium but does not prove bright-sun character/wall behaviour.
The sheets precede the final per-host sky-edge correction. No further game run was permitted.
Final Vulkan synthetic 1440p times: Low half 0.757 ms, Medium half 0.989 ms, High auto/half 1.228 ms;
High forced full 4.581 ms. Medium misses the requested approximately 0.6 ms, although near the
inherited documented approximately 0.97 ms. See codex/REPORT-071.md for evidence and limitations.

## Task 066 strength calibration (15 October 2026)

Intensity 1 now applies three times the former gathered energy, in the composite constant only.
Intensity 4 is correspondingly strong (twelve times the old unit energy). This is a presentation
calibration for partial screen-space transport over already baked-lit colour, not a claim of a
complete physical bounce. Sampling counts, radius, resolution, temporal filtering and blur are unchanged.
GI remains off by default; the default occlusion remains zero and AO owns contact darkening.

The old almost-black yard overstated how visible the bounce would be outdoors. A new bright receiver
test uses a 0.298 grey floor, ambient 0.5, and the same 0.75/0.35/0.15 sunlit wall. At Medium, the
floor's red addition at 0.6 m increases from 0.060 to 0.152; at 2 m from 0.027 to 0.076. Green/blue
additions at 2 m are 0.016/0.004, so this is coloured bounce rather than a uniform exposure lift.
These are synthetic GPU pixel measurements; see REPORT-066.md for the chapter captures and limits.
The new test checks visible Medium colour, zero-intensity identity, stronger intensity 4, no
darkening, unchanged sky and preserved alpha. Live-toggle comparisons now let both histories settle
through 64 noise patterns, retaining their original error bound.

## Task 062 verification (15 October 2026)

The inherited linear-light, wider-radius rework is retained. GI stays off by default. With GI enabled,
`native_gi_occlusion` now defaults to 0: bounce only, with contact darkening left to the AO setting.
An explicit occlusion override is still available for experiments. The synthetic dark-yard test checks
that no pixel loses luminance with the defaults (including AO off). Medium raises the near floor's red
from 0.047 to 0.224 and leaves the sky and distant floor unchanged. This is synthetic evidence only;
the coordinator must check the chapter appearance. The rework notes below describe its earlier 0.5
occlusion default, superseded here.

## Beta 4 rework (14 October 2026, branch `claude-effects`)

The owner's test at 4K (DLSS Quality) found the GI invisible even at intensity 4, and in dark scenes slightly
darker than off (`runs	ask026-20261013-211423`, chapters 8 and 9). The causes, all in the old design below:

- the light was gathered and added in gamma space, where a bounce of a few percent of a sunlit wall is a few
  levels; the game renders into 8-bit targets holding display values, so the light is now linearised (power
  2.2) for the gather and the composite and encoded again at the end: the same physical bounce lifts a shaded
  floor next to a sunlit wall from 0.05 to about 0.22;
- the radius was 1.5 to 2.5 m and, worse, clamped to a fifth of the picture's height on the screen, so beyond a
  few metres from the camera the gather reached about a metre; now 3 / 5 / 8 m (Low / Medium / High) and up to
  0.45 of the picture's height, with five radiance mips (a far sample reads the mean of the surfaces around it)
  and two more steps per side;
- the occlusion of the flat ambient light (with the AO off) took away about as much as the bounce added in the
  shade; it is now half by default (`--native_gi_occlusion` 0.5) and can never take more than 35 % of a
  pixel's linear light;
- one bounce only: the composite now adds the light the surroundings send back again (a geometric series in the
  albedo and the hidden share, as multi-bounce ambient occlusion does it), up to 2.5 times;
- the albedo limit for lit surfaces is 0.75 (a picture value; 0.53 in linear light) instead of 0.6.

`GameGi_GameLikeDarkSceneBounceIsVisible` checks it on a scene with the game's numbers (ambient 0.08, a shaded
floor of 0.047, a sunlit orange wall): see "Tests".

## Where it stood (paused 12 October 2026, 16:35)

Done: the composite GI (spatial and temporal), its settings, the launcher row, the tests (`nr_game_gi_tests` 8,
all 13 standalone tests pass), the bench, one probe launch through the GI tour at Medium, scale 1 (GPU times
above; no "off" launch to compare with yet).

Next, when the game is free again (the user's reduced bar for the beta): the short tour
(`tests/gi_short.json`: Skull Island, V-Rex, The Cave) at scale 1 with off, Low, Medium, High
(`analysis\ssgi	ools\sheets_batch.ps1`, which also runs the `vrex` golden scene with GI off), then the sheet
(`analysis\ssgi	ools\gisheets.py`) and the intensity / radius tuning it shows. The first try of that batch
(16:34) ended after 6 s with the game exiting by itself (code -1, nothing in its log); it was not retried
before the pause.

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
3. `cs_temporal` (with `--native_gi_temporal`, the default): this frame's light blended into last frame's;
   see "Temporal accumulation".
4. `cs_denoise`, 2 or 3 times: a 3x3 (Low/Medium) or 5x5 (High) a-trous blur with steps 1, 2, 4,
   weighted by depth and normal (edges stay).
5. `ps_composite` (a full-screen triangle over the picture, the colour target's alpha left alone), reading a
   copy of the colour: the result upsampled by depth (as GTAO's `cs_output`) and added to the surface.

### Temporal accumulation

The upscalers' branch makes per-pixel motion vectors at its upscale point, after the post chain begins: later in
the frame than the GI's moment, and only with an upscaler on. The GI needs where each pixel was last frame
before the fog pass, with or without an upscaler, so it uses the same camera reprojection the upscalers' camera
pass uses (`upscale_motion.h`, `CameraReprojectionVote`) with its own small history:

- every opaque or depth-only scene draw before the moment is keyed by its identity across frames (vertex
  shader, vertex streams and offset, index buffer, range, primitive, as the upscalers' `MotionDrawKey`), and its
  world-view-projection rows are kept for one frame;
- a draw met again offers last frame's rows times the inverse of this frame's: for anything that did not move
  that is the camera's reprojection (the world cancels out); the matrix most draws agree on wins (at least two,
  and at least half of the offers), else the frame has no history;
- `cs_temporal`: each working pixel's NDC (from the viewport and its stored depth) goes through the matrix to
  last frame's picture; the history's four texels there count only where their view depth was this point's depth
  then (5 %); the history is clamped to the 3x3 neighbourhood's range of this frame's light, widened by half of
  it (what moved or changed does not linger); the new light weighs 0.15. A point last frame did not see, or off
  the picture, takes this frame's light alone.
- The result before the blur becomes the next frame's history, with this frame's view depth. The noise changes
  every frame (XeGTAO's temporal offset), so the history averages 64 patterns.

Moving objects (Kong, characters, the V-Rex) are reprojected as if they had not moved: the depth test and the
clamp keep their light from trailing; their own per-object motion (the upscalers' motion pass) would do better
and can be used once it runs before the fog pass.

### The composite: telling surface colour from light

The lighting shaders give a surface colour = albedo x (ambient + direct light). The bounce light has to be
multiplied by the albedo, which a forward renderer does not keep. The composite estimates it from the colour and
the frame's flat ambient light `A`:

- the frame's `A` is read from the scene's lit draws: the renderer finds `g_vAmbientColor` in each pixel
  shader's own constant table (`GameShader::ambient_constant`) and reads that constant at every draw of the
  scene (the mean of the frame; the last frame's when a frame has none);
- where a surface is in the shade its colour is about albedo x `A`, so colour / `A` is its albedo;
- where it is lit its colour is brighter than (albedo limit) x `A`, and its light is taken as its brightness over
  the limit (`--native_gi_albedo`, 0.75 as a picture value): the estimate never exceeds the limit and is exact in the shade, which is
  where indirect light matters;
- in linear light (the picture's values to the power 2.2; the game's targets hold gamma encoded values):
  out = colour + albedo x (bounce x 3 x intensity x bounces - `A` x (1 - visibility) x occlusion), where bounces =
  1 / (1 - albedo x (1 - visibility)) (at most 2.5) stands for the light the surroundings send back again, and
  the result is never darker than 65 % of the pixel's own linear light; then encoded back to gamma.

The second term is the "better local occlusion": the flat ambient light taken away where the surroundings hide
the sky, and replaced by the light those surroundings send. With the ambient occlusion setting on (either
method) that term is left out (occlusion 0): the AO already darkens there, and darkening twice would be too much.

Water, fur, smoke, fire, rain, light shafts, the sky and the HUD are drawn after the moment: untouched. The sky's
pixels (stored depth at the far plane) are left alone, and the sky does not send bounce light (its depth is out
of every radius; the flat ambient constant stands for the sky).

### Levels

| Level | Slices x steps per side | Blur passes | Resolution | Radius (world units) |
|---|---|---|---|---|
| Low | 2 x 6 | 2 | half | 3 |
| Medium | 2 x 6 | 2 | half | 5 |
| High | 4 x 10 | 3 | full up to a render scale of 1.5, half above | 8 |

The radius on the screen is at most 0.45 of the picture's height (working pixels); far samples read the
radiance's mip of about their spacing (five mips).

Half resolution is of the scene target at the render scale (at scale 2, 1280 x 720 working pixels), which is
also the path for slower GPUs (the Steam Deck class): at 1280 x 800 Low works on 640 x 400 pixels.

## Cost

After the Beta 4 rework (14 October, the same bench run back to back with the old shaders, the GPU shared with
the desktop, so slower than the table below): at 3840 x 2160 (scale 3, half resolution) Low 1.85 ms (old
1.71), Medium 2.45 ms (old 2.24), High (half) 2.97 ms (old 2.69); at 2560 x 1440 Medium 0.97 ms. About 10 % more
for the wider gather. With DLSS Quality at 4K the GI works on the 2560 x 1440 render: about 1 ms at Medium.

Before the rework:

`nr_ssgi_bench` (gtao_tests' ray-cast room at any size, a colour image, temporal accumulation on, the GPU's
timer queries over 60 frames; RTX 4070 Ti, Vulkan, the GPU at full clocks). Total of `Gtao::Prepare`,
`Ssgi::Compute` (radiance, gather, temporal, blur) and `Ssgi::Composite` (the copy and the composite):

| Level (resolution) | 1280 x 720 (scale 1) | 2560 x 1440 (scale 2) |
|---|---|---|
| Low (half) | 0.16 ms | 0.64 ms |
| Medium (half) | 0.22 ms | 0.87 ms |
| High (half: at scale 2 by auto) | 0.28 ms | 1.09 ms |
| High (full: up to scale 1.5 by auto) | 0.99 ms | 4.0 ms |

So Medium at 1440p is under 1 ms (the target was 2 ms); High is full resolution only up to a render scale of 1.5
(at 1080p output that is 1920 x 1080 working pixels, about 2.2 ms). The Steam Deck class at 1280 x 800 with Low
works on 640 x 400 pixels (0.16 ms here; several times that on the Deck's GPU).

In the game (`--native_gi_gpu_time`, the 600-frame averages; the frame rate capped at 30, so the GPU runs at low
clocks and every pass looks slower, as the AO's numbers in `ao.md` do): Medium at scale 1, the first probe
through the GI tour (`runs\ssgi-s1-medium-probe`), 0.60 to 1.37 ms, mostly 0.7 to 1.1 ms (GTAO at the opaque
moment measured 0.46 to 1.01 ms the same way).

## Settings

| Setting | Values (default first) | What it does |
|---|---|---|
| `native_gi` | `off`, `low`, `medium`, `high` | the launcher's row; off creates nothing of it |
| `--native_gi_intensity` | 1, 0 to 8 | calibrated bounce strength: 1 normal, 4 strong (three times the old unit energy) |
| `--native_gi_quality` | `auto`, `half`, `full` | the working resolution (auto: the level's) |
| `--native_gi_radius` | 0, world units | how far light is gathered from (0: the level's) |
| `--native_gi_thickness` | 0, world units | how deep a surface on the screen is taken to be (0: a fifth of the radius, 0.25 to 1) |
| `--native_gi_albedo` | 0.75, 0.05 to 1 | the brightest albedo the composite assumes (a picture value) |
| `--native_gi_occlusion` | 0.5, 0 to 1 | with the AO off: how much of the hidden ambient light is taken away |
| `--native_gi_temporal` | true, false | temporal accumulation (above) |
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
| `GameGi_RedWallBleedsOntoTheFloor` | Low, Medium, High: the floor 0.25 units from the wall gains red (0.239 to 0.39-0.41 since the Beta 4 rework, 0.33-0.35 before) and almost no green or blue; the floor 1.25 units away and the sky do not change; the ambient light came from the constant |
| `GameGi_GameLikeDarkSceneBounceIsVisible` | the game's numbers (ambient 0.08, a shaded floor of 0.047, a sunlit orange wall 4 m wide), each level's defaults: the floor 0.6 m from the wall 0.047 to 0.217 / 0.224 / 0.238 (Low / Medium / High) in the wall's colour, 2 m away 0.071 / 0.094 / 0.107, 7 m away unchanged; the lit wall unchanged, a shaded corner far away -0.002 to -0.004, no pixel darker by more than 0.008, the picture's mean brighter, the sky untouched |
| `GameGi_TransparentDrawAfterIsUntouched` | the quad is half its colour plus half the lit scene behind it (within 0.002), where painting over the finished picture would move it by 0.03 |
| `GameGi_WithAoItOnlyAddsLight` | with GTAO at the opaque moment the GI adds red and takes nothing away |
| `GameGi_VisibilityTakesTheHiddenAmbientAway` | with the AO off, by the wall green and blue get darker (the wall hides the sky) while red rises; the open floor is unchanged; the debug view shows the red bounce |
| `GameGi_TemporalStaticCameraSettles` | eight frames from a still camera: the history is used from the second frame on, and the light settles where a frame without it is (mean difference below 0.0001) |
| `GameGi_TemporalMovingCameraFollows` | the camera moves every frame: the history is reprojected (used 7 times) and the red bleed sits where a fresh frame from the last camera puts it (0.329 against 0.334; mean difference 0.0003) |
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
- Temporal accumulation from the camera's reprojection (voted as the upscalers vote it), not from the
  upscalers' motion vectors: those are made later in the frame and only with an upscaler on. Without a history
  the noise is the same every frame (nothing crawls).
- Off by default until the user has seen the comparisons.

## Later

- **Per-object motion** for the history (Kong, characters), once the upscalers' motion pass can run before
  the fog pass; then fewer samples or blur passes per frame.
- **The lighting shaders' ambient term** (the `ambient` hook of `ao.md` extended to `ambient x ao + bounce`):
  the right albedo and the right place relative to the per-object fog, but the bounce must exist before the lit
  draws, so it would come from the last frame's colour, reprojected (motion vectors); the hooked shader variants
  must be built into the player's pack (`kkshaders pack-build` reading the list made from the player's own
  database); foliage and characters drawn with depth writes after the pre-pass would read the bounce of the
  surface behind them.
- Ray-traced GI.
