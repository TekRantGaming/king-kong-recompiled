# Task 116: owner-route replay and Beta 6 test 2 package

The requested replay runs and player package are complete, but **repeatability is not proven**. Every run finished all four chapters and saved results. Every run also reported route drift. The three corrected uncapped Vulkan runs exceeded the under-3% average-FPS spread target in every chapter. Kong has no valid position checkpoints in this recording. This package is for Claude and owner testing, not standardized benchmark scores. Eight game runs were used; no further game was launched.

## Scope and compatibility

Read central DECISIONS.md first, then TASK-116, GAME-RULES.md, REPORT-115.md and BENCH-ROUTE-RECORDED.txt. No applicable AGENTS.md was found. Worktree `F:\KK-native-renderer\wt\bench-rec`, branch `bench-rec`, entered at `ed1aa95`, a report-only commit after the task's stated `c341574`. The two pre-existing generated/manifest line-ending modifications were left out of commits. No task item directly conflicted with the standing decisions. The explicit standing prepared-cache deletion instruction took precedence over GAME-RULES' general no-delete rule for that cache only.

Owner route: `F:\KK-native-renderer\testbuilds\bench-routes\route-45s-1015-1609.kkbr`, 10,454,677 bytes. SHA-256: `4FDAF2B8E8CCDB25D5062DC8AA2734A7197AE9C6FE06043159B256B177527EFD`. The original Documents recording was not modified. The route was never committed.

The current C++ reader accepted this older 1602 recording unchanged: version 1, four 45-game-second chapters, 83,212 pad reads, 184 checkpoints, recording ID `00d2c1b614e7b8a6`. All 184 checkpoints have matching primary-pad timing metadata; derived frame-start times are ordered in all four chapters. Full live completion also confirms format compatibility with the final task115 reader and this replay correction.

## Replay correction

Code commit: `1ef5d6ec6a850103a841a32ef2a5ff059341c61f` (Align benchmark replay input and checkpoint times). It has the required Claude Opus 5.5 co-author trailer.

The recorder's frame hook adds the current delta before recording input timestamps and checkpoints, while the sampled position still belongs to the beginning of that simulation interval. Comparing those positions at the stamped interval end introduces a frame-dependent offset. Replay now uses the recorded interval start (`seconds - raw_dt`) and the corresponding live pre-step time for pad selection. Frame-one input is not supplied before the first frame hook. Version-1 files and their stored bytes remain unchanged.

Checkpoint diagnostics derive the same source position time and numerically sample between adjacent observed positions at that instant. This is read-only diagnostic arithmetic; it creates no rendered frames, writes no player positions and changes no simulation delta. The final swap supplies the ending observation, so corrected runs compare all 45 post-initial checkpoints in each Jack chapter. `drift.csv` now includes the effective `position_seconds`, recorded XYZ and replay XYZ. Unmatched timing remains explicitly unavailable.

This fixes replay/comparison timing and ending coverage. It does **not** fix the remaining live drift. No position snapping, artificial input queue, RNG/clock overwrite, AI throttling, physics change, frame interpolation or frame generation was introduced.

## Measurement conditions

AMD Ryzen 7 7800X3D, RTX 4070 Ti. All runs used 1280x720 windowed arguments, `fullscreen=false`, `kk_fullscreen_mode=desktop`, VSync off, SDL activation hints both 0, and the existing kkbg2 helper (PID 11936). The Native preset resolved to a 3x, 3840x2160 internal picture from the reported 1800p output selection. This was not a 720p internal-render test. Requested graphics stayed fixed: native upscaler off, High GI, Strong GTAO (3 samples), original shadows/water and volumetrics off. Recorded camera/FOV/HFR/cheat controls were applied locally. API and cap varied as labelled. The owner's recording used different graphics and fullscreen; its FPS is not compared with these runs.

All six corrected development runs used identical executable/native-DLL/runtime hashes. Every run used a fresh private user/result/cache location and the same two shader packs. No build ran during measured gameplay. Desktop activity and thermal/cache state were not held laboratory-constant. A later read-only GPU snapshot showed P0, 100% load and 2805 MHz; it does not establish the cause of the run-to-run variation.

Figures below are **game swap intervals**, with loading excluded and cap waits/in-chapter stalls included. 1% low is the reciprocal mean of the slowest ceil(N/100) intervals. Host-present-return timings are separately preserved in each result file; neither series is claimed as scanout timing. Independent CSV recomputation and SVG/XML checks passed for all eight runs. All values remain diagnostic because of drift.

## Required repeatability runs

