# Task 076: integrate test build 3

15 October 2026. Worktree `F:\KK-native-renderer\wt\beta4-tb3`, branch `beta4-tb3`, base `43f69d6`.

## Result

Integration and offline verification pass. Game verification is partial: the actual beta executable starts V-Rex on Vulkan and DirectX 12 without Xenos, with DLSS Quality and uncapped effects-off measurements. The requested live GI Medium and Remastered/60 phases and launcher Graphics-page image were NOT obtained. Background window messages did not deliver the required keyboard actions. No foreground activation, fake focus event, developer-enabled beta, or extra campaign was used to work around this. This is a review candidate for Claude, not complete runtime acceptance or shipping approval.

Read DECISIONS.md first, then TASK-076, GAME-RULES.md and REPORT-066/068/069/071/073/074/075. No integration requirement conflicted with standing decisions. The prohibition on taking focus was respected when UI automation failed; those runtime checks remain outstanding. Linux work remains excluded. No tessellation branch was merged.

## Integration

Repointed only this worktree's `tools/rexglue-vulkan` junction from the permanent 035 bundle to `F:\KK-native-renderer\sdk-vulkan-upscalers-037`. The old junction is preserved at `F:\KK-native-renderer\runs\task076-sdk-junction-035`. No shared SDK source or binary was edited. Asset, generated-code and DLSS/FSR/XeSS links were inspected and resolve to permanent locations.

Merged in the required order, each with `--no-ff` and the required Claude co-author trailer:

| Merge | Branch tip incorporated | Merge commit |
|---|---|---|
| codex-native-only | 21f71ed | ecb7967 |
| codex-quickwins | 487b857 | f8a26ee |
| codex-gi2 | 1f94532 | b22e434 |
| codex-uipaint | bbac191 | f19a45f |

All merges completed automatically without conflicts. Reviewed the overlapping application, plugin and UI paths. Native-only selection/recovery/staging survived; no Classic choice or KK_NATIVE_RENDERER branch returned in the launcher/debug/build files. Compatibility parsing and historical mentions remain as intended by REPORT-074. The Linux packaging limitation documented there is inherited and was not expanded into Linux work.

Version remains `2.0.0-beta.4`. SDK 037 supplies `ui_paint_on_demand=true`; `native_diagnostics=false` and `native_gi=off` remain the defaults. Source retains Original at 30 and Remastered at 60 with Strong GTAO (strength 3), including setup and look switching. Other new lighting defaults remain original/off. No AI pacing, interpolation, frame generation, upscaler defaults or other feature changes were added during integration.

## Builds and tests

- `native-renderer/tests/check-native-only-074.bat`: exit 0. Configured/built kk-dev-vulkan with NR_BUILD_TESTS=ON, ran all 24 registered CTest suites, then configured/built kk-beta. CTest: 24/24 pass, 75.54 seconds. Includes all 17 GI tests and the modified diagnostic draw checks. Existing compiler deprecation warnings remain.
- Beta built with `KK_DEV_TOOLS=OFF`, `NR_BUILD_TESTS=OFF`. CTest was also invoked on beta; it registers no suites, so this is not counted as another test pass.
- Built uipaint-069 against SDK 037 in `out/task076/uipaint`; all 16 representative presenter checks pass. This covers idle, guest wake, handled input, clearing, animation with guest output stopped, expose, resize and simulated surface recovery. It does not replace actual-game UI checks.
- `strings.exe` on each native DLL: 37 NVSDK_NGX matches and 40 xess matches. Both builds configure FSR/DLSS/XeSS enabled. Beta contains nvngx_dlss.dll, libxess.dll, amd_fidelityfx_vk.dll, amd_fidelityfx_dx12.dll, amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll, dxcompiler.dll, rexruntime.dll and rexgpu-native.dll. No rexgpu-xenos.dll is present in beta or any private run copy.
- Both executable/runtime pairings use SDK 037. The runtime SHA-256 is AF892591CB1340091516CC5F6C255D7A0DD66AFC0F49E5E47A2E636FEB72C938, matching the UI branch's ABI pairing. Private-run test.json files record full executable/native/runtime hashes and launch arguments.
- Build-generated line-ending-only changes in the manifest and generated rexglue.cmake were restored. Script parsing and `git diff --check` pass. No game-derived data is staged or committed.

Logs: `out/task076/build-and-ctest.log`, `beta-ctest.log`, `uipaint-configure.log`, `uipaint-build.log`, `uipaint-tests.log`. Developer CTest detail: `kk/out/build/kk-dev-vulkan/Testing/Temporary/LastTest.log`.

## Clean offline quick-wins benchmark

Baseline source is the existing read-only beta4-next worktree, verified at `43f69d68e16ebd02c652c0d04f57c66dd180c183`. Its standalone nr_game_upscale_tests was built into THIS worktree's `out/task076/baseline`, without editing the baseline worktree. Both sides use Release Clang, the same local DXC 1.9 and SDK 037/upscaler dependencies. After uses the integrated developer-build test executable. NR_BENCH=1 and NR_DIAGNOSTICS=0; only Bench_CpuPerDraw selected.

Three complete alternating pairs, before1/after1/before2/after2/before3/after3. Each case measures 400 frames of 801 synthetic draws. All six accepted executions end with one test and zero failed checks. Checked all running processes before each execution and every 500 ms during it; no game or build overlapped any accepted execution. Per-run process inventories are retained. No game test ran concurrently with the benchmark.

