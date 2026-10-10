# Phase 2b cloud sessions

Same container rules as `cloud.md` (read it first): Linux, no GPU, no game files, never fetch or recreate game
data, SDK v0.10.0 public, lavapipe for Vulkan, DXC Linux release. State: on branch `nr-integration` the native
renderer draws the game with its own shaders, textures and states and passes all 13 golden scenes on Vulkan
(and each on D3D12); see `native-renderer/docs/backend.md`, section "Phase 2: the game renderer", its scores
and "Open problems (Phase 2)". Start from `nr-integration`, work on a new branch `cloud-p2b-<name>`, push it,
never push to `main`, `native-renderer` or `nr-*`. Read `design.md`, `backend.md`, `shaders.md`, `formats.md`,
`testing.md` and `d3d-api-map.md` first. Build the SDK from source with every patch (`tools/rexglue-patches`,
then `native-renderer/sdk-patches`) as `backend.md` describes. Plain language, no em dashes, small commits. Do
not ask questions: decide, write it down, carry on. Everything new is behind an option that defaults to the
current behaviour, so the golden scenes keep passing. Finish with a report: what is on the branch, what is
verified and how, and the exact Windows commands to check it against the game.

## D: Linux tests, frame log and pipeline cache

> Read `cloud-phase2b.md` and the docs it lists. Start from `nr-integration`, work on `cloud-p2b-infra`.
> (1) Since the Windows agent made `--native_element_endian` the default (each vertex element read with its own
> endian field), re-run `nr_draw_tests`, `nr_render_tests`, `nr_game_tests`, `nr_plugin_tests` and the shader
> tests on Linux under the Khronos layer and fix them; add tests for elements whose endian differs from the
> buffer's. (2) Make the native plugin write `REX_DEV_FRAME_LOG` lines in the format the harness reads
> (`testing.md`, and patches 0003 / 0012 in `tools/rexglue-patches`), including the vertex shader hash the
> Xenos log uses (XXH3-64 of the microcode after the library patched its vertex fetches for the declaration:
> reproduce that patching from `d3d-api-map.md` / `d3d-structs.md`), with tests. (3) Cold pipeline caches:
> pipelines are created when a draw first needs them and the first visit to a chapter stalls for seconds.
> Record every pipeline description to a file under the cache root and create them all on worker threads at
> start; with `--native_pipeline_wait` choose between skipping a draw whose pipeline is not ready (as the Xenos
> plugin can) and waiting. Tests with synthetic draws.

## E: resolution independence

> Read `cloud-phase2b.md` and the docs it lists. Start from `nr-integration`, work on `cloud-p2b-scale`.
> The renderer draws at the 360's 1280x720. Add a render scale (`--native_render_scale`, 1 by default; 1.5, 2,
> 3 and arbitrary sizes such as the window's): render targets keyed by EDRAM base / pitch / format get host
> sizes multiplied by the scale; viewports, scissors, clear rectangles and resolve rectangles and destination
> points scale with them; resolves into textures produce scaled textures, and the shaders that sample them
> (post effects, shadow masks, the screen copies) must see coordinates that still work (the translator's
> unnormalized texture fetches and any texel-size constants are the risk: find them in the shader corpus and the
> game's HLSL families' use, and decide per case, documented). Shadow maps (k_32_FLOAT 832x832) get their own
> scale option. Keep a 1:1 mode bit-identical to today. Tests on lavapipe with synthetic frames: a scene drawn at
> scale 1 and scale 2 must match after downsampling within a stated tolerance; resolves, render-to-texture and a
> full-screen post pass included. Then the presenter side: the frame image handed to the SDK presenter is the
> scaled one, and its existing upscalers and FXAA still apply.

## F: ambient occlusion on the native renderer

> Read `cloud-phase2b.md` and the docs it lists. Start from `nr-integration`, work on `cloud-p2b-ao`.
> The port has screen-space ambient occlusion in its Xenos plugin (patches 0004 to 0007 in
> `tools/rexglue-patches`, described in that folder's README): it notices the scene depth being resolved out of
> EDRAM before lighting and multiplies the occlusion into the colour at the next full-screen colour copy, under
> the fog. Port the technique to the native renderer, where depth and colour are ordinary host textures:
> the same detection points (the resolves are hooked as `Resolve`), the same settings (`ao_mode` 0 off, 1 on,
> 2 show AO only; `ao_strength` 1 to 3; read them the way the Xenos plugin does so the launcher's Graphics page
> keeps working), half-resolution occlusion, depth-aware blur, edge-aware upscale, distance fade, written as
> HLSL compiled to SPIR-V and DXIL like the other renderer shaders. Make it scale with `--native_render_scale`
> if session E has landed (check the remote branches), otherwise design it to. Tests on lavapipe with synthetic
> depth and colour.

The Windows side keeps the game-dependent work: running the golden scenes with each branch, tuning against the
real frames, and merging.