| Run | Chapter | Average FPS | 1% low FPS | Worst frame ms |
|---|---|---:|---:|---:|
| Vulkan uncapped 1 | Necropolis (entry 2) | 129.979 | 57.181 | 125.876 |
| Vulkan uncapped 1 | V-Rex (entry 8) | 89.634 | 32.533 | 82.377 |
| Vulkan uncapped 1 | Kong (entry 10) | 67.778 | 28.774 | 86.243 |
| Vulkan uncapped 1 | Skull Island coast (entry 1) | 158.271 | 78.086 | 89.495 |
| Vulkan uncapped 2 | Necropolis (entry 2) | 128.255 | 56.932 | 114.652 |
| Vulkan uncapped 2 | V-Rex (entry 8) | 93.758 | 31.373 | 78.356 |
| Vulkan uncapped 2 | Kong (entry 10) | 94.133 | 37.132 | 117.326 |
| Vulkan uncapped 2 | Skull Island coast (entry 1) | 152.992 | 72.341 | 74.250 |
| Vulkan uncapped 3 | Necropolis (entry 2) | 134.669 | 54.670 | 120.503 |
| Vulkan uncapped 3 | V-Rex (entry 8) | 97.307 | 39.173 | 110.608 |
| Vulkan uncapped 3 | Kong (entry 10) | 96.165 | 35.379 | 114.651 |
| Vulkan uncapped 3 | Skull Island coast (entry 1) | 151.212 | 62.688 | 132.817 |
| Vulkan cap 60 | Necropolis (entry 2) | 59.688 | 39.257 | 136.958 |
| Vulkan cap 60 | V-Rex (entry 8) | 57.668 | 29.572 | 77.357 |
| Vulkan cap 60 | Kong (entry 10) | 58.620 | 31.864 | 86.048 |
| Vulkan cap 60 | Skull Island coast (entry 1) | 58.299 | 39.472 | 60.106 |
| DX12 uncapped | Necropolis (entry 2) | 133.144 | 32.352 | 244.499 |
| DX12 uncapped | V-Rex (entry 8) | 108.139 | 38.048 | 125.328 |
| DX12 uncapped | Kong (entry 10) | 94.245 | 13.215 | 776.506 |
| DX12 uncapped | Skull Island coast (entry 1) | 171.223 | 73.162 | 136.153 |

Spread = (maximum - minimum) / arithmetic mean of the three corrected uncapped Vulkan averages. No run was discarded to improve the spread.

| Chapter | Min average FPS | Max average FPS | Spread | Under 3% |
|---|---:|---:|---:|---|
| Necropolis (entry 2) | 128.255 | 134.669 | 4.90% | No |
| V-Rex (entry 8) | 89.634 | 97.307 | 8.20% | No |
| Kong (entry 10) | 67.778 | 96.165 | 33.00% | No |
| Skull Island coast (entry 1) | 151.212 | 158.271 | 4.58% | No |

## Additional runs within the eight-run limit

| Run | Chapter | Average FPS | 1% low FPS | Worst frame ms |
|---|---|---:|---:|---:|
| Initial Vulkan uncapped, before fix | Necropolis (entry 2) | 139.784 | 71.795 | 107.973 |
| Initial Vulkan uncapped, before fix | V-Rex (entry 8) | 99.714 | 52.439 | 83.785 |
| Initial Vulkan uncapped, before fix | Kong (entry 10) | 98.887 | 48.698 | 90.729 |
| Initial Vulkan uncapped, before fix | Skull Island coast (entry 1) | 163.999 | 86.203 | 71.922 |
| Vulkan cap 120 diagnostic | Necropolis (entry 2) | 78.502 | 37.962 | 112.028 |
| Vulkan cap 120 diagnostic | V-Rex (entry 8) | 94.581 | 28.717 | 83.794 |
| Vulkan cap 120 diagnostic | Kong (entry 10) | 94.633 | 40.360 | 102.093 |
| Vulkan cap 120 diagnostic | Skull Island coast (entry 1) | 72.598 | 39.598 | 79.258 |
| Packaged player Vulkan uncapped | Necropolis (entry 2) | 113.185 | 45.011 | 104.572 |
| Packaged player Vulkan uncapped | V-Rex (entry 8) | 66.527 | 28.148 | 104.904 |
| Packaged player Vulkan uncapped | Kong (entry 10) | 65.482 | 25.069 | 103.357 |
| Packaged player Vulkan uncapped | Skull Island coast (entry 1) | 108.097 | 46.217 | 117.935 |

The initial run exposed drift before the timing correction. The 120-cap run shows that matching the recording's requested cap does not remove drift under these test conditions. The final run used byte-identical copies of the packaged production executable and DLLs, with `KK_DEV_TOOLS=OFF`, and exercised default `benchmark-route.kkbr` lookup and all chapter transitions. Its performance is reported separately from development repeatability.

## Drift results and unresolved causes

