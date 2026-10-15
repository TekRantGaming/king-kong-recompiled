# Task 115: recorded benchmark route, restart continuation

Recording package ready; replay implementation and offline checks complete. **Live replay and repeatability remain unverified.** The owner holds the game lock, and an owner-approved route has not been supplied. No game was launched during this continuation.

Worktree: `F:\KK-native-renderer\wt\bench-rec`, branch `bench-rec`, continuing the existing Beta 6 test build work. Read central DECISIONS.md first, then TASK-115 including its restart note, GAME-RULES.md, REPORT-096/110/112, and the existing benchmark, chapter-jump, input injection, frame-clock and checkpoint investigation code. No applicable AGENTS.md was found. There was no standing-decision conflict requiring a code item to be skipped. The cache handover restriction is recorded below.

## Commits

- `c72a88f`: **WIP: input-recorded benchmark recording and replay**. Committed the inherited substantive changes before editing, as explicitly required by the restart note.
- `77e1c8f`: 45-second defaults, finished route replay, input/checkpoint drift reporting, total/chapter graphs and offline regressions.
- `c341574`: capture the guest delta and clock at each pad read, including frame zero immediately after a chapter load.

Each commit ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Only this task's changes were committed. The two generated/manifest files already appeared modified at entry but have no content diff; their pre-existing line-ending state was left alone.

## Recording build for the owner

**Use the refreshed final package:**

`F:\KK-native-renderer\testbuilds\bench-rec-1015-1617\KingKong-bench-record-windows-x64.zip`

SHA-256: `891AAB1A8AEC889F3A97FF4B4FEEA2DE62CF0C5D49815ACD0099963D962ACA6E`

`F:\KK-native-renderer\codex\BENCH-RECORD-READY.txt` points to this zip. HOW-TO.txt is inside KingKong and beside the zip; the folder also contains the checksum and build-hashes.json.

