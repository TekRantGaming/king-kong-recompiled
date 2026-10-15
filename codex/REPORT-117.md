# Task 117: benchmark UI, monitor and results

Implemented and packaged for Claude's review. **The requirement to prove no measurable performance cost is NOT established.** All 12 CTests pass and two full Vulkan runs completed, but this variable, drifting route and a single on/off pair cannot establish a small overhead. Four launches were used, including two short harness failures. No more game launches are permitted for this task. The final atomic tick timestamp, more precise latency wording, current-FPS sampling and palette refinements passed builds/offline tests but have not all been exercised together in the game. Do not describe the package as fully runtime-verified.

## Scope and commit

Read `F:\KK-native-renderer\DECISIONS.md` first, then TASK-117, GAME-RULES, REPORT-115/116/100, benchmark code and the F8 panel's style/layout. No applicable AGENTS.md was found. No task item was skipped because of a standing-decision conflict. Worktree `F:\KK-native-renderer\wt\bench-rec`, branch `bench-rec`, entered at `e6a1858` after task 116. The generated CMake/manifest line-ending states present on entry remain uncommitted and have no content diff.

Code commit: `8bd111a32a897d675239f43828ca8395b8e47ff5`, with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Only task source, translations, tests and task runner/package scripts are included. No game-derived assets, recordings, screenshots or logs are committed.

No game logic, AI scheduling, animation, physics, delta, frame-cap/default-setting or replay behavior was changed. Vulkan remains primary, DirectX 12 secondary, with no Classic renderer, interpolation or frame generation. No Linux work, SDK edits, download/install, main/beta-line update, push, merge or release.

## UI and data

- Extracted the existing F8 palette into `graphics_panel_style.h`, retaining the F8 panel's existing colors and font. Benchmark start/progress/live/results use that palette, brass headings, separators, buttons and charts. Result tabs also have explicit brass colors, replacing the inherited green ImGui tab colors found during visual review.
- About > Run benchmark validates the route and opens the new F8-style explanation/start screen. Its Start callback preserves the existing launcher launch path and graphics settings. Command-line `--kk_benchmark=true` continues to start directly.
- The live panel shows current FPS from the latest sampling batch, whole-run average/1%/0.1% lows, p50/p95/p99, >50 ms stutters, process CPU, busiest process GPU engine, process/system RAM, VRAM usage/budget, GPU scene-frame time, game/render thread CPU execution time, pipeline wait, tick age, render/output dimensions, upscaler and preset/API. Installed NVIDIA NVML supplies clock, temperature and power on this PC. Unsupported vendor readings are omitted; other unavailable readings display Unavailable.
- The scrolling graph contains the latest 240 measured swap intervals. Result graphs use peak values per elapsed-time bucket, preserving hitches. Total and four chapter tabs contain frame statistics, counter means/peaks, comparison values and a bottleneck indication. System information includes CPU, GPU inventory/driver, physical RAM, OS, actual API/render dimensions/upscaler and requested preset. The inventory is explicitly labelled rather than pretending to identify the selected adapter on an untested multi-GPU machine.
- Automatic files: `results.txt`, `frames.csv`, `summary.csv`, `drift.csv`, `frame-times.svg`. Save results rewrites the human text and aggregate CSV in the same local result directory; Copy results/Copy CSV copy those formats. The previous completed file set is retained. Nothing is uploaded. Local history is taken from the newest `run-*` directory in this user-data root; the reader accepts old task-116 text or new aggregate CSV. Older runs show missing counters as unavailable. It does not fingerprint the PC or enforce equal route/settings; the comparison warning asks the reader to check the reports. Tests used files known to originate on this PC.
- New UI text and dynamic metric labels use the existing translation lookup, with English, German, Spanish, French and Italian coverage. The audit covers **73 keys**, placeholders and the no-em-dash rule. Existing detailed diagnostic fields and machine-readable CSV column identifiers remain stable English schema; new human metric/measurement notes use the translation table.

Frame statistics use successful consecutive guest-swap intervals, excluding chapter loading and including in-chapter stalls/cap waits. Total FPS is count divided by total measured time. Lows are reciprocal means of the slowest `ceil(N/100)` and `ceil(N/1000)` intervals; percentiles use nearest rank. Stutters are strictly greater than 50 ms. Final FPS reports the last valid interval, not an extra timing series. Successful host-present-return intervals remain separately exported and are not scanout timing.

CPU % is process CPU time normalized over all logical processors. Process RAM is working set; system RAM is physical used RAM. GPU % is the busiest matching process PDH GPU Engine instance, capped at 100%, not a sum of engines or whole-adapter utilization. VRAM comes from the existing active-API DXGI QueryVideoMemoryInfo/Vulkan memory-budget query, refreshed near 1 Hz. Scene GPU time is the existing asynchronous whole-command-list query, averaged over recent frames, excluding presenter/UI. Game CPU time samples the thread executing the benchmark frame hook; render CPU time uses the renderer thread's cumulative execution time. CPU/pipeline deltas are divided by rendered frames in valid sampling windows. Counter means are weighted by sampled time; displayed peaks are sampled peaks, not guarantees of instantaneous hardware maxima. Boundary windows crossing loads/chapters are discarded.

