# REPORT-069: UI paint on demand

15 October 2026. Worktree `F:\KK-native-renderer\wt\codex-uipaint`, branch `codex-uipaint`, base `43f69d6`.

## Disposition

Implemented and built the isolated SDK/game change. The no-game presenter tests pass and the completed DirectX 12 capped sample demonstrates removal of the monitor-rate repaint loop. **Runtime validation is incomplete: the Vulkan leg was aborted by a runner error, and the real pause/F8/overlay-clearing checks did not pass verification. This is not a shipping approval or a fully validated Beta 4 change.** The owner still decides Beta 4 versus Beta 5.

Read `DECISIONS.md`, `codex/GAME-RULES.md`, and REPORT-067 section A before implementation. No standing-decision conflict required skipping code. Kept Vulkan primary, Remastered's 60 FPS/Strong GTAO and Original's 30 FPS defaults, lighting defaults, guest timing/AI, and presentation settings unchanged. No interpolation, frame generation, added presentation queue, or AI throttling.

## Implementation

- New patch `native-renderer/sdk-patches/0037-ui-paint-on-demand.patch` adds `ImGuiDialog::NeedsContinuousPaint()`. Its default is true, preserving ordinary SDK dialogs and the launcher. The drawer queries surviving dialogs after drawing, rather than treating registration as visibility.
- The game's persistent pause backdrop/menu, F8 panel, FPS/cover overlay and toast opt out while inactive. Launcher/settings/console retain continuous drawing; pause/F8, cover and queued toast animations continue ticking for their active lifetime. Static F2 text uses new guest output and an explicit wake when its statistics snapshot changes.
- Window input requests painting after propagation, including keys consumed by a higher-priority listener. Dialog add/removal and game-side cover, pause, toast, F2 and F8 state changes request paints. Overlay close/removal retains a clearing paint.
- Guest-thread state wakes marshal through the existing app UI queue. A mutex protects the installed callback, which is cleared at app destruction.
- `ui_paint_on_demand` defaults true. Setting it false restores the old drawer self-request loop for this test, using the same executable/runtime. This avoids comparing ABI-incompatible SDK DLLs. `ui_paint_trace` defaults false and adds successful host-present counts only when explicitly enabled.
- The development-only `KK_DEV_UIPAINT_TEST` sequence changes only test settings, logs phase boundaries and guest-swap counts, and exercises the planned comparison phases. It is absent from normal builds.

Mailbox ownership/latest-frame coalescing, forced guest tick wakeup, Vulkan present-mode selection, DXGI flags, VSync/tearing and GPU synchronization are unchanged by 0037. Patch 0035 remains the existing per-image semaphore fix.

## SDK isolation and builds

Original SDK source: `F:\KK-native-renderer\sdk-src-upscalers`, HEAD `5f4fa22cd9c4741c8966f51b655802a28fd9c387`. Copied it, including local submodule content/metadata, to `F:\KK-native-renderer\sdk-src-upscalers-037`. Applied 0035 there. Copied the existing FidelityFX dependency tree into the new bundle and reversed/reapplied 0036 there because that copied header already contained its format fix. Applied 0037 to the copied ReXGlue source. A reverse/check/apply roundtrip of 0037 reproduced the four compiled source files byte for byte.

Built Release ReXGlue with both Vulkan and D3D12 and installed to `F:\KK-native-renderer\sdk-vulkan-upscalers-037\win-amd64`. Repointed **only this worktree's** `tools\rexglue-vulkan` junction to the new permanent bundle. Neither `sdk-vulkan-upscalers` nor `sdk-vulkan-upscalers-035` was edited. The previous junction is preserved at `F:\KK-native-renderer\moved-aside\task069-sdk-junction`.

The copy's glslang CMake commands add `-I` to the required Python312 interpreter invocation. No bare Python command was launched. The SDK installer automatically registered its new prefix in the CMake user registry; removed only that task-specific registration afterwards, leaving previous entries intact. Obsolete copied 0035 build artifacts, including a FidelityFX build cache pointing at 0035, were moved to `moved-aside\task069-copied-035-build-artifacts` instead of being used or deleted. The new bundle contains `win-amd64` and `fidelityfx`.

