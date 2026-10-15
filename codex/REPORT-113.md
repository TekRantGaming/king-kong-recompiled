# Task 113: Windows Beta 6 test build 1, no release

15 October 2026. Worktree F:\KK-native-renderer\wt\beta6-tb1, branch beta6-tb1,
base db7a247. Production source commit: 414fdc466e8b2e8ef123113a49a689b453c1afa2.

Both game builds pass, all **70/70 primary CTests pass**, and the three short,
sequential game launches exited normally. The requested single Vulkan steady
pair is **246.69 FPS Beta 5 -> 245.08 candidate (-0.65%)**. An important limitation
is a **4.97-second candidate startup frame dominated by pipeline waits** in the
fresh-cache, launcher-bypassed test. This is explicit in the owner checklist.

This is a private test build for Claude's verification and owner testing.
No push, release, merge to a beta line/main, or follow-on work was performed.

## Decisions and scope

Read DECISIONS.md first, TASK-113, GAME-RULES.md, the memory kk-working-rules.md
handover checklist, and REPORT-105/108/110/107. No applicable AGENTS.md was found.

**One conflict was resolved in favour of the standing decision.** TASK-113 says
"Do NOT delete the owner's shader cache (Claude does it)." DECISIONS.md's mandatory
handover checklist requires deleting Documents\king_kong\cache\native-shaders
while the owner's game is closed. Skipped only the task's deferral instruction.
After all game runs, under the atomic slot guard with no game running, checked
directory aliases, reparse points and file hard links, then deleted that cache's
six files. The target is absent afterward. The package's
KingKong\user\cache\native-shaders was already absent. No owner saves or Desktop
game files were changed. Evidence: out/task113/shader-cache-cleanup.json.

## Integration and contents

- Inspected wt\opt-beta6 immediately before merging. Its current head was
  648ad4e480dfd3b22fa522ff856907269b65e955; imported that exact commit with --no-ff,
  producing 414fdc4. Did not merge hfr-beta6 or the concurrent task-112 follow-ups.
- Four conflicts retain both intents. The ignore file keeps existing test-output
  and generated-resource ignores. Backend CMake includes game_memory.cpp and
  game_preload.cpp. Renderer initialization/end-frame retain memory handling,
  resource profiling and upload flushing. The game fixture retains descriptor
  controls and upload submission, and both descriptor/preload test groups remain.
- Repointed only this worktree's tools\rexglue-vulkan junction to permanent
  F:\KK-native-renderer\sdk-vulkan-upscalers-041. Preserved the old junction at
  out\task113\rexglue-vulkan-038-link. Neither SDK nor another worktree was edited.
  Verified the mkwt-created asset, generated-code, SDK and upscaler links point
  at permanent locations.
- Version remains **2.0.0-beta.5**, checked in source and binary strings.
  Both outputs' runtime hashes match SDK 041. The packaged runtime contains
  RexSetPresentObserver096.
- Contents include the integrated cleanup, hardware/scheduling, older Vulkan,
  automatic memory adaptation, F8 controller and benchmark work, plus opt-beta6's
  CPU/upload/chapter preparation and shipped chapter hints.
- Vulkan remains primary, D3D12 secondary, native rendering only. HFR gameplay
  fixes are excluded. Unfinished shadows, water reflections and shafts remain
  hidden. Ann tessellation stays off by default and confined to F8. Task 113
  changes no graphics defaults, game timing or AI cadence.

The merge and conflict resolutions are this task's only production changes.
Two build-generated, line-ending-only rewrites of kk/generated/rexglue.cmake
and kk/king_kong_manifest.toml were checked, their single hard-link names
verified, and restored. Merge/report commits use the required Claude trailer.

## Builds and automated validation

Fresh Release builds pass for kk-dev-vulkan, kk-beta, and standalone native
renderer, resources, shaders, pacing and presenter projects.

| Suite | Result | Seconds |
|---|---:|---:|
| Development CTest | 9/9 pass | 2.53 |
| Native renderer CTest | 36/36 pass | 119.05 |
| Resources CTest | 2/2 pass | 3.09 |
| Shader CPU CTest | 22/22 pass | 265.63 |
| Shader GPU differential CTest | 1/1 pass | 115.66 |
| Primary total | **70/70 pass** | |
| Separate standalone pacing CTest | 3/3 pass | 1.88 |
| Direct presenter scheduling checks | 16/16 pass | |

