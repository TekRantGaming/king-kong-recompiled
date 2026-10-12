# Lighting: softer shadows, reflections on water, volumetric light (beta)

Status, 12 October 2026: built on branch `beta-lighting` (from `beta` = nr-integration 1ba7339). Three options of
the native renderer, each off by default (the game's own picture) until the user has seen the comparisons; the
Original look keeps all three at the game's. Raster and screen-space techniques only (ray tracing is for later).
"In the game" below has what the runs showed.

| Option | Values (default first) | Launcher (Graphics page) |
|---|---|---|
| `--native_shadow_quality` | `original`, `high`, `ultra` | Shadows: Original / High / Ultra |
| `--native_water_reflections` | `original`, `ssr` | Water reflections: Original / Screen space |
| `--native_volumetrics` | `off`, `low`, `high` | Light shafts: Original / Volumetric low / Volumetric high |

All are read once at start (init-only, like every `--native_*` setting). Tuning without a rebuild:
`--native_shadow_softness` (the PCF and PCSS kernels' size, 1), `--native_ssr_strength` (0 to 1 of the game's
reflection the hits replace, 1), `--native_volumetric_strength` (1). Tools: `--native_lighting_census=N` (every N
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
targets (the pitch psshadowquad draws into, recognised at its first draw) at twice the render scale: the mask becomes
1280x960 at render scale 1, as fine as the frame. Everything that reads the mask samples it with normalised
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

(Filled in below as the runs come in.)

## Open problems

- The visible water of most chapters (The Cave, Ann, To the Lair, the sea of Venture and Kong Struck Down) is not
  drawn by a `pswater` shader with a reflection: its reflections stay the game's. Reaching it means finding which
  `psgeneric` shaders draw water and hooking their environment reflection.
- The volumetric light uses the first light of the screen-space shadow; which of the three is the sun is not known
  for every chapter.
- The shadows' noise relies on the game's own blur of the mask; there is no temporal accumulation (motion vectors are
  on beta-upscalers).