`codex/build-sdk-069.ps1` rebuilds the prepared isolated source. `codex/build-tests-game-069.ps1 -Target tests` builds/runs the no-game tests; `-Target game` configures/builds `kk-dev-vulkan`. Both builds passed. Existing compiler warnings remain. Standard codegen touched the manifest/generated CMake file's line endings without a semantic diff; restored those incidental changes. No generated game code/data is in the commits.

FidelityFX/vendor DLLs were reused from the existing bundle, not newly rebuilt or certified by this UI task. The ReXGlue FidelityFX runtime option remains OFF, matching the original source build configuration; the native renderer still builds its upscaler integrations. `strings rexgpu-native.dll` found 37 `NVSDK_NGX` and 40 `xess` matches. DLSS, XeSS, AMD Vulkan and AMD DirectX DLLs are present in the test copy. Upscalers were OFF for the comparison and were not exercised here. No player package/archive was produced or handed over.

Tested binaries (SHA-256, both game launches):

| File | SHA-256 |
|---|---|
| king_kong.exe | 74666AEFB8A76FA076FD0B0C731293044575EC962D29C137AB5CCECEB9360716 |
| rexruntime.dll | AF892591CB1340091516CC5F6C255D7A0DD66AFC0F49E5E47A2E636FEB72C938 |
| rexgpu-native.dll | 8CE49DE50BAF270621F6A50BC895A3859A61BA874EFEC8A8A0B9904F7A2B534C |

## No-game tests

`native-renderer/tests/uipaint-069` exercises the actual shared Presenter, Window input propagation, mailbox refresh, ImGuiDrawer and ImGuiDialog with a fake surface/immediate backend. A never-shown, nonactivating HWND supplies the monitor membership required by Presenter. No game image, GPU rendering or saves are used.

All 16 checks passed:

- Registered idle dialog stops requesting paints.
- New guest output wakes and paints promptly once, within 100 ms.
- F2/F8/pause-style overlay open input wakes even when consumed before ImGui.
- Static F2 does not drive monitor refresh.
- Each overlay close paints empty geometry and becomes idle.
- Animated visible UI continues with guest output stopped.
- Expose forces a quiet-frame repaint; resize reconnects/paints.
- An outdated surface schedules recovery, repaints and returns idle.

Running `ui-paint-tests.exe --legacy` fails the first idle-dialog check as expected, proving the test detects the original loop. Logs: `F:\KK-native-renderer\runs\task069-tests.log`. Early harness failures were corrected: Presenter feature-dependent layout must match the SDK's private NIS/FidelityFX definitions, logging must be initialized, and monitor membership requires the hidden HWND. These were harness setup problems, not passing results.

The tests use representative dialog state, not the actual game's menu implementations. Surface recovery is simulated; physical GPU/device loss is not tested.

## Game runs and measurements

Waited for other tasks' lock ownership and checked priority/processes before each launch. Exactly one Vulkan process and one DirectX 12 process were launched by this task, sequentially. Each used a private executable/DLL/data copy under `runs`, private saves/cache/logs, activation hints 0, windowed 1280x720, `kk_fullscreen_mode=desktop`, and the already-running kkbg2 helper. Only our own process PID was stopped. No owner game, Documents saves/cache or Desktop copy was touched. The lock was released after each process ended. No fullscreen/borderless test or desktop screenshot was taken.

Vulkan: `F:\KK-native-renderer\runs\task069-20261015-014529\vulkan`. The runner's original `ReadAllText` call hit a live log-file sharing error during startup. Its `finally` stopped our PID and released the lock. There is one startup host sample (122 presents in 1.002 s), **no gameplay before/after counts, no guest FPS sample and no UI captures**. That startup sample is not performance evidence. Fixed the reader to use shared read/write/delete access with transient rotation handling before DirectX 12. No Vulkan retry was launched, respecting the single-run limit.

DirectX 12: `F:\KK-native-renderer\runs\task069-20261015-015007\d3d12`. NVIDIA GeForce RTX 4070 Ti, chapter 8, stationary input, 1280x720, Original look, AO/upscaler OFF, existing DXIL shader pack, async pipelines OFF. Process wall time 78.3 s, scripted level time 72.2 s; the runner then closed it. Captures show the same viewpoint. No GPU-loss/device-removed error was found; the log contains two startup filesystem device-resolution errors and missing optional asset warnings, which this task does not certify or fix.

