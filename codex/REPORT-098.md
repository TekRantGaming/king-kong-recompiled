# Task 098: older Vulkan support on Beta 5

15 October 2026. Worktree `F:\KK-native-renderer\wt\vkold-beta6`, branch `vkold-beta6`, base released Beta 5 `e890ece`, permanent SDK link `sdk-vulkan-upscalers-038`.

The reduced bindless path from `codex-hw-vulkan` through `0a27f59` is ported and locally committed. Both builds pass, all 30 configured CTests pass, and minimum tables match full tables exactly in the synthetic pressure test. V-Rex passes its five standard golden scenes. Rescue fails the standard historical clock and passes both image scenes in a separately retained +7 second alignment diagnostic. This is not an unconditional golden-suite or older-hardware certification.

## Scope and standing decisions

Read DECISIONS, current GAME-RULES, tasks 098/037, REPORT-037 and REPORT-012. REPORT-037 describes an earlier stopped attempt; the specified branch actually contains the subsequent implementation and validation checkpoint. Read its two modified generated CMake/manifest files and their empty content diff; left that worktree untouched.

The old implementation's suggestion to use Classic was omitted because the 15 October standing decision removes Classic. Unsupported devices instead retain the driver-update/DirectX 12 advice. No other standing-decision conflict prevented the task. Vulkan remains primary; API choice does not depend on vendor or hardware tier. No game logic, AI, frame generation, defaults, SDK sources, or task 096/097 worktrees were changed. Production changes in `game_resources.cpp` are descriptor bookkeeping/rebinding, not upload optimisation.

## Supported limits and remaining feature requirements

Devices previously rejected only for descriptor capacity can now run if they meet every minimum below. The selector keeps full capacities where possible, limits the affected descriptor types first, then fits aggregate budgets. A low sampler limit no longer needlessly shrinks image/buffer tables.

| Requirement | Previous full tables | New configured minimum |
|---|---:|---:|
| Bound descriptor sets | 7 | 7 |
| Update-after-bind sampled images, both per-stage and per-set | 18,944 | 512 |
| Update-after-bind storage buffers, both per-stage and per-set | 16,384 | 256 |
| Update-after-bind samplers, both per-stage and per-set | 1,024 | 128 |
| Aggregate per-stage update-after-bind resources | 36,357 in task 037 | 902 |
| Update-after-bind descriptors in all pools | 36,352 | 896 |

Beta 5 can bind six constant buffers, b0-b5. Aggregate checks now reserve six, correcting task 037's five; full tables require 36,358 aggregate stage resources. Minimum table capacities in order 2D/3D/cube/array/samplers/buffers are `128/128/128/128/128/256`, including dummy slot zero. Forced small tables are `1024/128/128/128/128/1024`. Declared bindless slots fall from 36,352 to 896 at the floor; this is not a measured VRAM saving.

Vulkan 1.3, dynamic rendering, timeline semaphores, synchronization2 and all nine existing indexing features remain required: sampled-image/storage-buffer dynamic and nonuniform indexing, sampled-image/storage-buffer update-after-bind, update-unused-while-pending, partially-bound descriptors and runtime descriptor arrays. Missing features and limits below the floor still reject startup with diagnostics. There is no non-bindless path.

Only this PC's RTX 4070 Ti was tested. GTX 900/1000, RX 400/500, Intel UHD/Iris Xe and Steam Deck are candidates only when their driver exposes the required capabilities; none of those families is certified by this task.

## Lifetime handling and limitations

Inactive descriptor mappings can retire without deleting their texture/buffer data. Pressure reserves the next draw's required tables before generating indices, submits/completes earlier GPU work, reopens the command list and resets mappings. GPU-written resolve data survives. AO, Beta 5 lighting slots and volatile constants are rebound after a drain. Launcher prewarm receives the selected capacities.

Reduced tables disable temporal upscalers because deferred motion replay retains descriptor indices. The same guard applies to startup, prewarm and live settings; the player's saved choice is preserved and a warning is logged. DLSS/FSR/XeSS remain built in for the normal path, but reduced mode renders at native size. Retaining temporal replay under pressure remains work for a later task.

## Offline validation and cost

`native-renderer/tests/build-098.bat game` builds `kk-dev-vulkan`; `build-098.bat` builds the standalone renderer/tools/tests. Existing SDKs only, no downloads or installations. Build configuration reports FSR, DLSS and XeSS enabled.

