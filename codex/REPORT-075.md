# REPORT-075: UI repaint verification follow-up

15 October 2026. Branch `codex-uipaint`, worktree `F:\KK-native-renderer\wt\codex-uipaint`.

## Result

**Partial pass; the complete requested verification does not pass.** Both APIs now have gameplay measurements, DLSS Quality evidence and proven input delivery. DirectX 12 has full-window overlay appearance/clearing evidence. Vulkan's F2 appearance remains unproven because the initial capture code clipped the window at this PC's DPI setting. Both permitted launches were used; no third launch was made. Short animation transitions are not comprehensively verified by these sparse captures. This is not shipping approval.

Read `DECISIONS.md`, `codex/GAME-RULES.md`, TASK-075 and REPORT-069 before work. No task part conflicted with standing decisions. Vulkan was first. No default settings, production timing, AI, renderer scheduling, SDK files/junctions, frame generation or interpolation changed. No push, merge, release or player package.

## Tooling changes and build

All new game test behavior is behind `KK_DEV_TOOLS`, or in `dev_tools.cpp` which CMake includes only with that option. `kk-beta` does not enable it and the option defaults OFF. Absence from beta was checked in source/presets; a new beta binary was not built.

- Implemented developer pause `open` with a 250 ms Start press through the existing developer XInput provider. It leaves the game's normal pause eligibility and takeover intact. `close` resumes only when the real pause menu is open. These steps use `KK_DEV_AUTOSKIP=1` and the level-load script clock.
- Added dev-only F2 and actual pause-panel visibility logs. The existing F8 visibility logs remain in place.
- Extended the existing graphics-panel developer `Key` harness: F8/F6/F7 call the same listener methods real window input reaches; unhandled key-down goes through the SDK bind registry, so F2 executes its registered callback. Logs record handled state. No raw window key messages, synthetic OS keys, or focus changes. This proves handler delivery, not the complete SDL OS-event route.
- Extended `KK_DEV_UIPAINT_TEST` to six 12-second phases: legacy/on-demand cap 30; legacy/on-demand uncapped; legacy/on-demand DLSS Quality uncapped. At 72 seconds restore cap 30/upscaler off for overlay checks. F2 74/78, pause 80/90, F8 94/108, F7 held 99-102, F6 104, real toast queue 112. The toast does not unlock or save an achievement. Pause input moves Down and Up while open.
- `codex/run-075.ps1` retains shared-access live-log reading and lock/priority/process checks. `codex/capture-075.ps1` is the DPI-aware own-PID capture recovery helper. `codex/summarize-075.py <run-dir>` reproduces measurements.

Built `kk-dev-vulkan` successfully with `codex/build-tests-game-069.ps1 -Target game`. Retained configure/build logs as `runs/task075-game-configure.log` and `runs/task075-game-build.log`. Existing compiler warnings remain. Restored generated manifest/CMake line-ending-only changes. Final PowerShell scripts parse successfully; `git diff --check` passes. No new presenter changes required rerunning task069's representative presenter tests.

## Runs and rules

Campaign: `F:\KK-native-renderer\runs\task075-20261015-015706`, subdirectories `vulkan` and `d3d12`. Exactly two sequential game processes, PIDs 1756 and 32220. Vulkan wall time 128.2 s, DirectX 12 127.0 s; each reached 124 seconds on the level clock and was then closed by the runner. Neither exited unexpectedly during the sampled loop. Final check found no game process and the lock says `free`.

Each ran from its private executable/DLL/glyph/shader/compiler copy, with private `userdata`, saves, cache and logs. Both activation hints were 0, arguments were windowed 1280x720, fullscreen false, desktop mode. The existing kkbg2 PID 11936 remained running. Checked free lock, absent competing priority and no games before claiming/launching each run. Only our PIDs were closed. Owner Desktop game and Documents saves/cache were untouched. No desktop capture, foreground activation, fullscreen or borderless test. No files were deleted.