Each cell below is **maximum position distance; maximum checkpoint lag; mismatched/compared checkpoints**. Position tolerance remains 1 guest unit; checkpoint-lag tolerance remains 100 ms. The initial row uses the old timestamp comparison; corrected rows use `position_seconds`.

| Run | Necropolis | V-Rex | Kong | Coast |
|---|---|---|---|---|
| Initial Vulkan uncapped, before fix | 51.400; 10.712 ms; 7/44 | 29.526; 11.792 ms; 19/45 | N/A; 10.784 ms; 0 comparisons | 10.443; 6.701 ms; 24/44 |
| Vulkan uncapped 1 | 84.272; 10.162 ms; 6/45 | 66.075; 15.480 ms; 19/45 | N/A; 24.086 ms; 0 comparisons | 11.075; 7.310 ms; 24/45 |
| Vulkan uncapped 2 | 35.739; 8.984 ms; 5/45 | 36.186; 18.359 ms; 18/45 | N/A; 11.656 ms; 0 comparisons | 23.198; 8.124 ms; 28/45 |
| Vulkan uncapped 3 | 52.159; 9.372 ms; 5/45 | 33.135; 17.190 ms; 19/45 | N/A; 11.736 ms; 0 comparisons | 9.110; 37.363 ms; 28/45 |
| Vulkan cap 60 | 46.107; 15.262 ms; 14/45 | 42.812; 28.361 ms; 18/45 | N/A; 34.483 ms; 0 comparisons | 40.171; 20.210 ms; 29/45 |
| DX12 uncapped | 91.158; 9.888 ms; 4/45 | 38.977; 12.083 ms; 18/45 | N/A; 15.288 ms; 0 comparisons | 10.352; 7.738 ms; 27/45 |
| Vulkan cap 120 diagnostic | 49.757; 16.636 ms; 5/45 | 33.891; 19.378 ms; 14/45 | N/A; 13.558 ms; 0 comparisons | 24.222; 21.708 ms; 26/45 |
| Packaged player Vulkan uncapped | 22.417; 20.216 ms; 8/45 | 17.603; 21.480 ms; 19/45 | N/A; 52.041 ms; 0 comparisons | 33.479; 17.690 ms; 28/45 |

All corrected Jack chapters had 45 comparisons and one excluded initial checkpoint. All 46 Kong checkpoints are invalid in the supplied file, so Kong position verification is unavailable. Across all eight runs, polls missing recorded input and polls skipping button/trigger transitions were both zero; maximum input age was 0.062966 seconds, below the 0.25-second stale-input threshold. All chapter checkpoint lags remained below 0.1 seconds. **Position tolerance failed in every verifiable chapter.**

Input coverage, chapter transitions and the frame-phase timing error were checked. Residual differences persist after the timing correction and at both 60 and 120 caps. For example, corrected Vulkan run 1's coastal checkpoint at effective time 16.9952 seconds differed by 11.075 units despite only 0.771 ms observation lag. This is not explained by a missed checkpoint poll alone.

The supplied route contains pad states and sparse Jack probe samples, not camera state, RNG state or actor snapshots. No confirmed game RNG seed API was found in the exposed hooks/docs. AI/RNG, collision outcomes, camera integration, frame-dependent simulation and startup/loading timing can still vary; these tests do not isolate their individual contributions or prove why all FPS variation occurs. The inherited 0x82D64250 probe's full positional interpretation remains incompletely validated, as REPORT-115 already notes. No visual/player-path certificate or claim that a player never gets stuck is made from this probe. Tolerances were not relaxed. The remaining all-four-chapter path proof and under-3% repeatability criteria are unmet at the eight-run limit.

## Player package

`F:\KK-native-renderer\testbuilds\beta6-test2-1015-1649\KingKong-beta6-test2-windows-x64.zip`

SHA-256: `DC4CE2C0A0E10A6217A4E3CD5962D2531C2C5A03CFB98119E9938597A40ED3C9`

Windows tar created this zip with `C:\Windows\System32\tar.exe -a -c -f <zip> KingKong`. PK signature and all 216 entries passed inspection. A fresh extraction passed all 209 file SHA-256 checks. `SHA256SUMS.txt`, `build-hashes.json` and the short `TEST-CHECKLIST.txt` are beside the zip; the checklist is also inside KingKong and asks the owner to run the benchmark twice and compare.

Contents are the 1617 package, overlaid with the production executable/native DLL/runtime, the unchanged owner route beside the executable as `benchmark-route.kkbr`, and the new checklist. Every other retained 1617 file was hash-compared. Recording HOW-TO.txt and Start Record Benchmark.cmd are omitted; originals are preserved in the 1617 package. Developer tools are compiled off and no recording action is exposed in the launcher. The `--kk_benchmark_record=true` command-line flag remains. About > Run benchmark still validates and launches the adjacent route through the existing callback; that wiring was reviewed in code, while live checks used the same default route lookup from the command line without foreground UI automation.