- Renderer CTest: **27/27 pass**, 106.32 seconds. Includes normal, forced-small and forced-minimum rendering/AO/GI suites, capacity boundaries, per-type isolation, 256 mixed-budget combinations and live-upscaler rejection. The complete LastTest.log contains no Vulkan/NVRHI error lines.
- Game CTest: **3/3 pass**: hardware recommendation, launcher logic, startup recovery.
- Strict offscreen tests: exact equality of all readback pixels between full/minimum tables over 900 draws, three batches and six drains. Includes distinct views/samplers/buffers, retirement-window reuse and scale constants. A separate test verifies a GPU-only resolved image survives pressure with no CPU texture upload. Both pass with Khronos core, synchronization and GPU-assisted validation, zero error checks. The layer prints its expected warning about running core and GPU-assisted checks together.
- Two observations per mode of the existing 801-draw, 400-frame benchmark, first quarter excluded: full medians **4.269/4.175 ms**, small **4.226/4.235 ms**, minimum **4.269/4.226 ms**. Timed CPU recording/submission excludes the final GPU-idle wait. Core validation was enabled, driver caches warm, and the shared PC active. This supports little steady-state overhead at this precision, not a precise sub-percent speed claim or a worst-case drain/hardware guarantee. Pressure deliberately introduces GPU completion waits.

Logs: `kk/out/task098/ctest-complete.log`, `ctest-game-complete.log`, `pressure-resolve-strict.log`, `bench-readable.log`. Repeat alignment with the absolute Python 3.12 executable, `-I native-renderer/tests/vulkan-098-align.py <run-root>`.

## Three game launches, no more

Every launch used a private exe/DLL/glyph/shader/DXC/userdata copy under runs, explicit worktree assets, Vulkan, forced reduced tables, Original look at 30 FPS, both SDL activation hints zero, the running kkbg2 helper and the required 1280x720 windowed/desktop-mode arguments. The runner obeyed the shared lock and priority file, checked all game processes, released only its own lock and stopped only its own PID. No user game/saves, desktop capture or display mode was touched. Each run used Khronos core and synchronization validation plus NVRHI validation.

All roots below are under `F:\KK-native-renderer\runs\`:

| Run root | Mode, chapter | Process duration | Evidence |
|---|---|---:|---|
| `task098-golden-minimum-20261015-103927` | minimum, V-Rex 8 | 213.27 s | 21 captures, 4,690 drains; loading 3/3, V-Rex 4/5 + 4/5 + 5/5, pause 3/3; standard **5/5 scenes pass** |
| `task098-golden-minimum-20261015-104457` | minimum, Rescue 27 | 89.65 s | 8 captures, 213 drains; standard **0/2 scenes pass**, loading/cutscene late against historical clock |
| `task098-golden-small-20261015-105428` | small, Rescue 27 | 107.82 s | 61 captures in a declared 50-80 s window, zero drains; standard **0/2 scenes pass**; separate alignment **2/2 pass** |

The longer V-Rex run is needed by its existing 182-second golden script plus startup. No run exceeded four minutes. Every run has `test.json`, `result.json`, game/validation logs, captures and `report/report.html`.

All three runs have **zero Khronos validation errors**, no descriptor-exhaustion/device-removal error, and ten reported unused-fragment-output warnings each, limited by the layer's duplicate cap. They are not warning-free.

For the last Rescue run, one common offset was selected across both scenes by nearest-golden 160x90 MAE over the predeclared capture window. At **+7 seconds**, unchanged full-resolution thresholds pass cutscene **3/3** and gameplay **4/5**; one gameplay image still fails its two envelope thresholds. `alignment/summary.json` retains every candidate offset, the selected source images and full-resolution scores. Original failed reports and golden files remain unchanged. This supports a clock/timing mismatch but is a diagnostic fit, not an independent standard-clock pass. Draw-log comparison was unavailable in these collected scene reports. Reviewed the game's own V-Rex and Rescue frames; no desktop images were taken.

First-run DLL SHA256: `5563355E980994A35DC439520D7963EE0FFBE93448347AA7A333A97A751616B9`. Runs two/three use final DLL `EA57F3C6F6586052229B6422BC0DA536943EEE6503D0405AFCEFD239CF16513B`; the intervening production change only improves automatic capacity selection, leaving forced minimum capacities unchanged.

## Handover

Local commits: `07aab9b` port/lifetime guards; `dd37be0` capacity isolation; `69599e2` offscreen/strict/golden harnesses; `a60f840` retained Rescue timing diagnostics. Every commit has the required Claude co-author trailer.

Remaining: real older AMD/Intel/NVIDIA hardware and driver testing, optional-format coverage, temporal upscaling under reduced tables, a standard-clock Rescue regression/control, and performance under sustained minimum-table pressure without validation overhead. No claim of universal GPU support, all-chapter coverage or player-ready certification. No game-derived data committed, no deletion, push, merge, package, release or changes to beta lines. All three launch slots are consumed; no task 098 game or queued runner remains. Report mirrored to `F:\KK-native-renderer\codex\REPORT-098.md` for Claude.
