# Cel shading (screen effect, task 198, 17 October 2026)

A screen effect on the 3D scene: ink outlines and the lighting in a few flat colour bands, colours keeping
their hue. Off by default; the Original look keeps it off (`kk::OriginalLookSettings`). Beta 7 ships it as a
built-in Workshop mod ("Cel shading", a settings preset that switches it on; DECISIONS.md 17 Oct), with the
fine controls in F8 "Cel shading". Both APIs.

## Where it runs, and why there

At the end of the scene's opaque pass, the AO's `opaque` moment (docs/ao.md "When in the frame"): in King Kong
just before the AfterEffects fog pass, after the AO's occlusion and the GI's bounce light went in. So:

- **Exclusions come free.** Water, smoke, fire, rain, particles, Kong's fur shells, the fog, the light
  shafts and the HUD are all drawn after it and are never outlined or banded. The sky (stored depth at the
  far plane) is skipped. The fog then fades distant lines and bands as it fades the scene.
- **The depth is there.** Lines need the scene's depth and normals; after the upscaler there is no depth at
  output resolution, and water, smoke and the HUD would be inked too.
- **Render resolution, before any upscaler.** Thickness is in pixels of a 1080-line picture scaled with the
  render height, so lines keep their share of the picture; a temporal upscaler (DLSS/FSR/XeSS/DLAA) then
  smooths them like any edge (they jitter with the scene). Without an upscaler they are as sharp as the scene.
- A frame the rules do not cut gets none (no fallback at Present, where the HUD may already be in the target).

## How it works

`SceneCel` (scene_cel.*, its own copy of the AO's classifier, tracker and camera vote, as the GI) calls
`CelShade::Apply` (cel_shade.*, shaders/cel.hlsl):

1. A copy of the scene colour; `cs_exposure` (one group): the log2 mean luminance of a 64 x 36 grid of the
   picture (sky left out), followed over frames (6 % a frame; a gap starts again).
2. `ps_cel`, one full-screen pass into the scene colour target (alpha untouched):
   - **Depth lines** on the nearer object's side of a silhouette: a tap (8 directions, at the line radius
     and half of it) whose view depth is over 3 % (full at 10 %) behind what this pixel's own surface
     predicts there (1/z is linear on a plane, so floors at grazing angles stay clean). The sky behind
     counts as a full step, so silhouettes against the sky are inked.
   - **Crease lines** from the normals: this pixel's against 8 taps, and opposite taps against each other
     (the rebuilt normal right on a crease is a blend), 40 degrees starts a line, 65 is full, 85 % of the
     darkness. Normals: the AO's when it worked them out this frame with normal smoothing on (Remastered
     defaults: High), else its own `Gtao::Prepare` + `Gtao::SmoothNormals` (High; half resolution above
     render scale 1.5 like AO "auto"). Smoothed normals keep low-poly facets from becoming lines.
   - Lines fade with distance: full to 12 m, gone at 60 m (view units are metres).
   - **Bands**: the luminance (gamma encoded) is quantised into 2 to 4 bands spread over 5 log2 steps
     centred on the followed average, so a night scene is banded like a day scene; pixels darker or
     brighter than that range keep their value. A band's level is its log2 middle (its mean kept), with
     soft edges over a quarter of a band. The band is chosen from the pixel's **local** luminance (the mean
     of 17 taps on its own surface within 8 px at 1080p) and that band's change is applied to the pixel:
     the lighting goes flat, texture detail (skin, bark, stone) stays and never speckles between two
     bands. Hue is kept (rgb scaled together, held under white), then blended with the original by strength.
   - `CelBandLuma` / `CelBandColour` in cel_config.cpp are the same functions on the CPU (the test compares).

## Settings (all live; F8 "Cel shading", 5 languages)

| Setting | Default | Range | |
|---|---|---|---|
| `native_cel_shading` | false | bool | on/off |
| `native_cel_outline` | 1.5 | 0 to 4 | line thickness in px at 1080p (0: no lines) |
| `native_cel_darkness` | 0.8 | 0 to 1 | 1 black lines |
| `native_cel_bands` | 3 | 2 to 4 | flat light levels |
| `native_cel_strength` | 0.6 | 0 to 1 | 0 the original shading, 1 fully flat bands |
| `native_cel_debug` | 0 | 0 to 2 | 1 the lines alone on white, 2 depth lines cyan / creases magenta (not in F8) |
| `native_cel_gpu_time` | false | bool | log the GPU time every 600 frames (F8 shows it anyway) |

Also in the benchmark and diagnostics settings lists and the Graphics page's reset list. The panel's GPU line
shows "Cel shading x ms"; the frame timing log line has `cel`. Turned on, it is made on a worker (as the GI)
and parked when turned off (300 frames, then retired).

## Cost (RTX 4070 Ti)

`Bench_CelShading` (NR_BENCH=1, quiet window, median of 60, two runs agree):

| | AO's normals (pass only) | own normals (prepare + smoothing + pass) |
|---|---|---|
| 1920x1080 | 0.20 ms | 0.42 ms |
| 3840x2160 | 0.89 ms | 1.13 ms (half-resolution normals) |

In game (1080p, not quiet): Vulkan with the AO's normals 0.13 to 0.23 ms; D3D12 without AO 0.31 to 0.40 ms.
With a temporal upscaler the pass runs at the render resolution (DLSS Quality at 4K: 2560x1440).

## Tests

`nr_cel_tests` (backend/tests/cel_tests.cpp, the gtao_tests scene): the band function (monotonic, N flat
levels, night = day two steps darker, darks and highlights kept, hue kept, white not passed); lines along
every silhouette pixel of the box, the convex box corner and the whole floor-wall crease, none on open floor
(near and grazing) or the wall; the sky and alpha untouched; the GPU's average and bands equal to the CPU's;
lines about 4x the pixels at twice the resolution; lines fading with distance (none at 40x the scene's size).
`NR_TEST_IMAGES=<dir>` writes the line views.

## Known limits

- Kong's fur shells (and water, smoke, particles) are drawn after the moment: Kong's outline is mostly
  hidden under his fur; his fur is not banded. A second, line-only moment after the fur would need its depth.
- The 4K pass without an upscaler is about 0.9 ms (some 70 texture loads a pixel); tiles in groupshared
  memory, or the band's local luminance at half resolution, would cut it.
- Hair cards and dense foliage give many lines (as any depth-based ink); distance fade and the fog keep the
  far jungle clean.
- No temporal filtering of its own: without an upscaler lines can crawl on thin geometry in motion.