The package retains all original upscaler runtimes: DLSS, XeSS, FSR/AMD DX12 and Vulkan DLLs and DXC. Actual llvm-strings checks found `NVSDK_NGX` and `xess` in the packaged native DLL. Their visual quality/performance was not retested here because these replay measurements used upscaling off. No Classic plugin, game assets, recording shortcut or prepared native shader cache is included. The inherited executable version string remains `2.0.0-beta.5`; Beta 6 test 2 is the requested test-package name.

Production SHA-256 values:

- king_kong.exe: `0A71977BF9BE17707B3D7971E210D0DAF11E7349DD43591A24269779A5CBA1CD`
- rexgpu-native.dll: `6FC6F3BDF4275AB7ADDB0AE52932567ADB6B9CBCFC56EC8EC4229623615A24B1`
- rexruntime.dll: `9F2E78C4153362E197AE16BFAFECC3E9ECDA11CCC03460D2FB916C461772B2FD`

## Verification and game-rule handover

Development build PASS; production Release build with `KK_DEV_TOOLS=OFF` PASS; final CTest **10/10 PASS**, 2.49 seconds. New regressions cover pre-step selection at arbitrary/variable frame rates, first-frame gating, version-1 checkpoint timing, unavailable timing and numerical position comparison. Existing format, writer, startup, settings, panel and game-slot tests passed. PowerShell syntax checks and git diff --check passed.

The existing mkwt-created worktree's asset/generated/tool/upscaler links point at permanent locations. Both configured builds retain permanent SDK 041 in `CMAKE_PREFIX_PATH` and `NR_FFX_SDK_DIR`, plus the existing DXC runtime path. The older SDK 038 junction and SDK installations were not edited. The production build reused its existing `out/task115/beta-build` configuration; logs for this rebuild are under out/task116.

Each launch acquired the shared game lock with priority/owner-process checks, copied the game into its own runs directory, copied the permitted private test userdata, used private roots/logs/configs, and terminated only its recorded owned PID after results. No owner's game was running when launches were taken. No foreground/focus action, fullscreen/borderless test, desktop screenshot or own-window capture was used. Every game run was approximately three minutes plus loading, below ten minutes. The lock is free and kkbg2 remains running.

The standing handover cache deletion completed at `2026-10-15T16:55:12.3933140+01:00` with no king_kong.exe running and the game slot guarded: only `C:\Users\TekRa\Documents\king_kong\cache\native-shaders` was deleted (4 files, 445,557,447 bytes). Before removal, 78,415 project directories and 537 directory links were checked for cache references; none referenced it. The cache and children were not reparse points, each file had exactly one hard link, and relevant scripts/mkwt were checked. Package/extracted/task116 test-user folders contain no prepared native-shaders cache. Saves, the original owner recording and the owner's game copy were untouched. No other files were deleted.

## Evidence index

| Label | Run folder under F:\KK-native-renderer\runs | PID | Result subfolder under userdata\benchmarks |
|---|---|---:|---|
| vk-1 | `task116-vk-1-20261015-162222` | 37996 | `run-1792077743461529` |
| vk-fixed-1 | `task116-vk-fixed-1-20261015-162839` | 8992 | `run-1792078120714329` |
| vk-fixed-2 | `task116-vk-fixed-2-20261015-163215` | 28068 | `run-1792078336116792` |
| vk-fixed-3 | `task116-vk-fixed-3-20261015-163531` | 39192 | `run-1792078532117278` |
| vk-60 | `task116-vk-60-20261015-163850` | 47428 | `run-1792078731949535` |
| dx12 | `task116-dx12-20261015-164222` | 21976 | `run-1792078943536788` |
| vk-120-diagnostic | `task116-vk-120-diagnostic-20261015-164607` | 45904 | `run-1792079168579029` |
| player-smoke | `task116-player-smoke-20261015-165026` | 23896 | `run-1792079427729766` |

Each result folder contains results.txt, frames.csv, drift.csv and frame-times.svg; arguments, component hashes and test-result.json are in its run folder. Task-owned evidence is in `F:\KK-native-renderer\wt\bench-rec\out\task116\`: build-dev-tests.log, build-player.log, final-ctest.log, owner-route-compatibility.log, owner-route-timing.jsonl, runs-analysis.json, package-audit.json, archive-verification.log, cache-cleanup.json, project-directory-links.json and verification.json. Run ledger: `F:\KK-native-renderer\runs\task116-launches.txt`.

No AI/game-logic timing changes, Linux work, SDK installation, game-derived data commit, push, merge, beta-line merge or release. Stop for Claude's verification.