The required duration-only rebuild was published **before any further phase-2 work**, at `testbuilds\bench-rec-1015-1602\`. Its SHA-256 is `CEBF521DAB7EE1FA28B251C858ACFC36BB7747D867A9D6AD241C3206F36F9200`. The final 1617 package supersedes it and includes the completed replay code plus the per-read clock correction. Earlier task115 recording files remain compatible. Old packages were preserved.

Both packages start from `beta6-test-1015-1441\KingKong`, overlaid with this worktree's development executable and DLLs. They were packed with `C:\Windows\System32\tar.exe -a -c -f <zip> KingKong`; PK bytes, archive contents and SHA-256 were checked. The final archive has 217 entries, includes HOW-TO.txt and Start Record Benchmark.cmd, and contains no recording, log or prepared native shader cache. DLSS/FSR/XeSS/AMD runtime DLLs and DXC are present; actual llvm-strings checks find NVSDK_NGX and xess. The runtime matches permanent SDK 041. No Classic plugin is included.

Configure graphics normally, close the launcher, then run **Start Record Benchmark.cmd**, or `king_kong.exe --kk_benchmark_record=true`. Use a controller with Keyboard & mouse disabled. The route remains:

| Order | Chapter-select entry | Chapter | Duration |
|---|---:|---|---:|
| 1 | 2 | Necropolis, early Jack jungle/ruins | 45 game seconds |
| 2 | 8 | V-Rex | 45 game seconds |
| 3 | 10 | Kong, Kong controls | 45 game seconds |
| 4 | 1 | Skull Island, coastal water/outdoor opening | 45 game seconds |

Total: **3 game minutes plus loading**. Both DefaultRoute's default and the recorder's duration default are 45. Chapter switches happen at the first game swap reaching the duration; the final tick may overshoot by one game frame. Cutscenes count; loading between chapters does not. Pause/F8, death/reload or leaving a chapter early invalidates the recording. Closing before completion does not save the route.

After all four chapters, the route is saved and the game returns to the launcher. Default output: `KingKong\user\benchmarks\record-<timestamp>\route.kkbr`; a custom user_data_root replaces the user directory. Saves use a fresh private subdirectory. Nothing is uploaded.

The KKBR version-1 text file preserves the chapter list/durations, build identifier, settings, every captured XInput read's user/status and 16 guest-order state bytes, game-frame index, elapsed game seconds, raw and scaled guest delta, and raw clock word at 0x82CCAE1C. Metadata is now read with the pad itself, fixing cached values on the first reads after a load. Alignment uses summed unscaled guest dt, independently of wall-frame counts. A structural/coverage check prevents the final source from marking an unusable capture complete.

## Replay implementation

About > Run benchmark reads **benchmark-route.kkbr beside king_kong.exe**. `--kk_benchmark=true` also starts replay; `--kk_benchmark_route=<path>` overrides the input file. The launcher gives a translated explanation when the file is missing or invalid. Parsing rejects incomplete/unsupported/truncated routes, invalid indices/times/bytes, unordered samples and missing chapter input/checkpoint coverage. File/sample counts are bounded.

Replay uses the same chapters and game-time durations and holds the latest pad state at the current game time. It reproduces connection status and all pad fields, bypasses physical/developer input and remapping, and disables mouse camera input. Settings applied after the captured hook, including camera response/sensitivity, FOV, HFR enablement and cheats, follow the recording locally; graphics settings and the cap remain available for comparison. No simulation step, AI schedule, input queue, interpolation or frame generation was added.

The results dialog shows total and per-chapter average FPS, 1% low, worst frame and frame-time graphs. Files under `user\benchmarks\run-<timestamp>\` are results.txt, frames.csv, drift.csv and frame-times.svg. The SVG contains total plus chapter graphs. Loading gaps are excluded; in-chapter stalls and frame caps remain included. Total FPS weights the measured intervals. 1% low is the reciprocal mean of the slowest ceil(N/100) frame intervals. Successful host-present returns are reported separately from game swaps and are not claimed to be scanout timing. The existing owned writer and atomic file publication/shutdown barrier are retained.

Drift checks report position distance, checkpoint time lag, input age, absent inputs and button/trigger changes lost between replay polls. Position tolerance is 1 guest unit, checkpoint lag 0.1 seconds, stale input 0.25 seconds. Early position checks before one second are excluded. Skipped ending checkpoints are counted rather than silently discarded. Detected drift produces a do-not-compare warning.

**Coverage limitation:** the existing Jack probe reads three floats at 0x82D64250; its height component at +4 is documented in native-renderer/docs/backend.md and dev_frame_log.cpp. Its full position interpretation and tolerance still need live validation across these chapters. It does not track Kong. Kong checks are explicitly unavailable, and the UI/report warn that route verification is incomplete. This is not a certificate that all four chapters follow an identical path.

No confirmed game RNG seed interface was found in the exposed port hooks/docs. No speculative seed write or AI throttling was added. Fresh saves, identical chapter starts and captured controls reduce variation; AI, particles, collision outcomes, load timing and frame-rate-dependent simulation differences can still vary. Short input pulses can fall between replay polls at a lower frame rate; those are reported instead of delaying/inventing input. The owner route is not bundled or committed pending review/approval.

## Verification

| Check | Result |
|---|---|
| Initial duration check, before phase 2 | PASS: four entries retained, 45 each, 180 total; short dev override retained |
| Final development build | PASS |
| Final non-development Release, KK_DEV_TOOLS=OFF | PASS |
| Final development CTests | **10/10 PASS**, 2.32 seconds |
| Standalone offline suite | **4/4 PASS**, overlapping the development suite |
| Route format/replay regressions | PASS: lossless metadata/state, malformed/incomplete rejection, bounds/order/coverage, disconnected users, reset and transition-loss detection |
| Replay selection at 30/60/120/144/240/1000 FPS and variable deltas | PASS, synthetic offline data; no fixed frame-count alignment |
| Inherited synthetic recording decoded by final reader | PASS: 4 chapters, 3,776 reads, 20 checkpoints, ID c1e530d4b06dbe0a |
| Actual PadRead function in isolated guest-memory fixture | PASS: per-read deltas/clock at chapter frame zero, status/state and inactive guard |
| Actual GraphSvg function in isolated fixture | PASS: valid XML, total + four chapter graphs, preserved 250 ms synthetic hitch |
| New replay UI keys | PASS: English, German, Spanish, French, Italian; no em dashes |
| Task115 PowerShell parser / git diff --check | PASS |
| Final package contents, PK, SHA-256, SDK/exe pairing | PASS |

Evidence is in `F:\KK-native-renderer\wt\bench-rec\out\task115\`, including final-build-tests.log, offline-tests.log, duration-test.log, synthetic-recording-inspection.log, pad-metadata-test.log, svg-audit.log, text-script-audit.log, package-audit.json and final-build-hashes.json. The isolated PadRead/GraphSvg fixtures execute functions extracted from the current source; they are offline fixtures, not a full runtime reproduction.

The worktree's links point to permanent locations, matching mkwt.ps1's layout. Its rexglue-vulkan junction still names SDK 038, so both configured builds explicitly use `CMAKE_PREFIX_PATH=F:/KK-native-renderer/sdk-vulkan-upscalers-041/win-amd64` and `NR_FFX_SDK_DIR=F:/KK-native-renderer/sdk-vulkan-upscalers-041/fidelityfx`. Do not silently reconfigure against the junction's older SDK. Production also sets `NR_DXC_RUNTIME` to the 1441 package's dxcompiler.dll. SDK sources/install/junctions were not edited. The initial cold production configuration used the wrong DXC option name; it was corrected and rebuilt, and the final DLL set was verified.

Final packaged executable SHA-256: `BB39A66570E3359D32EC57C2C6F7F6A13B72BBFB1FD03E148199844CFA929EAC`. Native DLL: `FA707635C746B7CF2790802EB5102B9A84A3BBE6442484B1D0DC061C155D23EE`. Runtime: `9F2E78C4153362E197AE16BFAFECC3E9ECDA11CCC03460D2FB916C461772B2FD`.

## Game rules, gaps and handover

At entry `runs\codex-game.lock` said `user: owner recording the benchmark route`. Following the restart note and GAME-RULES, I did not launch, claim/change the lock, wait for it, touch the owner's game, take screenshots or operate windows. All new tests were offline/windowless.

The prior phase-1 evidence is preserved at `runs\task115-record-20261015-154816\`: one short synthetic-pad run with four 4-game-second chapters, sequential switches, completed route write and return-to-launcher evidence in test-result.json/run.log. Its recorded PID was 31608 and launcher PID 43176. This continuation inspected that evidence and decoded its file; it did not rerun the game. Required windowed/private-root arguments are recorded in arguments.txt. This proves the inherited recording flow, not the new replay runtime or the owner's full 45-second path.

**Still needs owner/Claude verification:** a controller recording through all four 45-second chapters; then replay of that exact file, repeated at 30/60/120/144/240/uncapped as practical, checking chapter transitions, results UI/files, drift tolerances and repeatability. No live performance/variance or deterministic-path claim is made. Review the incomplete Kong/Jack checkpoint coverage before treating results as standardized scores. No owner-approved standard route is included.

The standing handover checklist calls for deleting prepared native-shader caches only while the owner's game is not running. The user lock prevented that step: **Documents\king_kong\cache\native-shaders was left untouched**. The fresh packages contain no user/cache/native-shaders. No files were deleted.

Vulkan priority, native-only rendering, graphics defaults and HFR/AI decisions remain intact. No Linux work, download/install, game-derived data commit, push, merge or release. External writes were limited to the requested task-owned testbuild folders, central ready file and this report. Stop here for Claude's review.
