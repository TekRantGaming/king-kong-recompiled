# Task 124: SMAA and the narrow LOD experiment

15 October 2026. Worktree `F:/KK-native-renderer/wt/gfx-lod`, branch `gfx-lod`, starting commit `7cdfd85`. Implementation commit: `43f6d172d492c9ad8479e63f19fbd857547831e2`.

**SMAA is implemented and tested on Vulkan and DirectX 12. The LOD investigation is partial: the proposed selector-only bias does not control the audited drawing path, so no Detail distance setting was added. All 72 CTests pass. Exactly six game launches were used, including the completed recorded-route benchmark.** No push, merge, release or public package.

Read DECISIONS.md first, then GAME-RULES.md, REPORT-122.md and TASK-124. No applicable AGENTS.md was found. There was no standing-decision conflict: task 124 accepts the existing official v2.8 BSD-style release. No new downloads, Linux work, HFR/AI cadence changes or changes to graphics defaults.

The approved archive SHA-256 is `f6192b0759f3a9f52769f328832f61870849d2ba19dbae7718e0d76d3411befc`. Vendored its SMAA.h, AreaTex.h, SearchTex.h and LICENSE.txt under `native-renderer/thirdparty/smaa`. Source encoding/whitespace is normalized; the only shader adaptation is explicit s0/s1 bindings for the equivalent linear/point clamp samplers. The lookup texel arrays are unchanged. The full licence and required six-author credit are in `kk/THIRD-PARTY-NOTICES.txt` and the launcher credits. CMake copies the notices beside the executable; the Windows builder copies them into staging.

The implementation is **SMAA 1x, High preset, luma edges, zero subsample indices**: edge detection, blending weights, neighborhood blending. It has no temporal resolve, history, jitter, generated frames or added frame queue. It uses the existing gamma-ramp/source copy, then AA at the native frame-image dimensions. Both RGBA8 scratch images are cleared, including alpha, every frame. The embedded area texture is RG8_UNORM 160x560; the search texture is R8_UNORM 66x33. Neighborhood blending uses the display image's gamma space, explicitly permitted by v2.8's integration instructions.

Six DXIL and six SPIR-V blobs are checked in. All three pipelines, including neighborhood variants for the native frame formats, are created during renderer initialization and therefore during PipelinePrewarm. They and the lookup images live inside the plugin; no new game shader-pack entries or loose lookup files are needed. Runtime smoke runs successfully loaded the existing 17,924-entry packs and prewarmed the shipped pipeline list, with no missing-shader waits. The runtime's 10-bit frame format is covered by a new Present comparison test.

Startup and live settings both recognize `swap_post_effect=smaa`. Launcher Graphics and F8 offer Off / FXAA / SMAA. F8 labels and the temporal-upscaler note cover English, German, Spanish, French and Italian; SMAA is language invariant. The 136-key five-language text audit passes. Existing defaults/presets remain FXAA where they were FXAA. Original retains its original treatment. The existing live upscaler path suppresses and restores SMAA; the live-switching GPU test checks this. Off in the comparisons means added post-AA off; the game's own MSAA is unchanged.

All image and cost evidence is in `F:/KK-native-renderer/documentary/gfx-lod/smaa/`. Four reviewed comparison sheets are `vulkan-1440_comparison.png`, `vulkan-2160_comparison.png`, `d3d12-1440_comparison.png` and `d3d12-2160_comparison.png`. There are **48 individual 384x384 native-pixel crops**, full processed images, original runtime Off/FXAA/SMAA captures, input PPMs, source JSON files, cost logs and `measurements.csv`.

Each comparison uses the exact same captured Off frame through each GPU AA pass, avoiding movement between the three columns. Crops are not rescaled or enhanced. The original runtime captures at level-clock seconds 22, 30 and 40 separately verify live switching and native integration. SMAA smooths the mast/railing edges while retaining more rock texture than FXAA in these samples. The default was not changed from this single-scene comparison.

RTX 4070 Ti measurements, milliseconds:

| API | Native frame | Same-frame FXAA | Same-frame SMAA | Actual game SMAA | Timed game frames |
|---|---:|---:|---:|---:|---:|
| Vulkan | 2560x1440 | 0.322 | 0.362 | 0.389 | 600 |
| Vulkan | 3840x2160 | 0.654 | 0.749 | 0.664 | 1,200 |
| DirectX 12 | 2560x1440 | 0.324 | 0.350 | 0.353 | 1,200 |
| DirectX 12 | 3840x2160 | 0.659 | 0.739 | 0.631 | 1,200 |

