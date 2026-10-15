# Task 105: Beta 6 integration, no release

15 October 2026. Worktree `F:\KK-native-renderer\wt\beta6-int`, branch `beta6-int`, base released Beta 5 `e890ece`.
Integration source HEAD: `4d599bc`. All seven requested branches are merged in order. Both game builds pass and all three allowed game sessions completed normally. **The all-CTest gate is not met: 64/65 required CTests pass.** The remaining shader GPU differential failure matches the retained Beta 5 control exactly, including all 279 printed mismatch/result lines.

Read DECISIONS.md first, then TASK-105, GAME-RULES and REPORT-094, REPORT-098, REPORT-100, REPORT-096, REPORT-095, REPORT-103 and REPORT-101. No standing-decision conflict required skipping a task part. No applicable AGENTS.md was found. This is an integration handoff for Claude's verification, not release approval.

## Merges

Every merge used `--no-ff`; all seven merge commits end with the required Claude co-author trailer.

| Order | Branch | Imported head | Merge commit |
|---|---|---|---|
| 1 | cleanup-beta6 | a6144aa | 0b99711 |
| 2 | hw-beta6 | cf7d234 | f7a5b26 |
| 3 | vkold-beta6 | 75b1ac2 | 7318292 |
| 4 | vram-beta6 | 3ea4512 | 64d1046 |
| 5 | f8-beta6 | 8f181c2 | 06ea04a |
| 6 | pace-beta6 | a0d5020 | 177e131 |
| 7 | fx-beta6 | 871bf12 | 4d599bc |

`opt-beta6` and `hfr-beta6` were not merged. No beta line, main, or other worktree was edited. The displayed application version remains `2.0.0-beta.5`; no version bump was requested.

### Conflicts and resolutions

- **vkold-beta6:** conflicts in `backend/tests/game_upscale_tests.cpp`, `vk_test_device.cpp` and `vk_test_device.h`. Retained both the cleanup queue-ownership regression and the reduced-table startup/live-upscaler regression. Kept cleanup's single mutex, `RunExclusive`, locked submission/wait/readback, and guarded callback registration/destruction. Retained incoming descriptor-capacity and extension support. Removed the duplicate upscaler include introduced by the overlap.
- **vram-beta6:** conflicts in `backend/backend.cpp`, `host_device.h`, `backend/tests/vk_test_device.cpp` and `.h`. Combined descriptor capacities/draining with the memory-budget callback and both host interfaces. Kept one test-host queue exclusion implementation, one mutex, one out-of-line readback implementation and one advertised device-extension list. Retained the supported push-descriptor extension needed by live DLSS warmup; removed duplicate includes/fields/assignment from the overlap.
- The other five merges completed automatically. Reviewed the combined test registrations and input-hook ordering. Benchmark input replacement and F8 controller capture are both retained. The launcher and F8 lighting visibility guards remain closed; task 101's effects stay hidden.

There are no additional production fixes beyond these merge resolutions.

## SDK 041

Built a complete independent source copy at `F:\KK-native-renderer\sdk-src-upscalers-041` from SDK 038 plus `0041-pacing-benchmark-present-observer.patch`, installed at `F:\KK-native-renderer\sdk-vulkan-upscalers-041`.

- Followed the `codex/build-sdk-069.ps1` configure/build/install pattern: full Release rebuild, Vulkan and D3D12 enabled, existing native-renderer FidelityFX integration retained.
- Copied source and independent Git/submodule metadata, without the old build directory. Initial preparation/configure attempts exposed CRLF empty patch context and missing copied submodule markers; normalized only the patch's line endings and copied the markers before the successful full build. No SDK 038 files were edited.
- The three patched SDK files match pace-beta6's private validated copies after line-ending normalization. `rexruntime.dll` exports `RexSetPresentObserver096`; the benchmark records successful host present returns. Both D3D12 game sessions log maximum frame latency 1 with the waitable swapchain.
- Preserved the Python `-I` build adjustment in the copied glslang tree. Disabled CMake package-registry registration in the new SDK copy so installation does not change other worktrees' package discovery. Copied the existing FidelityFX subtree into the new install.
- Repointed only this worktree's `tools/rexglue-vulkan` junction. Preserved the old junction at `out/task105/rexglue-vulkan-038-link` instead of deleting it. Other SDK/upscaler/asset/generated-code links remain at their permanent targets; `mkwt.ps1` is unchanged.
- Before/after hashes of SDK 038's three pacing source files and installed runtime are unchanged. Evidence: `out/task105/sdk038-before.json`, `sdk038-after.json`.