No registered test was excluded. Beta and standalone presenter register zero
CTests; the latter's existing executable was run directly. One orchestration
attempt stopped on PowerShell's handling of the empty presenter CTest project's
stderr. Continued from that point, retaining passed CPU results. No registered
CTest failed.

The GPU differential run executes 1,761 shaders with zero translation/compilation
failures and **zero mismatches**. Existing fragile/unknown comparisons and four
nonterminating skipped rounds are retained. No tolerance, seed or comparison
was changed. GPU tests ran windowless on the RTX 4070 Ti with the validation
layer off; no Khronos-validation-clean or older-hardware certification is made.

Actual strings checks find NVSDK_NGX and xess in both native DLLs. Both outputs
and the package contain nvngx_dlss.dll, libxess.dll, amd_fidelityfx_vk.dll,
amd_fidelityfx_dx12.dll, amd_fidelityfx_loader_dx12.dll,
amd_fidelityfx_upscaler_dx12.dll and DXC. Neither output nor package contains
rexgpu-xenos.dll or another Xenos DLL.

Fresh private setup preparation passes in **347.044 seconds**. Both API packs
contain 17,924 entries with scale/motion/lighting variants. Each API processes
all 6,828 pipeline-list records in both layouts, with 3,714/4,235 jobs queued
and no missing-shader waits. Play-settings and API-switch checks remain current.
The first preparation wrapper incorrectly treated a normal native stderr
diagnostic as fatal, interrupting after the Vulkan pack was written. Preserved
that partial output and completed a fresh preparation in
out/task113/prepared-cache-complete using separate stdout/stderr files, exit 0.
Generated packs/caches remain private and are excluded from the ZIP.

Evidence root: F:\KK-native-renderer\wt\beta6-tb1\out\task113.
Build/configure/CTest logs, test-summary.json, test-exits.json,
binary-checks.json, presenter-checks.log, prepare-complete-stdout.log,
prepare-complete-stderr.log and worktree-links.json are retained there.

## One smoke/comparison session

Exactly three sequential launches cover both APIs and the one Vulkan pair.
No repeat pair or extra game was run.

| Case | Run folder under F:\KK-native-renderer\runs | PID | Seconds | Exit |
|---|---|---:|---:|---:|
| Released Beta 5 Vulkan | task113-baseline-vulkan-20261015-151722 | 36192 | 75.78 | 0 |
| Current kk-beta Vulkan | task113-current-vulkan-20261015-151840 | 23076 | 75.95 | 0 |
| Current kk-dev-vulkan D3D12/F8 | task113-current-d3d12-20261015-151958 | 23276 | 119.73 | 0 |

All three start chapter 8 and log the requested API. D3D12 logs SDK 041's
waitable swapchain and maximum frame latency 1. The controller harness passes
18/18, including panel opening/closing and input isolation. Keyboard F8 is
handled through the real listener at seconds 94 and 108; the handler toggles
the panel. No physical controller was tested. Reviewed native shot_1000.png
and shot_1120.png: the chapter is rendered without an obvious new defect in
these samples. These captures contain the game image, not an independent
visual capture of the F8 overlay.

Every launch used copied binaries/runtimes/glyphs, matching private shader
packs, shipped pipeline list, copied private test userdata, explicit private
cache/config/log roots and this worktree's assets. Both current runs load
42 shipped chapter keys and 237 hints for chapter 8. Both SDL activation hints
were 0. Arguments explicitly set 1280x720 windowed, fullscreen false and
kk_fullscreen_mode=desktop. Existing kkbg2 PID 11936 stayed running.
No focus operation, desktop capture, exclusive fullscreen or borderless test.

Waited for tasks 109, 112 and 111 and respected their locks/priorities. Used the
existing atomic guard and then a task-owned priority reservation. Every owned
game exited before the next launch and guard release. After cache cleanup, the
priority file was link-checked and moved to moved-aside. No user lock or
user-owned running game was observed. Only task-owned PIDs were closed.
Final audit: no game process, lock free, no task-113 priority, kkbg2 running.
PowerShell's nested JSON array wrappers were flattened for the final ledger;
the preserved raw ledger accounts for the same three PIDs. No fourth launch.

Each game log contains only the two known ResolvePath device-not-found errors
at error severity. No new renderer/device-loss error was logged. These short
runs do not certify whole-game rendering or timing.

### Requested uncapped Vulkan pair