**Latency definition:** the final field is `Game tick age at swap (ms)`: steady-clock time from the latest observed game frame/tick hook to guest-swap entry. Title simulation and swaps can run on different threads, so an atomic nanosecond timestamp is used and there is no claim of matching frame IDs. A timestamp newer than the sampled swap is unavailable rather than negative. This interval can include waits since that tick; it excludes Present, display/input latency and any claim of end-to-end latency. The monitored screenshots/export were made before this final wording/atomic correction and still say frame start to guest swap. Claude should verify the final field in a new owner-authorized run.

Bottleneck indication compares GPU ms with the slower CPU-thread ms, using a 15% margin. Close timings, or work below 75% of the measured interval, report Mixed or cap-limited. Missing timing reports unavailable. This is a diagnostic indication, not proof of the complete critical path. All four monitored chapters indicated GPU-bound on this PC.

## Cost and validation

The frame producer uses a fixed 8,192-entry SPSC queue, without locks, allocations, sorting or OS-counter calls. The worker drains the queue and samples counters approximately four times per second, including sorting the accumulated frame statistics. GPU query results are reused without a GPU wait. New renderer telemetry publication is disabled outside monitored runs. Optional NVML is loaded only from System32, requires a single NVIDIA device and matching active vendor, and is not a required dependency. No component was installed.

The full monitored run logged **23,069 UI draws at 0.043229 ms of UI construction per draw**, **965.6674 ms of worker-loop wall time across 730 samples**, and **zero dropped samples**. Worker initialization is outside that loop-cost sum and starts before chapter measurement. These are measured nonzero costs; they do not independently measure UI GPU cost. The offline producer fixture reported approximately **1.10 ns per accepted enqueue and 1.36 ns on a full queue** on this build, a microbenchmark rather than a game-thread cost guarantee.

| Scope (route order) | Monitor on FPS | Monitor off FPS | On relative to off |
|---|---:|---:|---:|
| Total | 126.750921222 | 123.598306899 | +2.55% |
| 1 | 138.071263356 | 127.594270564 | +8.21% |
| 2 | 103.307264408 | 99.7994907423 | +3.51% |
| 3 | 101.1409413 | 98.591683813 | +2.59% |
| 4 | 164.528874592 | 168.489731417 | -2.35% |

Both full runs requested uncapped Vulkan, Native preset, 1280x720 windowed arguments, upscaling off, High GI and Strong GTAO, matching task 116's test setup. The actual internal/output picture was 3840x2160, confirmed by renderer readout; the window arguments do not imply a 720p internal render. Both runs reported route drift. The monitored run had early live-panel PrintWindow captures and a developer language tour; the baseline removed the live panel/counter worker. Builds and test suites were not run during measured gameplay. Desktop activity, AI, cache and thermals were not held constant. The binaries also differ by final UI/baseline refinements, and the final atomic-label correction came afterward.

The total on result is 2.55% faster, while the final chapter is 2.35% slower. Neither is attributable to the monitor from this evidence. Task 116 already found materially larger uncontrolled chapter variation. **No measurable-overhead proof remains unmet**, and no performance improvement is claimed. A controlled fixed-scene/capture-free A/B protocol and UI GPU timing still need Claude/owner authorization beyond the four-launch limit.

| Check | Result |
|---|---|
| Final development Release build | PASS |
| Final production Release, KK_DEV_TOOLS=OFF | PASS |
| Final CTest | **12/12 PASS**, 3.14 s |
| Benchmark text audit | PASS, 73 keys in five languages |
| Monitor/history fixture | PASS: low/quantile/stutter math, weighted missing data, bound decisions, legacy/new history, malformed values, size check, queue draining, per-chapter latency and current readouts |
| Independent full-run CSV recomputation | PASS for total + all chapters: counts, seconds, average, both lows, quantiles, worst, stutters and final FPS |
| Export SVG/XML | PASS, total + four chapter graphs in each full run |
| Local history | PASS: task-116 text in monitored run; monitored summary CSV in baseline |
| PowerShell syntax / staged diff check | PASS |
| Archive/upscaler/hash checks | PASS |

The existing route, writer, pacing, startup recovery, graphics panel, text, settings and game-slot tests passed alongside the new tests. No physical mouse/controller Start, Save or Copy action was exercised. The real start dialog was displayed via its developer preview; launcher callback wiring was reviewed in code. Automatic exports and independent file checks passed. DirectX 12 runtime telemetry, AMD/Intel hardware, multi-GPU selection, missing-driver counters on real hardware and final player-executable gameplay were not tested. The final atomic timestamp/palette/current-readout refinements have offline/build coverage rather than a final monitored game run.

## Exactly four launches