| Case | Before repeats, us/draw | After repeats, us/draw | Before median | After median |
|---|---|---|---:|---:|
| No upscaler | 2.234, 2.248, 2.241 | 2.052, 2.022, 2.014 | 2.241 | 2.022 |
| DLSS Quality | 3.925, 3.681, 3.657 | 3.772, 3.431, 3.661 | 3.681 | 3.661 |
| No upscaler + live settings | 3.015, 2.528, 2.536 | 2.851, 2.740, 2.250 | 2.536 | 2.740 |
| DLSS Quality + live settings | 4.188, 4.071, 4.031 | 3.850, 3.851, 3.992 | 4.071 | 3.851 |
| DLSS Quality + live + GTAO | 4.920, 5.219, 4.848 | 4.545, 4.839, 5.053 | 4.920 | 4.839 |

Four medians decrease and one increases. This does not establish a uniform performance improvement or isolate every merged change. The direct renderer fixture exercises ApiTimer but not the SDK-specific CallTimer/ring integration; it is not game FPS. Desktop GPU load/clocks remain uncontrolled. RTX 4070 Ti; Khronos validation is not installed.

Runner issues were kept separate from accepted measurements. The first valid before run completed, then PowerShell returned a null ExitCode because the process handle had not been retained. Fixed handle retention and resumed with after1, keeping the valid before1 log. Three later partial before attempts were stopped immediately when another task started short builds (including codex-tess task072 test/tool builds). Their logs are preserved with `.attempt-*` suffixes and excluded. The third clean pair ran after waiting. No other task's process was stopped.

Evidence: `out/task076/bench-{before,after}-{1,2,3}.log`, matching stderr/process JSON files, runner/resume logs, contamination inventories, and `summary.json`. `codex/summarize-076.py` reproduces the medians.

## One scripted game campaign

`F:\KK-native-renderer\runs\task076-20261015-021504`. Exactly three sequential beta processes: launcher 17916 (122.9 seconds), Vulkan 23748 (44.0 seconds), DirectX 12 44476 (37.6 seconds), plus cleanup time. No further game campaign or retry was launched.

All launches checked shared lock/priority and the absence of any game before claiming the lock. kkbg2 PID 11936 was already running. Explicit SDL activation hints 0, fullscreen false, windowed 1280x720, desktop fullscreen mode; hidden process launch; no focus/foreground action. Each phase copied the beta exe/DLLs/glyphs, inherited SPIR-V/DXIL packs, official local DXC runtime, and a fresh private userdata copy. Private cache/user/log roots and this worktree's assets were explicit. Only the tracked private PIDs were closed. The final lock is free and no game process remains. Owner Desktop game, Documents saves and caches were untouched. No desktop screenshot or file deletion.

| Check | Result |
|---|---|
| Beta launcher starts without Xenos | Pass |
| DPI-aware own-window capture | Pass, complete 3232x1888 window bitmap at this desktop's DPI |
| Graphics page showing no Classic row | NOT VERIFIED: E navigation and mouse messages did not reach the requested page; captures remain Home |
| Vulkan V-Rex, uncapped, effects off | Pass; first presented frame, cap 0 and original/off effect logs |
| Vulkan DLSS Quality | Pass; 310.9.1 preset M, 853x480 -> 1280x720; recurring execution logs report failures 0 |
| Vulkan live GI Medium phase | NOT RUN: background F9 console command did not open the console, so no setting change was delivered |
| Remastered 60 cap phase | NOT RUN for the same input limitation; retained defaults verified by source/tests only |
| DirectX 12 starts a chapter | Pass; V-Rex first presented frame and reviewed own-window capture, DLSS Quality active |

The beta intentionally lacks KK_DEV_TOOLS, including the task075 direct-listener input harness. Tried own-PID PostMessage input without moving the pointer, setting keyboard focus or foregrounding. The captures do not prove input delivery and no setting-change success is claimed. Did not add test aids to the shipping beta or fake focus to bypass the game rules. The runner's phase.done files mean phase completion, not every acceptance check passed; their text identifies the missing checks. The checked-in runner now records this distinction explicitly in result.json notes.

Host/guest effects-off observations, independent windows 15-35 seconds after each first-presented-frame log:

| API | Host presents / seconds | Host presents/s | Guest FPS median (range), 19 samples |
|---|---|---:|---|
| Vulkan | 3327 / 19.093 | 174.25 | 178.4 (137.6-185.0) |
| DirectX 12 | 3854 / 19.050 | 202.31 | 204.1 (191.3-209.4) |

Counts are successful host presents, not unique images or scanouts. Guest values are medians of per-second swap-derived FPS logs; host rates are total count/total seconds, so these are not identical statistics or synchronized intervals. Vulkan still had pipeline/motion warmup in this short run. No causal API performance comparison or capped redraw result is claimed. There are no Medium/Remastered measurements.

Reviewed images: `launcher/launcher-initial.png`, `launcher/graphics.png`, `launcher/graphics-click.png`, `launcher/console-probe.png`, `vulkan/console-probe.png`, `d3d12/chapter.png`. They are full own-window PrintWindow captures with per-thread DPI awareness, never desktop captures. The misleading filename graphics.png still shows Home. Both APIs show the V-Rex chapter view. Logs contain the same two startup ResolvePath device-resolution errors noted in the earlier UI report; no device-loss/device-removed error was found. This task did not repair those filesystem errors.

## Handoff limits

Claude still needs to verify GI Medium and Remastered/60 in the integrated beta, obtain the Graphics-page capture, and review inherited GI visual/cost and actual UI animation/clearing limitations from REPORT-073/075. Native-only code checks and synthetic tests do not substitute for those images/runs.

No push, release, merge to main, package/archive, user-cache deletion, system installation, download, subagent work, or work on codex-tess. Only the four requested branch merges and task076 validation scripts/report were committed. Local build/test evidence remains uncommitted under out/ and runs/.