Both use the full respective production executable/runtime/plugin set,
stationary chapter 8, 1280x720, render scale 1, uncapped, VSync off, upscaler off,
GI Low, full GTAO with three samples and strength 1.5, original shadows/water,
volumetrics off and the healing cheat. The owner's global cap was not changed.
FPS = 1000 / mean native interval at level seconds **30-65**, aligned using
the loaded log marker and saved UTC/QPC clock data.

| Metric | Released Beta 5 | Current build |
|---|---:|---:|
| Steady FPS | 246.68995 | 245.08200 |
| Sample frames | 8,635 | 8,577 |
| p99 interval, ms | 5.1578 | 5.6017 |
| Worst steady interval, ms | 6.9682 | 8.2023 |
| Steady skipped draws | 0 | 0 |
| First 10 s: intervals over 50 ms | 2 | 5 |
| Worst startup interval, ms | 203.8940 | **4967.2088** |
| Startup pipeline waits, ms | 0 | **7691.5211** |

Steady change: **-0.6518%** in one pair. No compiler/CTest/shader-tool overlap
was recorded in either measured window. Other desktop/coordinator activity is
retained in CPU samples. These are renderer intervals, not unique scanouts.
AI, GPU clocks and vendor-global caches were uncontrolled. This does not explain
or dismiss task 107's slower sample or establish a general improvement or
absence of performance regressions.

**Startup caveat:** both runs deliberately start with empty application
pipeline/resource caches, packs beside the executable and the launcher bypassed.
The separately prepared pipeline cache was not copied. The candidate's longest
startup frame logs 4,909.4 ms of pipeline wait inside its 4,967.2 ms interval.
D3D12 also has a 3,574.4 ms startup interval and 4,772.3 ms aggregate startup
pipeline waits in this configuration. This is a real observation for owner
attention, not a clean first-visit stutter result. The successful setup job does
not prove prepared-cache gameplay: no prepared-cache game run was added.
Check first entry and revisits after normal launcher preparation. D3D12's
UI harness changes caps/upscalers during its session, so no D3D12 performance
comparison is claimed.

Evidence: game-runs.json, game-runs-raw.json, game-summary.json,
analyse-games.py, and each run's logs, frames.csv, clock.json, arguments,
input hashes, empty-cache proof and CPU samples.

## Package and handover

Folder:
F:\KK-native-renderer\testbuilds\beta6-test-1015-1441

ZIP:
F:\KK-native-renderer\testbuilds\beta6-test-1015-1441\KingKong-beta6-test1-windows-x64.zip

SHA-256:
d7aa9143813d5761021de93422f9653ed31676808874422b0707fe6eadf9686f

Size: **147,031,922 bytes**. SHA sidecar is beside the ZIP.

Started from testbuilds\v2.0.0-beta.5-release\KingKong, preserving VC runtimes,
glyphs, DXC, notices and the merged 6,828-record kkpipelines.list. Overlaid
**every EXE/DLL from this kk-beta output**, verifying identical hashes.
Added task 107's frozen kkresources.list: 42 chapter keys, 6,167 descriptions,
222,548 bytes, SHA-256
108f3d2ec1ce394a2e464a0602333e9eb90a4254ea76da62ec629cf8ecb83b17.
New UI strings and renderer shaders are embedded; no extra loose feature asset.

Created with C:\Windows\System32\tar.exe -a -c -f
KingKong-beta6-test1-windows-x64.zip KingKong. Verified PK, all **208** archive
file names/sizes, and every decompressed file's SHA-256 against staging.
Rebuilt/reverified after adding the startup finding to the checklist.
The original pre-observation archive is preserved privately under out/task113.

TEST-CHECKLIST.txt is inside KingKong and beside the ZIP. It covers chapter-start
stutters, uncapped FPS, F8/controller chord, About > Run benchmark, hybrid-CPU
scheduling, automatic low-VRAM behaviour, older Vulkan cards, owner fullscreen
checks, defaults, hidden effects, HFR exclusion, shader rebuilding and the
specific multi-second startup observation.

The actual About benchmark action was not launched here; it remains an owner
check, with prior functional coverage in REPORT-105. Physical older/low-memory
GPUs, hybrid CPUs, controllers, fullscreen modes and arbitrary-frame-rate
gameplay are not certified by this RTX 4070 Ti smoke pass.

No game assets, saves or generated shader packs are shipped. No game-derived
data is committed. No download, system installation or Linux build/run occurred.
All task-owned game/helper processes have exited. This report is retained in
the worktree and copied to F:\KK-native-renderer\codex\REPORT-113.md.
Stop here for Claude's verification.