Same-frame values are GPU timestamp medians over 24 frames after eight warmups on the captured RGB8 image. Actual-game values are means of 600-frame timestamp windows around SMAA's three passes and clears on the native frame image. They exclude the common gamma/source copy, allocation and first lookup upload. `measurements.csv` also retains the Off texture-copy benchmark; it is not an entire Off game frame. These are one outdoor scene and one GPU, with ordinary clock/desktop variation, not a general performance guarantee.

The narrow LOD experiment is developer-only (`dev_lod.cpp`, enabled only by `KK_DEV_LOD_PROBE` / `KK_DEV_LOD_BIAS`). It runs the original selector once, computes the alternative threshold comparison using `min(255, importance*bias)`, and changes only its output child pointer for the two audited rendering callers. It preserves the original return mask and does not write the importance byte, authored thresholds, resource+9, visibility or activation. No production LOD control or global multiplier was introduced.

The concrete finding is that `sub_827D2080` writes its chosen child through r4. In `sub_82876FA0` (return address `0x82877044`) and `sub_82876D00` (`0x82876D70`), r4 is stack+80, and that output is not read afterward. These callers use the return mask and dispatch the resource's +24 callback. The LOD callback `sub_827D2028` instead reads **child zero at resource+16**, temporarily installs it in the wrapper, calls its drawing callback, then restores the wrapper. Altering only the selector's comparison/output therefore does not make those callers draw the newly chosen child.

The outdoor bias-4 run recorded **14 distinct selector samples across eight objects**, including **five applied changes of child index**. Immediate before/after comparisons found **zero mismatches** in object bytes 0..31 (including importance +28 and activation +31), descriptor flags at `*(object+24)+46`, and the associated +165/+166/+167 fields consumed by the AI path identified in REPORT-122. The draw trace recorded eight objects; five draw children differed from the selector output, consistent with the first-child dispatch. This is a bounded sample and a static dispatch finding, not a certification of all gameplay state across the whole game.

Raw evidence: `F:/KK-native-renderer/runs/task124-vulkan-2x-lod4-20261015-210513/lod.csv` and `lod.csv.draw.csv`; summary: `kk/out/task124/lod-summary.json`. Read-only assembly extracts and analysis scripts remain under `kk/out/task124/`. The independently reselecting `sub_82815850` / `sub_827D1F90` path, the stored resource+9 importance and the AI consumers from REPORT-122 remain unchanged.

This stop reason is narrower than task 122's shared-byte coupling: the permitted pointer/comparison change is ineffective in the traced drawing dispatch. Making the callback consume a different child would require a further change outside this comparison-only experiment. **No Detail distance UI is offered.** There is no working distance feature to supply a truthful pop-in before/after pair or CPU/GPU delta for. The raw captures and frame CPU/GPU windows are retained, but are not presented as a distance improvement or as an overhead benchmark for a nonexistent setting.

Validation results:

| Suite | Result | Seconds |
|---|---:|---:|
| Native renderer, both APIs and new SMAA cases | 38/38 pass | 148.22 |
| Development/settings/launcher/panel/pacing/lock | 9/9 pass | 2.65 |
| Resources | 2/2 pass | 4.19 |
| Shader CPU/database/interpreter | 22/22 pass | 350.96 |
| Shader GPU differential | 1/1 pass | 125.54 |
| Total | **72/72 pass** | |

The native and development suites were rerun after switching to SDK 041; unchanged resource/CPU translator results were retained. The GPU differential rerun compared 1,761 shaders: zero translation/compilation failures, zero mismatches, with the existing 27,642 fragile invocations, 348,958 unknown components and four nonterminating rounds excluded. NVRHI checks and all generated SPIR-V validation pass; the Khronos validation layer is not installed. No tolerance or exclusion was weakened.

New SMAA tests check diagonal smoothing, flat RGBA preservation, scratch clearing, repeated frames, resizing, in-place rejection and GPU costs on both APIs. The native-format Present test matches the embedded pass; live upscaler switching suppresses/restores SMAA. A first Vulkan pixel failure exposed a missing push-constant annotation, corrected before game testing. The initial failure log is retained. Two capture-comparison wrapper attempts treated ordinary timing diagnostics on native stderr as fatal; the wrapper was corrected and both comparisons rerun successfully without another game launch. Final logs/images are complete. Generated-header whitespace cleanup was verified not to change any shader byte; hashes are in `smaa-blob-sha256.log`.

