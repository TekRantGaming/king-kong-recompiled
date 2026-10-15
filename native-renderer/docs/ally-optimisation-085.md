# Task 085: ROG Ally performance (Linux Beta 4 AppImage)

15 October 2026, branch linux-beta4. Nothing pushed, merged, released or deployed.
The Ally still runs the 084 AppImage (cfbb18ea...). Its king_kong.toml is unchanged
(f51bc5fe...). No game of mine is running there.

## Method
- Profiler: `kkprof`, a small static perf_event_open sampler (task-clock, 1 kHz, user callchains;
  perf_event_paranoid is 2 on the Ally, so no root is needed). It runs in the private test root and
  symbolises with eu-addr2line. For call chains and source lines, the native renderer was built with
  `-g -fno-omit-frame-pointer` (profiling builds p1, p2 only; never shipped).
- Runs: Desktop Mode, private root ~/Games/KingKongBeta4/test-085, chapter 8, 1280x720 window, render
  1920x1080, Native / GI High / GTAO Strong (3 samples) / 120 cap, on battery, platform profile
  "balanced" (about 20 W GPU, CPU at 1.4 GHz). The 083 virtual pad walks in circles. Same flags as 083
  (--native_diagnostics --native_stutter_stats --ui_paint_trace).
- Offline: WSL standalone native-renderer build (/root/kk085/nr-build), CTest, NR_BENCH micro-benches,
  and nr_ssgi_bench run on the Ally's GPU (RADV).

## Ranked hot spots (baseline b1, 084 AppImage, chapter 8, battery)
Process CPU 251% of one core. Per thread, share of one core:

| Thread | Core | Where |
|---|---|---|
| Audio Worker | 98% | SDK `PosixConditionBase::WaitMultiple` spinning: 1 ms poll slice truncated to 0 ms |
| Render thread | 39% (15.6 ms/frame) | index range scan of every indexed draw 24%; constants memcmp/copy/byteswap 9%; AO rule classification 5%; hash lookups ~7% spread; malloc/free of commands 3% |
| Main XThread (game logic) | 31% | recompiled game code; not touched |
| Game draw thread | 24% (9.7 ms/frame) | ThreadedSink copying 11.6 KB of device per call (memmove 17.5%), malloc 7%, clock reads |
| SDK timer queue | 21% | disruptor spin-wait strategy yielding in a loop |
| Ring skimmer | 4% | fine |

GPU (same scene): frame 33-35 ms, GI High 18-19 ms, GTAO Strong 6.3-6.9 ms, scene about 8 ms.
On battery the game is GPU bound with these settings.

## Commits and measured gains
| Commit | What | Measured |
|---|---|---|
| a443714 | SDK patches 0039 (WaitMultiple sleeps the real remaining time) and 0040 (timer queue waits on a condition variable; guarded !WIN32) | Audio Worker 98% -> 12% of a core; timer thread gone from the profile; GPU clock 1488 -> 1638 MHz; FPS 27.9 -> 29.7 (a1). Standalone tests: waiter 0.98 -> 0.009 s CPU per idle second |
| 51a6499 | Index range scan only for dynamic vertex buffers (+ test Draw_IndexedFromADynamicVertexBuffer) | about 24% of render thread |
| 4e77a55 | Shader constants compared/converted per 64-byte block | ~5-9% of render thread |
| 61e52f7 | Render thread gets only changed 64-byte device blocks; commands reused (+ test RenderThreadDeviceChangesArriveInOrder, + Bench_RenderThreadQueueing) | queueing 2.95 -> 1.32 us/draw (WSL bench) |
| b4676f3 | SameBytes64 inline compare | 1.32 -> 1.01 us/draw |
| 80f5d8c | REX_DEV_TESS_BENCH read once (was getenv per draw) | 0.8% render thread |
| 7b798fe | AO rule Matches: shader hashes first (identical result) | AoRule::Matches was 5.2% |
| b654a19 | Texture untile/copy with less per-block work | identical hashes; 8.5 -> 5.3 ms per bench set (-38%) |
| 4361554 | nr_ssgi_bench: per-pass times, bumpy scene, exact result compare | test only |
| cc7b530 | backend.md note | docs |

Steady state, chapter 8, High settings, battery (last four 600-frame windows):

| Run | Build | FPS | Game thread CPU | Render thread CPU | GPU frame | Process CPU |
|---|---|---|---|---|---|---|
| b1 | 084 deployed | 27.2-28.5 | 9.6-10.2 ms | 15.6-16.5 ms | 34.4-36.0 | 251% |
| a1 | + SDK spin fixes | 29.3-29.9 | 9.6-10.3 | 15.7-16.1 | 32.7-33.4 | 149% |
| o1 | + C1-C3 | 30.0-30.3 | 6.8-7.0 | 9.8-10.1 | 32.2-32.6 | 119% |
| f1 | all commits | 33.4-33.8 | 6.4-6.9 | 9.1-9.8 | 28.9-29.3 | 123% |