Reproduction and logs are in `F:\KK-native-renderer\wt\beta6-int\out\task105\`: `sdk-105.ps1`, `sdk-configure.log`, `sdk-build.log`, `sdk-install.log`, `0041-lf.patch`. The source/install copies are permanent directories, not links to another worktree.

## Builds and automated validation

| Check | Result |
|---|---|
| kk-dev-vulkan | PASS |
| kk-beta | PASS |
| Development CTest | 7/7 PASS |
| Standalone native-renderer CTest | 33/33 PASS, 106.05 s |
| Resources CTest | 2/2 PASS |
| Shader CTest excluding GPU differential | 22/22 PASS, 235.26 s |
| Shader GPU differential CTest | FAIL, 188.14 s |
| Required CTest total | **64/65; gate failed** |
| Standalone pacing/benchmark CTest | 1/1 PASS |
| No-game presenter scheduling checks | 16/16 PASS |
| git diff --check | PASS |

The beta top-level build registers zero CTests with developer tools off; its production renderer sources were tested through the full standalone suite. Development CTests include hardware recommendation, launcher logic, F8 logic/text, startup recovery, pacing/benchmark and VSync settings. The native suite includes both APIs' memory migration, reduced descriptor tables, queue exclusion, live upscaler switching, GI, lighting and SPIR-V validation. Khronos validation is unavailable on this installation; NVRHI validation checks pass. No Khronos-validation-clean claim is made.

Both game configurations report FSR, DLSS and XeSS enabled. `llvm-strings rexgpu-native.dll` finds `NVSDK_NGX` and `xess` in both. Each game output contains DLSS, XeSS, both FidelityFX API DLLs, both AMD loader/upscaler DLLs and DXC. Neither game output contains `rexgpu-xenos.dll` or another Xenos plugin file. File hashes and checks are in `out/task105/binary-checks.json`.

Logs: `dev-build.log`, `beta-build.log`, `dev-ctest.log`, `renderer-ctest.log`, `resources/Testing/Temporary/LastTest.log`, `shaders-cpu-ctest.log`, `shaders-gpu-ctest.log`, `pacing-ctest.log`, `presenter-tests.log`. Build/test helpers are retained in the same ignored evidence directory.

### Remaining CTest failure: exact Beta 5 match

The integrated GPU differential run executes 1,757 synthetic shaders with zero translation or compilation failures, 337,088 invocations and 7,506,659 compared components. It reports 2,319 mismatches in 84 shaders: texture-fetch 63, random 20, fuzz 1.

Compared the ordered mismatch/result lines against `cleanup-beta6/out/task103/shader-control-01.log`: **279/279 lines match exactly**, including values and invocation locations. `out/task105/shader-baseline-comparison.json` records this comparison. REPORT-103 establishes that control as released Beta 5 with only the two Windows test-startup corrections.

No comparison, tolerance, seed or shader semantics was changed or disabled to obtain a passing gate. REPORT-103 identifies a fetched-subnormal reference-model cause for one group, but does not classify every mismatch. Resolving that existing numerical gate requires separate shader/reference-model work and Claude/owner review; this integration does not report it as passing.

## Private shader preparation

Ran the exported setup/Play service without a game window under the shared GPU lock. Fresh preparation passes in 295.09 seconds. Both SPIR-V and DXIL packs contain **17,924 entries**, including scale, motion and lighting variants and the additional XEX shader containers.

Each API processes all 6,828 supplied pipeline-list records in both layouts: 3,714 queued/finished jobs unscaled, 4,235 queued/finished scaled/motion, zero failed and zero waiting for shaders. Subsequent Play-settings and API-switch checks retain the ready caches. This validates private application packs/cache preparation, not a genuinely cold vendor-global driver cache.

Evidence: `out/task105/prepare-cache.log`, `prepared-cache.log`, `prepare-cache-exit.txt` (0), and `prepared-cache/`. Only private copies of these packs/caches were used for gameplay. Nothing was packaged or handed over to the player.

## Three game sessions, no more

All roots below are under `F:\KK-native-renderer\runs\`. Folder timestamps are preparation times. Exactly three test sessions started chapters; all slots are consumed. The benchmark session includes its launcher and windowless startup supervisor in addition to its game child.

| Session | Folder | Seconds | Owned PIDs, all exit 0 | Result |
|---|---|---:|---|---|
| Vulkan, chapter 27 | task105-vulkan-panel-20261015-124627 | 117.20 | 47296 | Controller 18/18; F8 opens/closes; captures reviewed |
| D3D12, About benchmark, chapter 8 | task105-d3d12-benchmark-20261015-125041 | 102.45 | 30692, 14672, 21520 | Real About action; replay complete; both result files saved |
| D3D12, chapter 8 | task105-d3d12-chapter-20261015-125507 | 48.52 | 43864 | Unobstructed native captures at 10/40 seconds reviewed |

Vulkan's existing controller harness checks the L3+R3 chord, held-chord behavior, navigation, input isolation, comparison, capture, closing and neutral release, and cycles all five languages. The existing UI-paint aid supplies F8/F7/F6 through the real listener: F8 opens at level second 94 and closes at 108, both handled. Reviewed `f8-panel.png`, `chapter-final.png` and `shot_100.png`; the panel fits and the chapter is rendered. That existing UI aid also temporarily switches 30 FPS/uncapped, DLSS/off and UI painting, exercises F2, and queues a local test toast. These are private test overrides, not default changes. No achievement is unlocked by that toast.

The D3D12 benchmark starts from the launcher's About page using its existing scripted navigation, not an initial command-line benchmark request. Reviewed `about.png` with Run benchmark selected and `results-final.png` after completion. Test mode suppresses update/download prompts with an automatic Play deadline far beyond the bounded session; About's normal action performs the launch. The host observer is available and the waitable/max-latency-one path is confirmed in the log. No startup API fallback occurred in any session.

Results are retained at:

`F:\KK-native-renderer\runs\task105-d3d12-benchmark-20261015-125041\userdata\benchmarks\run-1792065077973844\results.txt`

and `frames.csv` beside it. Status is complete. With the explicit 120 FPS game cap, game swaps average 119.713 FPS, 1% low 96.426, worst 83.821 ms. Successful host present returns average 109.877 FPS, consistent with the owner's existing global NVIDIA cap. That setting was not changed. These are separate series; host returns are not unique visible scanouts. This one capped functional run does not establish benchmark repeatability, maximum throughput, scanout latency or a pacing improvement.

The third session supplies clean D3D12 game pictures because the benchmark results panel covers much of the scene. Reviewed `shot_100.png` and `shot_400.png`: the scene is filled, with no obvious missing textures/geometry or new visible rendering defect in these samples. Both APIs' runs retain only the two known startup `ResolvePath() failed - device not found` errors documented by earlier reports; no renderer/device-loss error was found. Still captures and these short chapter slices do not certify absence of brief flicker or whole-game regressions.

### Run rules and evidence

Every launch used copied exe/DLLs/glyphs, both current shader packs, pipeline list and DXC; copied test userdata; private cache/config/log roots; worktree assets; both SDL activation hints at 0; `--fullscreen=false --window_width=1280 --window_height=720 --kk_fullscreen_mode=desktop`; and the existing kkbg2 PID 11936. Benchmark saves are additionally private inside its run's benchmark directory.

Waited for tasks 104/106 and other shared-slot owners; never replaced their priority files or stopped their processes. No user lock or user-owned running game was observed. An earlier D3D12 preparation runner was stopped while waiting, before it launched anything, to adjust its private launcher test mode; the unused preparation folder remains. Run counting is normalized for PowerShell's array wrappers and all three sessions are retained in `out/task105/game-runs.json`. Each launched folder's `test.json` independently records arguments, hashes, duration, owned PIDs and successful exits.

Only game frames and PrintWindow images of owned windows were captured. No desktop screenshot or focus operation, Documents/Desktop game/save/cache access, file deletion, download, system-component installation or Linux work occurred. All task-105 game processes have exited, its lock was released, and kkbg2 was left running. Other tasks' later lock ownership is left alone. No fourth task-105 game launch is permitted.

## Handoff and owner decisions

- **The all-CTest gate remains failed.** Claude/owner must decide how to handle the proven pre-existing shader differential failure before calling Beta 6's test gate green. No shipping approval is implied by the successful builds and smoke runs.
- Keep task 101's shadows, water reflections and shafts hidden. Their prior visual acceptance gaps remain. GI preset/default behavior, vendor upscaler defaults, Remastered/Original default caps, Vulkan-primary policy and native-only startup recovery are preserved.
- `opt-beta6` and paused `hfr-beta6` remain excluded. These smoke runs do not sign off arbitrary-frame-rate game behavior, physical-controller input, older/low-memory hardware or the outstanding per-branch acceptance matrices. Fullscreen/borderless owner testing remains separate from these windowed runs; no mode-handling source was changed here.

Source changes are the seven scoped merges plus this report. No game-derived files are committed. No push, merge to beta lines/main, package or release was performed. Stop here for Claude's verification.