Every launch used an owned copy under `runs`, copied private test userdata, explicit private data/config/cache/log paths, required windowed 1280x720 arguments and both SDL activation hints set to 0. The shared game-slot/priority checks and existing kkbg2 helper were used, with an additional process check before the last launch. No foreground action, exclusive fullscreen, borderless test, desktop screenshot or user's game operation was used. Only the recorded owned PID was stopped. All runs were below ten minutes. The lock is free and kkbg2 remains running.

| Run | PID | Outcome |
|---|---:|---|
| `task117-vk-on-1-20261015-171422` | 48084 | Short harness failure: log reader lacked sharing flags. Stopped through finally, lock released. Counts as launch 1. |
| `task117-vk-on-2-20261015-171607` | 37004 | About 30 s: captured live languages, then runner mistook imported prior SVG for new results. Fixed selection to exclude imported history. Counts as launch 2, not a complete benchmark. |
| `task117-vk-on-final-20261015-171753` | 31004 | Full monitored route, all four chapters, files and results UI. Result `run-1792081074588598`. |
| `task117-vk-off-final-20261015-172312` | 10248 | Full monitor-off developer baseline, all four chapters and files. Result `run-1792081393852046`. This hidden-live-UI test mode did not produce results-overlay tour captures; its frame files are the baseline evidence. |

Own-window PrintWindow images are under `F:\KK-native-renderer\documentary\benchmark-ui\`, in the corresponding label folders. The monitored full run has start, five live-language images and result/detail images. Reviewed accented labels, metric values, charts, footer actions and system information. The timed capture loop missed some result-language stages, and chapter images sometimes retained the scrolled detail position. The five-language static audit passed, but not every final result page/language/scroll state has a visual certificate. The green result tabs seen in these captures prompted the final brass-palette fix; do not use an old image as evidence that the final palette was runtime-verified.

The two inherited startup ResolvePath device-not-found messages appeared, as in prior reports. Route drift and Kong's unavailable position checkpoints remain unchanged. These benchmark figures are diagnostic, not standardized scores.

## Test package

`F:\KK-native-renderer\testbuilds\beta6-test3-1015-1729\KingKong-beta6-test3-windows-x64.zip`

SHA-256: `5DAD1E418DD4645EEBE5EDB6FA88514219B6CEFFAFB70A99E585CC73E7FCBACA`

Refreshed from task 116's `beta6-test2-1015-1649\KingKong`, overlaid final production executable/native DLL/runtime and the new TEST-CHECKLIST.txt. All other retained files were hash-compared. The bundled owner route is unchanged (SHA-256 `4FDAF2B8E8CCDB25D5062DC8AA2734A7197AE9C6FE06043159B256B177527EFD`). Developer tools are off; the monitor-off/tour aids are not in this player build. The inherited executable version remains `2.0.0-beta.5`; test 3 is the requested package name.

Created with `C:\Windows\System32\tar.exe -a -c -f <zip> KingKong`. PK and all 216 entries were checked; a fresh extraction matched **all 209 file SHA-256 values**. The folder contains SHA256SUMS.txt, build-hashes.json and TEST-CHECKLIST.txt; the checklist is also inside KingKong. DLSS, FSR/AMD DX12/Vulkan, XeSS and DXC runtime DLLs are present. Actual llvm-strings checks found NVSDK_NGX and xess in the native DLL. No Classic plugin, game assets, recording shortcut or prepared native-shader cache is included. Upscaler visual quality/performance was not retested; gameplay runs used upscaling off.

Production hashes:

- `king_kong.exe`: `C0F847B4DCD9581DC01D8988EB7E2BDAF0D6DB3FDC8C385238E08DCDC6009637`
- `rexgpu-native.dll`: `DB69458E8D993D0CC6B01521FAE0501D5290F60667459032A165DE946579258E`
- `rexruntime.dll`: `9F2E78C4153362E197AE16BFAFECC3E9ECDA11CCC03460D2FB916C461772B2FD`

Both builds reuse the mkwt-created worktree with permanent asset/generated/tool/upscaler targets. The old SDK-038 junction is unchanged; configured CMAKE_PREFIX_PATH and NR_FFX_SDK_DIR explicitly retain permanent SDK 041, with the existing DXC runtime path. Source was built before its final commit; the inherited recording-build suffix still names task115. No SDK install/junction was modified.

The standing cache handover check at `2026-10-15T17:30:15+01:00`, with no game running and lock free, found no Documents prepared native-shaders cache and no such task-117/package cache. Nothing was deleted or moved. User saves, Documents recording and Desktop game copy were untouched.

Evidence: `out\task117\build-dev-atomic.log`, `build-player.log`, `text-audit.log`, `monitor-cost.log`, `runs-analysis.json/log`, `package-audit.json`, `archive-verification.log`, `permanent-links.json`, `cache-check.json`, `script-audit.log`, individual run logs and `runs\task117-launches.txt`. Package hashes/contents are also recorded beside the zip. Stop here for Claude and owner verification; no additional runs, push, merge or release.
