# Ambient occlusion: GTAO at the right moment of the frame (Phase 3)

Status, 11 October 2026 (cloud session, branch `cloud-p3-ao`, from `nr-integration`). Built and tested on Linux
with lavapipe and the Khronos validation layer; not yet run in the game. The defaults leave today's AO exactly as
it is: everything below is switched on by `--native_ao_method=gtao`.

## Why

Today's AO (`backend/ambient_occlusion.*`, ported from the Xenos plugin's patches 0004 to 0007, `backend.md`,
"Ambient occlusion (session F)") waits for the full-screen depth resolve, then darkens the whole picture at the
next full-screen colour copy. By then the water, smoke, particles, fog and glass drawn after the solid geometry are
in that picture, and they are darkened by the occlusion of what is behind them: players see AO through water and
smoke. It also works at half resolution with rough normals.

The native renderer sees every draw and its state, so it can do better: work the occlusion out from the depth at
the end of the opaque pass and multiply it into the scene right there, before the first transparent draw. Those
then draw over it unaffected. Better still, the occlusion can go into the lighting shaders' ambient term only,
which is what ambient occlusion means physically (the "ambient-only" mode below; the mechanism is here, which term
is the ambient one is for the Windows side to find).

The occlusion itself is XeGTAO (Intel, MIT, vendored unchanged in `thirdparty/XeGTAO`, see `thirdparty/NOTES.md`):
ground-truth based AO from the depth, with a depth prefilter, normals rebuilt from depth (the game is a forward
renderer, it has no normal buffer), full or half resolution and an edge-aware spatial denoise.

## What is where

| Path | What |
|---|---|
| `thirdparty/XeGTAO/` | XeGTAO 1.30: `XeGTAO.h`, `XeGTAO.hlsli`, the sample's `vaGTAO.hlsl` (reference), `LICENSE` |
| `backend/shaders/gtao.hlsl`, `generated/gtao_*` | the compute passes around XeGTAO and the two apply passes; SPIR-V and DXIL like every renderer shader |
| `backend/gtao.*` | `Gtao`: XeGTAO with NVRHI (images, layouts, dispatches), the apply passes, the depth copy |
| `backend/ao_classifier.*` | the draw classifier (a rule table) and the per-frame tracker of the scene's passes |
| `backend/ao_camera.*` | the camera's projection from the world-view-projection rows, the vote, the mapping to XeGTAO's constants |
| `backend/ao_config.*` | the settings (`AoConfig`) and their names |
| `backend/scene_ao.*` | `SceneAo`: when in the frame the occlusion is worked out and where it goes, with the fallbacks |
| `backend/game_ao.cpp` | the game renderer's hook bodies (draw, clear, resolve, Present, the ambient-only buffer) |
| `shaders/include/kkshaders/ambient.h`, `src/ambient.cpp` | the ambient-only term and the lighting shader list |
| `thirdparty/XenosRecomp/.../shader_recompiler.*` | the translator hook that multiplies the term (recorded in `NOTES.md`) |
| `shaders/tool/main.cpp` | `kkshaders translate --addresses / --ambient`, `kkshaders ao-list` |
| `backend/tests/ao_rules_tests.cpp` | classifier, tracker, camera, view mapping (no GPU) |
| `backend/tests/gtao_tests.cpp` | `Gtao` alone on a ray-cast depth image (lavapipe) |
| `backend/tests/game_ao_tests.cpp` | the whole path through the game renderer: box, floor, walls, water, particle (lavapipe) |
| `shaders/tests/ambient_tests.cpp` | the translator hook on the generated corpus |

Hook points in the existing renderer files are one or two lines each and marked `AO hook (game_ao.cpp)`:
`game_renderer.cpp` (Initialize: the b4 binding, the GTAO object; Draw: the classifier), `game_passes.cpp` (Clear,
Resolve, Present), `game_renderer.h` (the declarations, `Options::ao`), `shader_library.*` (listed shaders get their
term), `backend.*` and the plugin (the settings).

## The design

### When in the frame

`SceneAo` follows each frame through the draws the game renderer records:

- **apply=opaque.** At the end of the scene's opaque pass, just before its first transparent or effect draw, XeGTAO
  runs on the scene's depth and the scene's colour target is multiplied by the occlusion in place (a full-screen
  triangle with a multiplicative blend, `dst * src`, alpha untouched, nothing read back). The water, glass, smoke,
  particles and the fog pass then draw over it unaffected. Fog is an effect draw (no depth test), so distant fogged
  surfaces are not darkened through the fog either, without the legacy AO's fog arithmetic.
- **apply=ambient.** At the end of the scene's depth pre-pass (the first opaque draw after the depth-only draws)
  the occlusion is worked out once; the listed lighting pixel shaders read it at their pixel and multiply their
  ambient term. If the frame has no pre-pass, no listed shader drew in the scene, or ao_mode is 2, the frame falls
  back to the opaque moment.
- **apply=resolve.** Today's moment, with XeGTAO instead of the legacy occlusion: the full-screen depth resolve is
  copied (the game may clear the depth right after), the next full-screen colour resolve of the frame (the scene
  copy) and later ones to the same destination read a copy of the colour with the occlusion multiplied in under the
  game's fog (the same fog arithmetic as the legacy AO, from the AfterEffects fog shaders' constants). The game's own
  target is not changed.
- **Fallbacks.** When the rules do not cut a frame (no transparent or effect draw after the opaque pass), the scene
  copy gets it as in resolve mode; when there is no scene copy either and the scene target still holds the scene at
  Present, it goes in there. A frame never gets it twice.

The depth used: the scene's depth target while it holds this frame's depth (written by the frame's draws, not
cleared since: the tracker watches clears and resolve clears), else the depth resolve's copy of this frame, else
nothing (the frame has no AO, counted in the statistics).

### The draw classifier

Each draw's state, as the game renderer already has it for its pipeline (`AoDrawState`): writes colour (a target
with a write mask that the pixel shader outputs to), depth test, depth write, the blend of the first colour target
written (`RB_BLENDCONTROL`), alpha test, primitive, vertex and pixel shader microcode hashes. A small rule table
gives it a class: `depth` (the pre-pass), `opaque`, `transparent`, `effect`, `ignore`. First match wins.

The built-in table (`kDefaultAoRules` in `ao_classifier.cpp`):

```
depth        color=no depth_write=yes
ignore       color=no
effect       depth_test=no
opaque       blend=off
transparent
```

That is: the depth pre-pass writes depth and no colour; draws that write neither (occlusion surveys, stencil marks)
do not count; without a depth test a draw is a screen pass (fog, post effects) once the scene has begun, or the
sky or a backdrop before it (ignored); unblended draws are the opaque scene, alpha-tested foliage included; the rest
is blended over it.

Rules from a file (`--native_ao_rules`) come before these and win. One rule per line: a class, then any of these
conditions, all of which must hold:

| Key | Values |
|---|---|
| `color`, `depth_test`, `depth_write`, `alpha_test` | `yes`, `no` |
| `blend` | `off` (ONE, ZERO, ADD), `on` (anything but off), `alpha` (destination factor INVSRCALPHA), `additive` (destination factor ONE), `multiply` (DESTCOLOR, ZERO or ZERO, SRCCOLOR), `other` |
| `prim` | a comma list of `point`, `line`, `linestrip`, `list`, `fan`, `strip`, `rect`, `quad` |
| `vs`, `ps` | a 64-bit microcode hash in hex (the frame log's and `--native_dump_frame`'s hashes) |

`#` starts a comment; a line that cannot be read is skipped and logged with its line number. Examples:

```
opaque      ps=0123456789ABCDEF                  # Kong's fur shells: blended, but part of the scene
ignore      ps=FEDCBA9876543210                  # a HUD marker drawn into the scene target
transparent ps=1122334455667788 blend=off        # an opaque-looking water surface
```