Caution on f1's FPS: its GI pass measured 15.3 ms against 18.2 in o1/a1, but none of the commits after
o1 touches the GI GPU work (reviewed: all are identical-result CPU changes). The pad path differs from
run to run (draws per frame patterns differ), so the GPU numbers between runs carry scene noise. The CPU
numbers (render -38%, game thread -32%, process -51%) are robust. Treat f1's FPS gain over o1 as
unconfirmed.

Owner's current settings (performance quality, GI low, AO 1 sample, FSR), camera standing still,
base 084 build only (run "ob"): 63.5-63.8 FPS, render thread CPU 15.4 ms, game thread 10.3 ms, GPU frame
12.8 ms: CPU bound (render thread). The same run with f1 was NOT done (out of time). Expected from the
render thread drop (15.4 -> ~9.5 ms) is a large FPS gain there; it must be measured.

Chapter-start hitches: unchanged in count (about 29 frames > 50 ms, 4 > 100 ms in the first 10 s;
worst 135 -> 124 ms in o1). Causes: first frames' texture conversion (up to 100 ms in a frame, ~480 ms
total) and buffer uploads (53 ms in one frame), then 1-2 s GPU bound at ~20 FPS right after load. The
texture commit (b654a19) came after the last chapter-start measurement; not measured in game.

## GPU (GI High at 1080p is the main cost)
nr_ssgi_bench on the Ally (1920x1080, bumpy scene): gather ~17 ms, 3 blur passes 6-9 ms, temporal ~0,
composite ~1 ms; GI High at half resolution ~5.5 ms. Tried and reverted (no gain): an LDS-tiled blur
(slower: 21.5 KB LDS cut occupancy to 4 waves), a bit-exact early exit of the gather when a side's
sectors are all hidden (no change), RADV_PERFTEST=cswave32 (no change). Bit-exact GPU wins look small;
the big lever is a settings one (GI Medium/Low on the Ally) and is the owner's call.

## Still to do
1. Measure f1 with the owner's settings (CFG=king_kong.owner.toml PAD=0 SCALEFLAG= in run.sh) against
   run "ob", then decide on deployment (f1 candidate saved, not deployed).
2. Chapter start: measure b654a19 in game; next step is converting textures on worker threads or
   ahead of use, and splitting large buffer uploads.
3. Remaining render thread costs: dynamic vertex uploads (~4-5 MB/frame), volatile constant uploads
   (4 KB VS + 4 KB PS + draw constants per draw), hash lookups spread over many maps, AO motion recording.
4. nr_plugin_tests does not link in the standalone Linux build (FLAGS_native_diagnostics_storage_ and
   FLAGS_native_debug_storage_ undefined); pre-existing. resources/CMakeLists.txt names tests/test_tiling.cpp,
   which is missing from the tree.
5. The Windows SDK very likely has the same timer-queue spin (timer_queue.cpp is compiled there too and its
   thread starts at static init); 0040 is guarded to keep Windows unchanged. Check on Windows.

## Shared-code changes that need a Windows build and test
- backend/game_renderer.cpp/.h, game_resources.cpp (lazy index scan, IsDynamicVertexBuffer, block-wise
  constants, TessellationBenchRequested)
- backend/render_thread.cpp/.h (delta device blocks, command reuse)
- backend/guest_memory.h (SameBytes64)
- backend/game_effects.cpp, game_tessellation.cpp (getenv cached)
- backend/ao_classifier.cpp (rule test order)
- resources/src/guest_texture.cpp, texture_convert.cpp (untile/copy)
- tests: game_tests.cpp, draw_tests.cpp, ssgi_bench.cpp
- SDK patches 0039 (POSIX file only) and 0040 (guarded !REX_PLATFORM_WIN32): Windows unaffected.
WSL results: CTest 19/19 pass (nr_plugin_tests excluded, see above), kknr_resources tests 60 pass.

## Evidence
- F:\KK-native-renderer\testbuilds\linux-beta4\085\: run data b1, a1, p1, o1, p2, f1 (game.log, system.csv,
  prof.bin + .sym/.maps/.threads), kkprof.c, sym.py, an.py (per-thread profile), lines.py (source lines),
  run.sh; candidate AppImage KingKong-v2.0.0-beta.4-linux-x86_64-085-candidate.AppImage
  (SHA-256 accf371ddc20f628331be39b9a01a7a073344298802c14ec5e12f18b6e3e8825, not deployed).
- Build script: testbuilds\linux-beta4\build-scripts\build-085.sh (installs the SDK with 0039/0040 applied
  in /root/kk-beta4-081/sdk-src, then builds the AppImage).
- Ally: ~/Games/KingKongBeta4/test-085/ (all runs incl. "ob", bench/ with nr_ssgi_bench and reference dumps,
  king_kong.high.toml, king_kong.owner.toml; test root's king_kong.toml is currently the owner copy).
