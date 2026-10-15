# Task 096: pacing and built-in benchmark

15 October 2026. Worktree `F:\KK-native-renderer\wt\pace-beta6`, branch `pace-beta6`, base Beta 5 `e890ece`, SDK -038. Implementation commit: `e570e28`.

Implemented the benchmark, limiter safeguards and a private SDK presentation patch. Functional checks pass, but pacing acceptance is partial: measurements do not establish a general variance improvement, and identical-build benchmark repeatability below 3 percent is not certified. This is for Claude's review, not shipping approval.

Read DECISIONS first, GAME-RULES, briefs 042/044, REPORT-012 including Hardware risks, REPORT-075 and REPORT-083-claude. No current instruction required violating a standing decision. Skipped the already integrated on-demand UI painting and SDK 0038 Vulkan pipeline-format fix. The old 042/044 reports contain no implementation to reuse. Vulkan remains primary; API selection, Remastered/Original default caps, GI defaults, game logic and AI scheduling are unchanged. No interpolation, frame generation, fixed simulation step or AI throttle was added. No Linux work, downloads, installations, push, merge, release or player package.

Changes:

- `kk/src/frame_pacer.h` replaces the limiter's private implementation. Cap changes reset the deadline immediately; uncapped does not wait; rendering overruns start a new schedule rather than producing a short catch-up frame. Small timer wake errors retain phase instead of accumulating rate drift. The high resolution waitable timer has a sleep fallback on creation, setting or wait failure. The final yield interval adapts between 100 and 500 microseconds, replacing the unconditional 500 microseconds. The handle is owned and closed. The wait stays at the existing guest-swap hook; no input queue or extra framebuffer was added.
- SDK patch `0039-pacing-benchmark-present-observer.patch` records successful host present returns through an optional exported callback. D3D12 requests a waitable swapchain and maximum frame latency one, waits before consuming the latest guest output, preserves creation flags across resize, closes the handle on destruction, and falls back with a warning if unsupported. VSync on now uses `Present(1, 0)`; off retains restart/tearing presentation. The swapchain still has its existing three buffers. Implicit VSync is reported to the common presenter to avoid a second UI timer.
- `ApplyVSyncSettings` preserves explicit CLI/environment mailbox choices when VSync is off, while VSync on always forces Vulkan FIFO. The log now prints the actual mode flags. Vulkan's existing bounded submission/mailbox handling is retained. SDK -038 does not enable present-id/present-wait, so no unsupported present-wait call or extra CPU delay was introduced.
- About > Run benchmark goes through the launcher's existing shader preparation and saved settings flow, then starts a fresh benchmark session. Cancelling preparation clears the pending benchmark request. New launcher/results labels have English, German, Spanish, French and Italian text.
- The benchmark uses a separate user directory before dialogs or runtime creation, so saves and achievement state are private. Normal pad input and mouse camera input are replaced during the run. Opening pause/graphics UI or changing levels invalidates it. Network playing/presence startup is skipped for this session. The transient benchmark flag is reset and is not saved.

The benchmark route is `chapter8-camera-v1`: chapter-select entry 8, ten simulation seconds of warmup, then a 75-second replay. It reuses the horizontal camera sweep from `native-renderer/tests/task016-run.ps1`, repeated in three 25-second views. The character stays at the chapter start; this is one area's camera replay, not a traversal of several chapter areas. Timing follows the game's unscaled delta without altering the simulation. AI and scene animation can still diverge, as allowed by the standing decisions. Loading is excluded; stalls inside measurement remain in the wall-clock intervals. Simulation and wall duration can differ after long stalls.

Results include average FPS, reciprocal mean of the slowest `ceil(N/100)` frames for 1% low, worst frame, stdev, p50 and p99. The graph keeps the peak in each elapsed-time bucket. Sorting and file IO happen after measurement on a worker. Game swaps and successful host present returns are separate series; neither host returns nor mailbox counts prove unique visible scanouts. `results.txt` contains redacted hardware/settings and summaries; `frames.csv` contains every interval. Copy results uses the same redaction as diagnostics. Both files are under `<user root>/benchmarks/run-<timestamp>/`. `Stats`/`Summarize` are reusable without the runtime for later automatic settings work. Caps are retained and reported; capped results are not maximum throughput scores.

Validation:

- `kk-beta` builds, with `KK_DEV_TOOLS=OFF`. Benchmark code has no dependency on developer scripts or hooks.
- All **23/23 CTests pass**, final run 70.04 seconds. Tests cover deadline transitions, overload/resume, wake-error drift, arbitrary rates, replay boundaries/integrated sweep, statistics, CSV, graph peak retention and VSync flag precedence, plus the existing renderer suite. The private patched runtime also passes **16 no-game presenter scheduling checks**, including quiet UI, close/clear, expose, resize and recovery.
- PowerShell scripts parse, the Python summary script runs, SVGs parse as XML, every literal benchmark UI translation key is present, `git diff --check` passes, and patch 0039 passes `git apply --check` against SDK -038. Blank context-line whitespace in the patch was normalized after the implementation commit; no binary changed for that formatting correction.
- `llvm-strings rexgpu-native.dll` finds `NVSDK_NGX` and `xess`. DLSS, XeSS, both FidelityFX API DLLs, AMD loader/upscaler DLLs and DXC are beside the build. This was not packaged or handed over as a player build.

Exactly four game processes were started, sequentially, with the shared lock and priority checks, kkbg2 PID 11936 running, both SDL activation hints zero, and the required windowed 1280x720/fullscreen-false/desktop arguments. Each used a copied exe/DLL/glyph/shader/pipeline-list/DXC set, copied test userdata and private cache under `runs/task096-*`. The SDK reported a 3200x1800 physical swapchain due to this desktop's DPI scaling. Runs lasted about 1.5 minutes; none approached ten minutes. Only those processes were closed. Captures are DPI-aware PrintWindow captures of their own windows after measurement. No desktop capture, focus request, user game/save/cache access or file deletion. Other tasks' lock/priority reservations were respected. Our four processes have exited; subsequent task099 ownership was left alone.

| Case / run folder suffix | PID | Cap | Game average FPS | 1% low FPS | Worst ms | Stdev ms | p99-p50 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| before-mailbox-20261015-103303 | 42136 | 60 | 60.000 | 58.634 | 29.571 | 0.253 | 0.044 |
| after-mailbox-20261015-103517 | 34956 | 60 | 59.992 | 57.335 | 26.263 | 0.221 | 0.191 |
| after-fifo-20261015-103909 | 632 | 120 | 119.979 | 108.778 | 21.336 | 0.200 | 0.199 |
| after-d3d12-vsync-20261015-105231 | 37384 | 120 | 119.973 | 114.572 | 24.892 | 0.186 | 0.181 |

All folders are under `F:\KK-native-renderer\runs\task096-`. Original look, scale 1, upscaler/AO/GI off, async pipelines and validation off; RTX 4070 Ti, Ryzen 7 7800X3D, Windows build 26300. The control executable has Beta 5's limiter with only the new benchmark recorder inserted into its swap hook; it uses the same patched SDK and renderer as the updated executable. `test.json` records arguments and hashes. Both Vulkan mode choices are confirmed by SDK logs: mailbox mode 1, FIFO mode 2. D3D12 confirms the waitable swapchain/max latency one path. All four logs retain the same two startup filesystem device-resolution errors seen in REPORT-075, with no critical/device-loss error found. Those filesystem errors were not fixed here.

| Successful host present returns | Samples / seconds | Stdev ms | p99-p50 ms | Worst ms |
|---|---:|---:|---:|---:|
| Vulkan mailbox before | 4510 / 74.995 | 0.798 | 0.348 | 51.465 |
| Vulkan mailbox after | 4517 / 75.008 | 0.959 | 0.417 | 45.122 |
| Vulkan FIFO after | 8251 / 75.011 | 0.291 | 0.114 | 20.653 |
| D3D12 VSync on after | 8105 / 74.997 | 0.570 | 2.314 | 20.823 |

The direct mailbox comparison lowers game stdev but worsens p99-p50 and host stdev. FIFO and D3D12 present returns do not keep up with the 120 guest swaps on this desktop; the callback correctly exposes this difference. No overall smoothness, scanout, latency or GPU throughput improvement is claimed. The two 60-cap average FPS values differ by 0.013 percent, but the limiter changed between runs and the cap hides throughput variation. This does not certify identical-build benchmark repeatability below 3 percent.

Offline limiter measurements used the final test executable, four warmup calls, five seconds per case, caps 30/60/120/144/240, and a high resolution simulated work overrun of 1.5 ms every 17 frames. The shared slot was held with no game running. Values below are before / after, in milliseconds.

| Cap | Quiet stdev | Quiet p99-p50 | Overrun stdev | Overrun p99-p50 | Overrun average FPS |
|---:|---:|---:|---:|---:|---:|
| 30 | 0.0048 / 0.0601 | 0.017 / 0.172 | 0.510 / 0.364 | 1.635 / 1.638 | 30 / 29.925 |
| 60 | 0.0044 / 0.0782 | 0.011 / 0.260 | 0.521 / 0.366 | 1.613 / 1.500 | 60 / 59.684 |
| 120 | 0.0385 / 0.0702 | 0.022 / 0.183 | 0.524 / 0.363 | 1.550 / 1.557 | 120 / 118.736 |
| 144 | 0.0305 / 0.0755 | 0.030 / 0.235 | 0.521 / 0.361 | 1.536 / 1.545 | 144 / 142.166 |
| 240 | 0.0248 / 0.0667 | 0.046 / 0.210 | 0.522 / 0.363 | 1.540 / 1.546 | 240 / 234.904 |