Phase timing: legacy capped 0-12 s; on-demand capped 12-24 s; legacy uncapped 24-36 s; on-demand uncapped 36-48 s; F2 requested on 48-52 s; pause requested at 54/58 s; F8 messages at 60/66 s. Initial/transition intervals are excluded below. `codex/summarize-069.py` reproduces the samples. Host and guest diagnostic windows are independent, so their durations/counts must not be compared as identical time intervals. Rates are count divided by the respective duration. Counts are successful SDK host presents versus guest swap calls, not visible scanouts or unique image counts.

| DirectX 12 phase (level-time selection) | Host presents / sampled seconds | Host presents/s | Guest swaps / sampled seconds | Guest FPS |
|---|---:|---:|---:|---:|
| Before, cap 30 (2-10 s) | 882 / 6.030 | 146.3 | 207 / 7.150 | 29.0 |
| After, cap 30 (14-22 s) | 214 / 7.132 | 30.0 | 215 / 7.165 | 30.0 |
| Before, uncapped (26-34 s) | 1486 / 7.007 | 212.1 | 1220 / 7.052 | 173.0 |
| After, uncapped (38-46 s) | 993 / 7.035 | 141.2 | 1115 / 7.041 | 158.4 |

The capped sample demonstrates about 79.5% fewer successful host presents/s, with host rate tracking the 30 FPS guest instead of the monitor-driven repeat loop. **No uncapped FPS gain is established.** The uncapped guest average was lower after the switch, with severe within-phase variation (before samples range roughly 34-278 FPS; after roughly 42-251 FPS), fresh pipeline-cache/warmup effects and an evolving game simulation. One sequential sample cannot isolate a scheduling effect on guest throughput. Host rates below guest swaps can occur with the unchanged latest-frame mailbox/coalescing and independently timed samples.

## UI verification and captures

Nine own-window PrintWindow PNGs were saved at 10, 22, 34, 46, 50, 56, 59, 63 and 68 seconds. They are in `F:\KK-native-renderer\documentary\uipaint\` and the worktree's `documentary\uipaint\`; all are uncommitted. The game's own guest-frame captures are also retained in the private run directory. Neither capture method took a desktop screenshot or activated the window.

**Actual game overlay verification is incomplete.** The F2 phase changed its cvar, but the reviewed PrintWindow capture does not show the counter. The pause script logged `open (menu open false, available false)` and `Resume is not available now`; no pause-open transition occurred. Static inspection afterwards found that the existing developer pause step parser documents `open` but does not implement it. F8 messages were queued to our HWND, but there are no panel-open/close log entries. Queued-message booleans in result.json do not prove that the UI received the keys. The images therefore do not prove open/close clearing or absence of stale overlays. These failures cannot be promoted to passed checks merely because the no-game representative overlays passed. No extra game launch was made to repair the incomplete runtime checks.

## Risks and handoff

- Vulkan gameplay counts/FPS/UI checks remain unmeasured. This is the primary API, so DirectX 12 evidence alone is insufficient for shipping.
- Real F2, F8 and pause open/close clearing remain unverified. A corrected background-safe input/test aid and a separately authorized run are needed. Do not infer success from the current screenshots or result booleans.
- Launcher/settings/console and actual toast/cover animation are preserved by policy/source inspection, but were not independently exercised with guest frames stopped. GPU loss was simulated only.
- Adding a virtual policy changes the ImGuiDialog ABI. Rebuild the game and SDK together; do not mix this executable with the older 035 runtime. Future persistent hidden dialogs must override the policy and explicitly wake on external state changes.
- This change does not deduplicate repeated native guest-image copies or identify identical guest images. It removes the independent idle UI loop only.
- New local SDK source/install folders and this worktree's junction are retained for Claude's verification. Global package discovery was restored. No push, merge, release, default-setting change or owner test-package handoff occurred.

Implementation commit: `1836bb4` (required Claude co-author footer). Follow-up diagnostics/runner/report commit is the next commit on this branch, also with the required footer. Game-derived captures, packs, logs and saves are excluded. The unified patch's blank context lines intentionally contain the diff prefix; whitespace checking with `core.whitespace=-blank-at-eol` passes, and source/script whitespace checks pass.