RTX 4070 Ti; chapter 8, stationary, Original look, AO off, async pipelines/validation off. Same tested binaries for both APIs:

| File | SHA-256 |
|---|---|
| king_kong.exe | FDD8B5E5C1011C1B998172F2C36A34E988C9EC00FA9E65ADFB6F9BD3EA74A2EF |
| rexruntime.dll | AF892591CB1340091516CC5F6C255D7A0DD66AFC0F49E5E47A2E636FEB72C938 |
| rexgpu-native.dll | 8CE49DE50BAF270621F6A50BC895A3859A61BA874EFEC8A8A0B9904F7A2B534C |

No device-loss/device-removed error was found. Each log has two startup filesystem device-resolution errors, also seen in task069; these were not repaired or certified here.

## Pass/fail table

FAIL below includes insufficient verification, and does not imply a demonstrated renderer defect.

| Check | Vulkan | DirectX 12 | Evidence / limit |
|---|---|---|---|
| Gameplay legacy/on-demand, cap 30 and uncapped | PASS | PASS | All four phases measured below |
| Host-present versus guest-swap counts | PASS | PASS | Successful host presents and swap calls; independent windows |
| DLSS Quality uncapped pair | PASS | PASS | Actual DLSS 310.9.1 Quality preset M, 853x480 -> 1280x720; recurring execution logs report failures 0 |
| F2 handler toggles on/off | PASS | PASS | Callback and handled=true key-down logs; unhandled key-up is expected for a down-only bind |
| F2 visibly appears and fully clears | FAIL | PASS | Vulkan top-right area omitted by initial capture bug; D3D full images show on/off |
| Actual pause opens/closes | PASS | PASS | Game pause/takeover, actual panel transition and resume logs |
| Pause navigation visibly responds | FAIL | PASS | Vulkan only partial menu captured; D3D Resume -> Start Again -> Resume captured |
| Pause fully clears | PASS | PASS | Later full-window frames contain no pause menu/backdrop |
| F8 visibly opens and fully clears | PASS | PASS | Vulkan partial open capture, later full-window clear; D3D full open/close captures |
| F8 responds while open / F7 hold-release | PASS | PASS | Saved-settings readout appears then clears in captures; handled=true logs |
| F6 reaches handler and writes capture | PASS | PASS | Each private screenshots folder contains PNG + settings TXT |
| Toast appears and fully clears | PASS | PASS | Full-window toast and later clear images |
| Smooth pause/F8 animation throughout opening and while open | FAIL | FAIL | Active UI responds, but sparse samples cannot establish smooth subsecond transitions; no guest-stopped actual-game UI test |

F7 had zero differing trial settings, so this verifies input, readout transition and redraw, not switching a nonempty trial configuration. No actual rendering defect or stale overlay was demonstrated in the reviewed complete captures.

## Measurements

Selection windows exclude transitions: 2-10, 14-22, 26-34, 38-46, 50-58, 62-70 seconds since phase 0. Each interval must fit fully inside its selection window. Host and guest windows are independent. Counts are successful SDK host presents and guest swap calls, not unique images or visible scanouts.