The tracker (`AoFrameTracker`) then follows the frame's main scene target. A draw counts only at the frame's size
(the guest frame's pitch: shadow maps, the 640x360 and 320x180 post targets are left out) with a depth target. The
first depth-only draw starts the pre-pass; the first opaque draw with a colour and a depth target names the scene's
targets and ends the pre-pass; the first transparent or effect draw into those targets after it ends the opaque pass.
Draws into other targets in between (a reflection, a shadow map) change nothing. Each moment happens once a frame.

### The camera

XeGTAO needs the projection: the field of view (to turn a pixel into a view direction) and near and far (to turn
the stored depth into distance). The renderer already finds the world-view-projection matrix among the vertex
constants (`LooksLikePerspectiveRows`, the same search the placeholder pipeline uses: c0..c3 for the static world,
elsewhere for skinned meshes). From its rows: the w row is view z, the z row is a times it plus b, and the x and y
rows are px and py times view x and y (plus off-centre terms). With a world and view made of rotations, translations
and one uniform scale, the ratios of the rows' lengths give px, py and the off-centre terms exactly, and a and b
come out whatever the world and view are (`ExtractAoProjection`). Draws whose world matrix stretches an axis give
other values, so the frame's first 16 depth-tested scene draws vote, and the camera most of them agree on wins
(values within 0.5 %); a frame without candidates keeps the last frame's.

With the viewport registers (`PA_CL_VPORT_*` with the VTE enables, the window offset, D3D9's half-pixel offset) and
the target's render scale, `ComputeAoViewMapping` gives XeGTAO's `DepthUnpackConsts` (view z = mul / (add - stored
depth), with the viewport's depth range) and `NDCToViewMul / Add` for the working image. The picture XeGTAO works
on is the viewport's rectangle in host pixels (rounded as every scaled rectangle is), not the whole target (targets
grow to the tallest surface seen at their EDRAM placement). A test rebuilds view-space positions of world points
through this mapping at render scales 1, 1.5 and 2, full and half resolution, a picture inside a larger target, an
off-centre projection and a depth range.

### The XeGTAO passes

All compute, in host pixels of the scene target (so they follow `--native_render_scale`), on a working image padded to
a multiple of 16 (no pass writes outside its images; the padding repeats the picture's edge):

1. `cs_prepare`: the picture's stored depth into the working image (at half resolution every second pixel).
2. `cs_prefilter`: `XeGTAO_PrefilterDepths16x16`, view-space depth and its four smaller mips.
3. `cs_normals`: `XeGTAO_ComputeViewspaceNormal`, normals rebuilt from the depth.
4. `cs_main`: `XeGTAO_MainPass`, 3 slices of 3 steps per pixel (XeGTAO's "high"), no temporal noise (the renderer
   has no temporal accumulation, so the noise is the same every frame and the picture does not shimmer).
5. `cs_denoise`, `cs_denoise_last`: two of XeGTAO's edge-aware denoise passes (its "medium"; without TAA one is too
   noisy).
6. `cs_output`: one R32F image over the picture (1 = no occlusion), at half resolution upsampled with weights from
   the depth; stored depth at the far plane (the cleared sky) gets exactly 1.

Formats are R32_FLOAT and R32_UINT only: every Vulkan device can store to them without the
`shaderStorageImageExtendedFormats` feature (XeGTAO's own R8 images would need it). Memory at 1280x720: about 27 MB
at full resolution (31 MB with the resolve moment's copies), about four times that at render scale 2, about a third
at half resolution.

Strength and radius: ao_strength sets XeGTAO's final power (1.6, 2.2, 3.0 for strengths 1 to 3; 2.2 is XeGTAO's
own tuning). The radius is `--native_ao_radius` in world units, or by default the legacy AO's radius for the strength
(6, 8, 12) times the near plane distance, so both methods start at the same footprint. XeGTAO's own radius multiplier
(1.457), falloff and sample distribution are its defaults.

ao_mode 2 (the occlusion only): at the opaque moment the scene colour is replaced by the occlusion's grey (later
transparent draws still show on top, which shows what they cover); at the resolve moment the copy is the grey of the
occlusion under the fog, as with the legacy AO.

### Ambient-only: the translator hook

The ambient term is named in the shader's own microcode terms (`kkshaders/ambient.h`), since translated shaders have
no variable names:

| Term | Multiplied by the occlusion |
|---|---|
| `output <n>` | the colour written to target n (the whole lit colour; per shader rather than per frame) |
| `constant c<n>` | every direct read of pixel constant c<n> (an ambient colour constant) |
| `interpolator color<n>` / `texcoord<n>` | the interpolated input (baked lighting carried by the vertices) |
| `fetch <n>` | what the texture of fetch constant n gives (a light map, an ambient cube) |
| `register r<n> after <a>` | register n as the instruction at address a leaves it (`kkshaders translate --addresses` prints the addresses) |

followed by an optional component mask (default `rgb`; letters `rgba` or `xyzw`). Relative constant reads are not
multiplied (the game's pixel shaders read float constants directly; the skinning shaders that index constants are
vertex shaders).

The list (`--native_ao_shaders`): one line per shader, `<microcode hash> <term> [mask]`, `#` comments. Lines
`family <name> <term>` are for `kkshaders ao-list`, which expands them into hash lines from the game's shader
database; there a constant may be named as in the game's HLSL (`constant g_vAmbientColor`) and is found in each
shader's own constant table.

A listed pixel shader is translated with its term (its input hash includes the term, so it never collides with the
plain translation). It declares the `KKAmbientOcclusion` buffer at b4 (space 0): the occlusion's index in the
bindless 2D table, the picture's origin and size, and whether to apply it. The shader reads the occlusion at its
pixel position once and multiplies the term. The game renderer adds b4 to the pipeline layout only in ambient mode;
it points the buffer at the occlusion for draws into the scene after the pre-pass and at nothing (factor 1) for
every other draw. Without a term the translator's output is byte for byte what it was (the 1,557 translations of the
test corpus and their input hashes compared with the commit before the hook).

Hooked shaders are not in the shader packs: DXC compiles them at their first use (a stutter the first time each
appears). If the mode is adopted, `kkshaders db-build` should learn to read the list and build them into the pack.

## Settings

| Setting | Values (default first) | What it does |
|---|---|---|
| `ao_mode` | 0, 1, 2 | the launcher's: off, on, the occlusion only. Read once a frame by name, as today (defined by the Xenos plugin, which the launcher preloads) |
| `ao_strength` | 1, 2, 3 | the launcher's: legacy tuning sets, or GTAO's final power and default radius |
| `--native_ao_method` | `legacy`, `gtao` | which occlusion. legacy is today's, unchanged |
| `--native_ao_apply` | `resolve`, `opaque`, `ambient` | when the GTAO occlusion reaches the picture (above). The legacy method always uses resolve (a warning is logged for another value) |
| `--native_ao_quality` | `full`, `half` | GTAO's working resolution (of the scene target at the render scale) |
| `--native_ao_radius` | 0, any number | GTAO's radius in world units; 0 from the camera |
| `--native_ao_rules` | a file | classifier rules before the built-in ones |
| `--native_ao_shaders` | a file | the ambient-only lighting shader list |

All `--native_ao_*` settings are read at start. ao_mode and ao_strength can change at any time, as today. With the
defaults the GTAO code is never created: the game renderer's layouts, pipelines and draws are the ones it always had
(the test `GameAo_OffChangesNothing` and the whole existing suite).

`--native_dump_frame=N` with AO on adds an `AO draw` line per draw of frame N: its class, whether it goes to the
scene, which part of the frame ended before it, and whether the occlusion was worked out, applied or read by it,
with the state the rules matched and the pixel shader's hash. Every 600 frames with AO on, one line says where the
occlusion went (`AO last 600 frames: ...`).

## Tests (Linux, lavapipe, Khronos validation on)

| Test | Checks | Result |
|---|---|---|
| `nr_ao_rules_tests` | blend kinds; the built-in table on every draw state of a game frame; rule files (first match, round trip, bad lines reported); the settings' names and defaults; the tracker through a frame (shadow map, sky, pre-pass, opaque pass, a reflection in between, water, the next frame, no pre-pass), depth validity across clears; the projection from a matrix (world scales 0.01 to 37, off-centre, refusals); the matrix found at c36 behind bone rows; the vote; view space rebuilt at scales 1, 1.5, 2, full and half, an inner picture, a depth range | 11 tests pass |
| `nr_gtao_tests` | `Gtao` on a ray-cast depth image (box on a floor in a corner of two walls, sky): creases darkened, open floor, walls and sky not; strength; half against full; scale 2 against 1 after an area average; a picture inside a larger target gives the same result; the apply passes (multiply, show, the copy, alpha and outside untouched); D32S8 targets and the depth copy; refusals | 8 tests pass. Crease 0.745, box foot 0.663, open floor and wall 1.000, sky exactly 1; half against full mean 0.007; scale 2 against 1 mean 0.004 (full), 0.007 (half) |
| `nr_game_ao_tests` | the whole path (hooks, tracker, game renderer, translated shaders, the camera in c0..c3): depth pre-pass, opaque pass with depth writes off, alpha-blended water and additive particle after it | 8 tests pass, see below |
| `kkshaders_ambient_tests` | the hook on the generated corpus: formats; plain translations and hashes unchanged, vertex shaders never change; for every pixel shader every kind of term it has, the hooked text is the plain text plus exactly the hook, its own input hash; one term per shader built to signed DXIL and SPIR-V | 822 pixel shaders, 3,282 hooked translations, 822 built, 735 vertex shaders unchanged |

`nr_game_ao_tests` in numbers: creases at 0.66 to 0.73 of their unoccluded brightness, open floor 1.000. At the
opaque moment the water (alpha 0.5) is within 0.0018 of half its colour plus half the occluded scene and the
particle within 0.0018 of the occluded scene plus its colour, where the legacy moment would put the water 0.098
away. Ambient-only with the lit shader's interpolated colour as its term gives the opaque moment's frame exactly
(difference 0.0000); without a pre-pass it falls back to the opaque moment. A rule file counting the water's shader
as opaque moves the moment to the particle (water darkened with the scene within 0.0003, particle not). The resolve
moment fills only the scene copy (the game's own target equals the frame without AO). Render scale 2 against 1
after an area average: mean difference 0.0028 (full) and 0.0045 (half). ao_mode 0 with every method and moment,
and no AO settings at all, give the same frame to the bit. The dumped frame names the four moments.

At half resolution the box's thin contact shadow is softer at scale 1 (a 96x54 working image) than at scale 2, so
the probes allow 0.08 there against 0.04 at full resolution; the mean differences are small either way.

The existing tests all still pass (`ctest`: 11 of 11 in the renderer build, 17 of 17 in the translator's own,
including the GPU differential run).

How to run them (Linux; the SDK built from source with every patch as `backend.md` says, DXC's Linux release):

```
cmake -S native-renderer -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 \
      -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_PREFIX_PATH=/opt/rexsdk/linux-amd64 -DNR_DXC_DIR=<dxc release>
cmake --build build && ctest --test-dir build --output-on-failure
cmake -S native-renderer/shaders -B build-shaders -G Ninja -DCMAKE_CXX_COMPILER=clang++-20 \
      -DKKSHADERS_SDK_DIR=<sdk source> -DKKSHADERS_DXC_DIR=<dxc release>
cmake --build build-shaders && ctest --test-dir build-shaders --output-on-failure
```

`NR_TEST_IMAGES=<dir>` writes the frames and occlusion images as PPM files.

The `gtao_*` shader headers were made with DXC 1.8.2505.1 (the Linux release; signed DXIL). The other headers were
not regenerated (they were made on Windows with v1.9.2609); `compile_shaders.ps1` on Windows remakes them all with
one version.

## Decisions

- XeGTAO is vendored unchanged and adapted from outside (defines before the include), so an update is a copy.
- Compute passes (XeGTAO's own shape), R32 formats only (portable storage), a working image padded to 16 (no
  out-of-bounds writes, which Vulkan does not discard without robustness features).
- No temporal noise: the renderer has no TAA, and a fixed pattern does not shimmer; two denoise passes.
- The opaque moment multiplies the target in place with a blend instead of copying it: no extra image, no read of the
  target.
- The camera comes from the matrix the game already gives its shaders, voted per frame; no game code is read.
- Strength and radius start at the legacy AO's footprint, so the first comparison in the game is like for like.
- The legacy method stays as it is and stays the default; it ignores `--native_ao_apply`.
- The ambient term is named in microcode terms (output, constant, interpolator, fetch, register), because that is
  what the translator can see; the tool resolves the game's constant names per shader.
- Each moment happens once a frame, on the first scene found at the frame's size with a depth target.

## The Windows checklist

What only the game can settle, in the order to do it. Runs as in `testing.md` (`tests/run.ps1`) and `backend.md`;
keep `--kk_original_look_keep=ao_mode,ao_strength` on every run that sets ao_mode (the Original look forces it to 0).

1. **Build and the tests.** Fetch `cloud-p3-ao`, build `kk-dev-vulkan`, and the standalone renderer build with
   `-DNR_BUILD_TESTS=ON` (`backend.md`, "Tests on Windows"). Run `nr_ao_rules_tests`, `nr_gtao_tests`,
   `nr_game_ao_tests` and the rest; build `native-renderer/shaders` and run its `ctest` (the new `ambient` test).
   Optionally run `compile_shaders.ps1 -Dxc <dxc 1.9.2609>` so every header has one DXC version; the SPIR-V of the
   old shaders should come out unchanged.
2. **Defaults unchanged.** `tests/run.ps1 -Plugin native` on all 17 scenes with the defaults: the same scores as the
   last run (`runs\native-final-default`). Then once with `-ExtraArgs "--native_ao_method=gtao
   --native_ao_apply=opaque"` and ao_mode 0: the same scores again (the GTAO object exists but does nothing).
3. **Which frames to dump.** One frame each from: V-Rex (`vrex_110`'s shot), Venture (rain, the ship, water),
   Kong to the Rescue (`kong`: fur, light shafts, fog), chap33 (the cave with the water channel), chap12 (mist), and
   the main menu (its 3D backdrop). Run each with `--native_ao_method=gtao --native_ao_apply=opaque --ao_mode=1
   --ao_strength=1 --native_dump_frame=<the shot's frame>` and keep the `AO draw` lines and the `AO last 600 frames`
   lines.
4. **Read the dumps.** In each frame expect: the pre-pass as `depth, scene`; the scene's solid geometry as `opaque,
   scene`; exactly one draw with `opaque pass ended before it, occlusion worked out, occlusion applied`, and that draw
   is the first water, glass, particle, smoke or fog draw of the scene; everything after it is not solid geometry.
   Write down for each frame: that draw's index and pixel shader hash; any blended draw before it that is really part
   of the scene (Kong's fur shells, alpha-blended foliage, decals) and any solid draw after it; whether the scene's
   targets are the right ones (a draw at the frame's size into another target first, such as a reflection, would be
   taken for the scene). The stats lines should show no moments without a camera or depth. A menu frame with no AO
   anywhere is fine (no depth-tested scene).
5. **Fill the classifier table.** Put rules for what step 4 found in a file (format above, hashes from the dump
   lines, or a whole family with `kkshaders ao-list xeshaders.bin --family psfur` to list its hashes) and run again
   with `--native_ao_rules=<file>` until every dumped frame has its single moment at the right draw. Then move the
   confirmed rules into `kDefaultAoRules` (`backend/ao_classifier.cpp`, before the built-in lines) so no file is
   needed, and add a line to `nr_ao_rules_tests` for each.
6. **Check the camera.** With `--ao_mode=2` the occlusion should sit exactly on the geometry: dark lines in the
   corners where walls meet the ground, under the V-Rex's feet, along Jack's contact with the ground, not shifted by a
   pixel and not on the sky. At `--native_render_scale=2` the same. If it is everywhere or nowhere, the camera is
   wrong: look for the "no camera found" warning, and check in a `--native_dump_frame` of the old placeholder
   (`--native_game=false`) which register holds the world-view-projection of the static world.
7. **Strength and radius.** Compare ao_mode 2 of both methods on the same shots (V-Rex, Venture, Kong): the GTAO
   picture should be at least as dark in creases and cleaner on flat ground. If its footprint is too small or large,
   set `--native_ao_radius` in world units (the log's first GTAO line says "radius from the camera"; the near plane
   is in the camera, `near_plane()`), and write the value and the shots it was chosen on here.
8. **Against the old AO and the golden frames.** Same shots, ao_mode 1, strength 1, legacy against
   `gtao` + `opaque`:
   - crops side by side of water (Venture, chap33), smoke and particles (V-Rex), fog (Kong, chap12): the legacy AO
     darkens them, the GTAO must not;
   - elsewhere the darkening should be of the same kind; the MAE between the two methods' frames should be of the
     order of the legacy-against-Xenos numbers in `backend.md` (3 to 7);
   - the golden scenes with AO off still pass (step 2);
   - frame times (`backend.md`, "Frame rate") at scale 1 and 2, with `--native_ao_quality=full` and `half`. The GTAO
     passes run once a frame; at 1280x720 they should cost well under a millisecond on the RTX 4070 Ti. Note them here.
9. **Ambient-only: find the lighting shaders and their ambient term.** This needs the real shaders.
   - From the dumps of step 3, list the pixel shader hashes of the `opaque, scene` draws (the scene's lit surfaces).
   - `kkshaders ao-list kk\assets\Shaders\xeshaders.bin` prints each pixel family's constants (by name and register),
     inputs and samplers; `--family psgeneric` lists every shader with its own table. Read the family's HLSL source
     (the 37 sources in `xeshaders.bin`; `kkshaders ao-list xeshaders.bin --sources <folder>` writes them out) and find how the ambient light is
     added: an ambient colour constant (`constant <name>`), per-vertex baked light (`interpolator color0`), a light
     map or ambient cube texture (`fetch <n>`), or a computed value (`register r<n> after <address>`, the address from
     `kkshaders translate <container> --addresses`). Check one shader's hooked translation with `kkshaders translate
     <container> --ambient "<term>"`.
   - Make the list: `kkshaders ao-list xeshaders.bin --family psgeneric --term "constant g_vAmbientColor" -o
     ao-shaders.txt` (the constant name is an example), more families appended the same way, or a list file with
     `family` lines expanded with `--list`.
   - Run with `--native_ao_method=gtao --native_ao_apply=ambient --native_ao_shaders=ao-shaders.txt`. The log should say
     how many shaders are listed; the stats lines should count every frame as ambient-only. Compare with the opaque
     moment: sunlit surfaces should keep their direct light and only the ambient part darken. The first run compiles
     every listed shader with DXC (a stutter); a pack with them is future work.
   - Write here, per family: the term, the shaders it covers, and the frames it was checked on.
10. **Report and decide.** With steps 2 to 9 done: whether `--native_ao_method=gtao` and which apply mode become the
    defaults (and the launcher's AO switch with them), the radius, the rules, the shader list.

## Known limits and open questions

- Not run in the game yet: the rules, the camera vote and the moments are designed from `d3d-api-map.md`,
  `backend.md` and the legacy AO's detection, and tested on synthetic frames that follow the game's pattern.
- The tracker takes the first frame-size scene it sees; a frame-size pass into another target before the scene
  (a reflection) would be taken for it (step 4 checks this; a rule can `ignore` its shaders).
- Each moment happens once a frame: a second scene pass in the same frame gets no occlusion.
- A world matrix that is not rigid with a uniform scale gives a wrong projection; the vote keeps the majority.
- Relative reads of a constant term are not multiplied.
- Listed (ambient-only) shaders are compiled at first use; packs do not hold them yet.
- XeGTAO repeats the picture's edge for samples off the picture: the occlusion is a little darker in the corners of a
  flat surface that faces the camera (0.94 at worst in the test, 1.00 sixteen pixels in).
- D3D12 is built (DXIL headers) but not run: NVRHI's D3D12 backend, the root signature with b4 and the compute passes
  need a Windows run with `--native_backend=d3d12`.