Quiet averages hold the cap, including the display's 120 Hz rate. Overrun stdev falls about 30 percent because catch-up frames are removed; the resulting average falls when work misses the cap. Quiet precision is worse with the shorter adaptive yield. At quiet 240, measured wait-thread CPU was 23.13 to 14.36 percent of one logical CPU; the injected-overrun case rose from 0.94 to 4.37 percent. Thread-time granularity is coarse and these are not handheld power measurements. Earlier runs made during compilation or using coarse injected sleeps are excluded.

Evidence and reproduction, with no further game launches for this task:

- Build/test logs, run ledger and synthetic CSVs: `kk/out/task096/`.
- Graphs: `kk/out/task096/analysis/before-after.svg`, `game-runs.svg`, `synthetic.svg`; numeric results in `analysis/summary.json`. These and game-derived evidence remain uncommitted.
- Rebuild the private SDK runtime with `codex/build-presentation-096.ps1`, then `codex/build-096.ps1 -Target all-tests` or `-Target kk-beta`. The build helper copies the private DLL beside the build. It relinks four recompiled SDK objects, including all built consumers of the changed D3D12 header, against the existing -038 objects. Shared SDK source/install files and worktree junctions are not edited. For normal integration, Claude must apply 0039 and rebuild a paired SDK; plain `kk/build.bat kk-beta` against unpatched -038 will not include these presenter changes. The observer does not alter the base Presenter layout; the D3D12 private layout does change.
- Reproduce plots using the required absolute Python 3.12 executable with `-I codex/summarize-096.py --runs kk/out/task096/game-runs.json --synthetic kk/out/task096/pacing-verified.csv --output kk/out/task096/analysis`. No plotting package is installed; the script writes SVG directly.

Final build SHA-256:

| File | SHA-256 |
|---|---|
| king_kong.exe | C9A3583EEFAE39A960A8583100C3B8CE784BD253834D15598CC69DF7EFCB622D |
| rexruntime.dll | 771F33D5B1C99DF7F99CCDD8272EAB0F08E44C02CEE7837F4205E4C8D78A0A4B |
| rexgpu-native.dll | 07E51F649A5A56E9ABF4C87DADD8117039EF05E890F9F1F1C49B0D89016671C5 |

Example saved output from the updated mailbox run, abbreviated:

```text
King Kong benchmark v1
Status: complete
Version: 2.0.0-beta.5
Route: chapter8-camera-v1
native_backend = vulkan
kk_frame_rate = 60
Samples: 4500
Measured seconds: 75.010
Average FPS: 59.992
1% low FPS: 57.335
Worst frame ms: 26.263
Stdev ms: 0.221
p50 ms: 16.667
p99 ms: 16.858
p99-p50 ms: 0.191
```

Reflex/Anti-Lag research only: NVIDIA's published Reflex guide requires frame tokens, sleep placement and latency markers across the simulation/render/present boundaries. The published Reflex repository licence is the NVIDIA SDK agreement plus a supplement with NVIDIA-GPU scope and attribution/UI requirements; it is not MIT. Estimated effort here is several engineering days plus validation, because guest frames and host presents are separate timelines. This is an implementation estimate, not an SDK promise. No Reflex SDK was added. Sources: [NVIDIA Reflex guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideReflex.md), [Reflex licence](https://github.com/NVIDIA-RTX/REFLEX/blob/main/license.txt).

AMD Anti-Lag 2's published SDK is MIT and its DX12 integration calls Update just before input polling, with device initialization and shutdown. Its guide documents DX11/DX12, not a Vulkan integration. Estimated work is one to three engineering days plus Radeon testing, using the real game input boundary and avoiding stacked limiters. No SDK was added and secondary D3D12 work must not hold up Vulkan. Source: [AMD integration guide and licence reference](https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK/blob/main/README.md). The waitable swapchain implementation follows Microsoft's [latency setter](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency) and [waitable handle lifetime/placement](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject) documentation.

Remaining acceptance gaps: the complete per-cap/API/VSync runtime matrix, D3D12 VSync-off and before/after runtime comparison, repeated identical-build uncapped benchmark runs below 3 percent variance, low-power handheld timing/energy checks, unsupported-timer/waitable fallback hardware, physical VRR/scanout and measured input latency. Borderless/exclusive fullscreen were reviewed in code only: SDL mode changes and the native HWND presentation paths remain; the user must check refresh switching, tearing/VRR, Alt+Tab and return to desktop. No claim is made that a synchronized presenter proves VRR. No fifth game run was made. Claude should review the mixed pacing result and the private SDK pairing before using this work.