| API / phase | Host count / seconds | Host / s | Guest count / seconds | Guest / s |
|---|---:|---:|---:|---:|
| vulkan / legacy-capped | 1052 / 7.016 | 149.9 | 213 / 7.099 | 30.0 |
| vulkan / on-demand-capped | 183 / 6.099 | 30.0 | 184 / 6.132 | 30.0 |
| vulkan / legacy-uncapped | 2095 / 7.000 | 299.3 | 1766 / 7.014 | 251.8 |
| vulkan / on-demand-uncapped | 1778 / 7.018 | 253.3 | 1778 / 7.017 | 253.4 |
| vulkan / legacy-dlss-quality-uncapped | 1959 / 7.017 | 279.2 | 1283 / 7.035 | 182.4 |
| vulkan / on-demand-dlss-quality-uncapped | 1306 / 7.016 | 186.1 | 1309 / 7.024 | 186.4 |
| d3d12 / legacy-capped | 1053 / 7.016 | 150.1 | 213 / 7.099 | 30.0 |
| d3d12 / on-demand-capped | 221 / 7.132 | 31.0 | 213 / 7.099 | 30.0 |
| d3d12 / legacy-uncapped | 1716 / 7.008 | 244.9 | 1909 / 7.010 | 272.3 |
| d3d12 / on-demand-uncapped | 1904 / 7.013 | 271.5 | 1895 / 7.012 | 270.3 |
| d3d12 / legacy-dlss-quality-uncapped | 1405 / 7.025 | 200.0 | 673 / 6.058 | 111.1 |
| d3d12 / on-demand-dlss-quality-uncapped | 1415 / 7.020 | 201.6 | 1408 / 7.020 | 200.6 |

Capped host rate falls about 80% on Vulkan and 79% on DirectX 12, with guest rate remaining 30. D3D's slightly-above-30 host rate includes capture/expose activity. No causal uncapped throughput gain is claimed: scene evolution, compilation/warmup, sequential phase order and capture overhead confound the comparison. In particular D3D's DLSS legacy sample contains a substantial throughput dip. Legacy host rate below guest rate can occur with existing latest-frame coalescing. The corrected capture helper ran roughly once per second through most D3D phases, and only at the end of Vulkan, so API throughput comparisons are not controlled benchmarks.

## Captures and remaining limits

Uncommitted captures: `F:\KK-native-renderer\wt\codex-uipaint\documentary\uipaint2\`. Guest-only shots and F6 captures remain in the private run folders. No game-derived data is committed.

The original task069 PrintWindow helper was not DPI-aware. Its GetWindowRect yielded a 1296x756 bitmap, while PrintWindow painted physical coordinates for a 2592x1512 window, clipping the right and bottom. This explains why the top-right F2 was absent from those captures; it is not evidence that F2 failed to draw. Added a per-thread DPI awareness context for complete own-window capture and restored the previous context afterwards. Discovered this after Vulkan's F2/pause phases. Started the separate recovery helper at Vulkan's toast, then during D3D's early measurement phases. The final runner incorporates this correction, but that edit was made after the already-running runner had compiled its original capture type. Its numbered images are therefore clipped in both runs; use `*-full-HHmmssfff.png` for full images. No rerun was made.

Reviewed evidence (filenames relative to uipaint2):

- D3D F2 on `d3d12-full-020036177.png`, off `d3d12-full-020038129.png`.
- D3D pause Resume `020041917`, Down `020043740`, Up `020046474`, clear `020050364` (all `d3d12-full-*.png`). Selection and prompt glyphs change, proving real menu response.
- D3D F8 open `020055197`, F7 held `020100869`, released `020102772`, closed `020109332` (same prefix/suffix). Panel text/enabled state changes, while scene/fire/rain continue.
- D3D toast `d3d12-full-020114318.png`, clear `d3d12-full-020119211.png`.
- Vulkan pause `vulkan-84.png` is partial. F8 `vulkan-96.png`, F7 `vulkan-100.png` and close `vulkan-109.png` show the sampled upper-left area. Complete later `vulkan-full-015911865.png` shows the whole scene free of F2/pause/F8/toast remnants.
- Vulkan toast `vulkan-full-015903971.png` and `vulkan-full-015906977.png`, then clear `vulkan-full-015911865.png`. Last capture during process shutdown is black and is not clearing evidence.

Remaining verification: Vulkan F2 visibility and full pause navigation imagery; sufficiently frequent animation captures for pause/F8 (and actual guest-stopped continuous UI if that is required for acceptance). The two-run limit is exhausted. Existing SDK ABI pairing requirement from REPORT-069 still applies. Claude should review this partial result before any use.