Exactly six launches, all private run folders under `F:/KK-native-renderer/runs/`:

| Run folder | PID | Result |
|---|---:|---|
| task124-vulkan-2x-lod1-20261015-205637 | 48544 | SDK 038 attempt stalled before the chapter; stopped, no captures |
| task124-vulkan-2x-lod4-20261015-210513 | 10596 | SDK 041, 1440p Off/FXAA/SMAA, LOD probe |
| task124-vulkan-3x-lod1-20261015-211059 | 26536 | SDK 041, 4K Off/FXAA/SMAA |
| task124-d3d12-2x-lod1-20261015-211245 | 48480 | SDK 041, 1440p Off/FXAA/SMAA |
| task124-d3d12-3x-lod1-20261015-211347 | 48492 | SDK 041, 4K Off/FXAA/SMAA |
| task124-vulkan-2x-lod1-20261015-211449 | 24804 | Recorded-route benchmark complete, SMAA 1440p |

The last run completed `chapter8-camera-v1`: 10-second warmup and 75-second replay, **4,500 game and 4,500 host intervals**. With the requested 60 FPS cap, game p50/p99/worst were 16.667/16.813/17.062 ms; host-return p50/p99/worst were 16.666/16.872/34.980 ms. This is a capped functional/pacing check, not a baseline or uncapped performance comparison, and host returns are not scanouts. Results and every interval are in `.../task124-vulkan-2x-lod1-20261015-211449/userdata/benchmarks/run-1792095295954714/{results.txt,frames.csv}`. No further launch was made.

All launches used SDL activation hints 0, windowed 1280x720, fullscreen false, desktop fullscreen mode, the existing kkbg2 helper, explicit private userdata/cache/config/log paths and this worktree's assets. No window was brought forward and no desktop screenshot was taken. Only task-started PIDs were stopped. Runs were below three minutes. The lock/priority contract was respected, including waiting for other tasks and checking for any existing game. The task-owned priority file was link-checked and moved to `F:/KK-native-renderer/moved-aside/task124-priority-20261015-211723.txt` after all work using the slot; other tasks' locks/processes were left alone.

Final builds use permanent **SDK 041**, as the verified baseline REPORT-113 did: private CMake caches explicitly set `CMAKE_PREFIX_PATH=F:/KK-native-renderer/sdk-vulkan-upscalers-041/win-amd64` and `rexglue_DIR` to its `lib/cmake/rexglue`. The worktree's SDK 038 junction and shared SDKs were not changed. Output rexruntime.dll matches SDK 041 SHA-256 `9f2e78c4153362e197ae16bfafecc3e9ecda11ccc03460d2fb916c461772b2fd`. The initial SDK 038 attempt and rejected older pack remain recorded; their stall is not diagnosed further by this task. Successful runs used copied baseline preparation packs plus shipped pipeline/resource hints. Bare CLI pack-check logs omit the launcher's default.xex builtin containers, so their source-hash mismatch is not a launcher currentness test; actual runtime acceptance is logged in every successful run.

Build/test/evidence scripts and logs are in `F:/KK-native-renderer/wt/gfx-lod/kk/out/task124/`; build directories reuse `kk/out/task122/{native,resources,shaders,dev}`. Binary checks find NVSDK_NGX and xess; all six upscaler DLLs, DXC and notices were present in the private runs. No test package is handed over, so no owner shader-cache cleanup was performed. No user game/save/cache locations were accessed or modified. No game-derived images, dumps, packs or logs are committed. The two build-generated line-ending-only rewrites were backed up privately, link-checked and restored.

Updated only REMASTER GRAPHICS items 6 and 7 in `F:/KK-native-renderer/WORK-LIST.txt`: **partial** for the ineffective narrow LOD path, **done** for SMAA. Task 121's sampler/effect-buffer work and task 123's GI/cursor/sliders were not imported. Integration review should preserve task 121's CAS pass after whichever AA mode is selected when combining the Present edits. Claude's verification/review is the required next gate before any use; no push, merge or release was performed. Stop here.
