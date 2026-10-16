# REPORT-172: benchmark "Other programs GPU (%)"

**Result: fixed (branch bench-gpu, 0cd6cdb).** Our own rendering was NOT counted as "other": the monitor runs in the
rendering process in every launch path. The real flaw: the metric counted every engine on every adapter, so a
recorder/streamer (video encode), video playback or a laptop's integrated GPU marked every pass busy. Now only other
programs' 3D/compute engines on the game's own GPU count, and all our own processes are excluded.

**Cause (code + measurement)**
- Launcher / Settings > About Run benchmark: startup parent -> launcher -> relaunch -> new startup parent -> benchmark
  child, which renders AND measures (pid matches; checked in game, Vulkan and D3D12). Command line: parent -> child,
  same. Startup parents never touch the GPU; the closing launcher used the GPU for a few seconds (now ours).
- dwm.exe: 0.4-0.7% in windowed (composed) runs, kept counted. No NVIDIA Vulkan/DXGI work under another pid.
- Player: own busiest 78.9% + other 81.7% > 100%; one engine can't exceed 100% across processes, so the 81.7% was a
  different engine/adapter. Flat ~82% in every chapter = a fixed-rate video session: most likely recording/streaming
  the game (NVIDIA Instant Replay/ShadowPlay, Discord Go Live, OBS: NVENC), else video decode or an iGPU. Not
  provable from the summary (no per-engine data); none of these count now.

**Numbers (windowed 1 pass, other GPU mean; PC not quiet: owner's upscaler python.exe 47-64% 3D, Discord encoding)**
| run | reported old -> new | without upscaler + agent game (offline recompute) old -> new |
|---|---|---|
| Vulkan launcher | 65.6 (old build) / 74.6 -> 74.3 | 19.9-26.1 -> 6.2-6.6 |
| Vulkan command line | 74.9 -> 74.6 | 13.8 -> 4.6 |
| D3D12 launcher | 76.9 -> 76.7 | 20.0 -> 6.2 |
High measured values are real 3D use by the owner's upscaler (correctly counted). "Near 0" was not reachable here.

**Change**: kk/src/benchmark_other_gpu.h (pure logic + tests), benchmark_monitor.cpp (own = same exe path; adapter
of the game's busiest engine; shader engines only), results.txt explains the definition (threshold 15% unchanged).
Dev only: KK_DEV_BENCH_GPU_DUMP=<csv> per-pid dump with names; launcher input benchmark:<n>; codex/run-172.ps1.

**Tests**: kk-dev-vulkan -Test 20/20 (benchmark-monitor timing test failed once on the loaded PC, passed 3/3
reruns + final); kk-beta builds. Game runs: 4 (all exited, slot released). Not tried: real fullscreen (rules),
a quiet PC, AMD/Intel engine names.
Details: wt\bench-gpu\kk\out\task172\details.md. State: codex\state\benchmark.md.
