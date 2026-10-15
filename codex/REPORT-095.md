# Task 095: CPU scheduling and cold driver-cache preparation

15 October 2026. Branch `hw-beta6`, based on released Beta 5 `e890ece`, using the existing -038 SDK. Implementation and offline CPU/cache checks are complete. **The all-CTest gate is not met: 50/51 tests passed.** The optional synthetic shader GPU differential test failed, as detailed below. Nothing was pushed, merged, released or packaged. No game was launched.

## Scope and existing work

Read DECISIONS.md first, then TASK-095, GAME-RULES, TASK-041, TASK-040, REPORT-012, REPORT-085-claude and REPORT-086. No AGENTS.md was found in the worktree or ancestor paths. Standing decisions were preserved: native Vulkan primary, D3D12 secondary, no Classic, no AI throttling, no frame interpolation/generation, no game timing or graphics-default changes. No standing-decision conflict prevented the scoped implementation. The older task 040 merge instruction is superseded by task 095's no-merge instruction; no merge was performed.

Already present, retained rather than repeated:

- Pipeline worker sizing respects Windows process affinity and physical-core topology, reserving two physical cores where possible. Demand jobs can jump the prewarm queue, with a worker reserved for demand work.
- Setup builds SPIR-V and DXIL packs, scale and motion variants, then both pipeline layouts using the existing progress interface. Runtime pipeline recording and Vulkan driver-cache persistence already exist.
- `dbb6e89` supplies Linux driver-cache usability checks and the AppImage-safe setup stamp. Only its missing Windows usability check needed extending.
- The Beta 5 list contains 6,828 records. Reused `testbuilds/beta5-work/pipelines/kkpipelines-beta5.list`, SHA-256 `529a7960ef51f8832de0b8e21c10b5cdc27d19af43407c7245516cdfb33deeab`. No list or other game-derived data was committed.
- SDK 0039/0040 address the measured Linux spins. REPORT-086 did not establish that timer spin on Windows and deliberately retained the original Windows timer. No shared SDK source/install or link was changed here.
- Renderer/backend hot-path optimisation and HFR/game-logic work remain with tasks 091 and 093. Only small scheduling/counter additions touch shared renderer files.

## Changes

### CPU scheduling

- The plugin caps setup shader workers to the allowed logical CPUs minus one, including callers requesting too many workers. The launcher requests this budget instead of computing it from machine-wide `hardware_concurrency`. One allowed logical CPU has a minimum of one worker.
- On Windows, game/draw work is identified at its first hooked Present. That thread, the native render thread, ring skimmer, vblank worker and SDK audio worker opt out of execution-speed throttling. On a hybrid CPU with at least two allowed performance cores, they use the highest EfficiencyClass cores inside process affinity. Homogeneous, E-only, single-P-core and ambiguous multi-group cases retain OS placement. This avoids assigning fixed logical CPU numbers or overriding process restrictions.
- Critical workers retain at least normal priority; audio gets above-normal priority. Pipeline workers use below-normal priority and EcoQoS for background jobs. Queued urgent jobs switch their worker to normal priority and disable EcoQoS before creation. The existing worker-count policy was retained.
- Audio scheduling uses a small SDL audio subclass at its worker's Initialize callback. No SDK patch or timer/AI frequency change is involved.

Windows API semantics were checked against Microsoft's [processor topology documentation](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-processor_relationship) and [thread QoS documentation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadinformation).

### Cold driver cache

- Windows now runs the same Vulkan LoadFile usability check as Linux. A cache from another driver/GPU, a damaged cache or a missing cache causes setup pipeline preparation before Play. A valid warm cache skips compilation.
- Windows setup stamps include adapter identities and installed DXGI driver versions, covering D3D12 as well as Vulkan. Relevant lighting/face choices also invalidate the pipeline stamp; the two scale/temporal layouts remain prepared together.
- Found and fixed an additional coverage gap: 15 listed records depended on shaders absent from `xeshaders.bin`. The D3D library has a vertex container and two pixel containers in the player's `default.xex`. A bounded, read-only extractor obtains those containers using the SDK's exported AES routine, without creating a guest runtime or running guest code. Pack building includes them and hashes their contents into its source identity. The two pixel containers share microcode but have distinct translation inputs, so both are retained. No shader bytes are embedded or shipped.
- Setup now checks completed/queued counts, deferred shaders and failed pipeline creations before writing a ready marker. An incomplete job reports failure instead of being declared ready. Detailed counts remain in logs.
- Added scheduling, extractor and additional-container regression checks, plus optional cache/affinity/negative audits to the existing exported setup-service test tool.

## Measurements

Host: Ryzen 7 7800X3D, 8 physical cores / 16 logical CPUs; RTX 4070 Ti. All measurements are offline. No game-frame percentiles or chapter stutter numbers are inferred.

### Affinity-limited CPU measurements

`nr_scheduling_tests --bench` ran before/after/after/before for each affinity mask. Each sample uses three seconds of synthetic CPU jobs and a 5 ms periodic wake, 600 wakes per run. Before reproduces the old setup formula and existing below-normal shader-worker priority; after uses the new allowed-CPU budget. These measure wake lateness, not game frame time. Each cell below spans the two repetitions.

| Allowed logical CPUs | Mask | Setup workers before/after | Before p50/p95/p99 lateness ms | After p50/p95/p99 lateness ms |
|---|---|---|---|---|
| 4 | 0xF | 15 / 3 | 1.161-1.236 / 1.958-2.238 / 62.417-64.980 | 1.125-1.227 / 1.845-2.096 / 2.082-2.199 |
| 6 | 0x3F | 15 / 5 | 1.182-1.200 / 1.927-1.933 / 31.457-32.949 | 1.079-1.160 / 1.888-1.910 / 2.024-2.113 |

Reserving CPU capacity reduced synthetic background throughput: roughly 148.6k to 112.2k jobs at four logical CPUs, and 222k to 185-186k at six. This is a responsiveness tradeoff, not evidence of faster shader compilation. These masks expose two and three physical cores on this SMT machine, not four/six physical cores and not older CPU IPC/cache characteristics.

The real exported setup service also ran in separate processes under both masks. It logged exactly 3 and 5 shader workers despite the tool requesting 6. Existing pipeline sizing selected one pipeline worker in each case. Both full audits passed, including driver change, damaged/missing Vulkan cache, changed DX12 driver stamp, and incomplete-list rejection. Synthetic topology tests cover mixed P/E classes, SMT siblings and restricted/E-only masks; actual hybrid hardware was unavailable.

### Private driver-cache audit

All cache roots were task-owned folders under `runs/task095-offline`. Fresh application packs and the application's driver-owned VkPipelineCache file started absent. Baseline used a preserved e890ece plugin. The final audit used the new plugin and the same shipped list.

| Case | Baseline | Changed build |
|---|---|---|
| Fresh setup, both APIs | 286.458 s; 15 records deferred per pass | 231.502 s; zero deferred/failed |
| Vulkan driver version changed in private file header | 0.060 s; incorrectly skipped prewarm | 2.938 s; rebuilt both layouts |
| Damaged private Vulkan cache | Not measured | 2.804 s; rebuilt |
| Private Vulkan cache moved aside | Not measured | 2.845 s; rebuilt |
| Warm follow-up | 0.062 s; no compilation | 0.257-0.260 s; no compilation |

Fresh-build durations include shader compilation and other local build activity, so this is not a controlled speedup claim. The Windows warm check adds device/cache validation; it does not rebuild the packs or pipelines.

Final packs contain 17,888 indexed entries per API, up from 17,874. Each API loaded all 6,828 records in each pass: 3,714 unique jobs in the unscaled pass and 4,235 in the scale/motion pass. All queued jobs finished, with zero failures and zero missing shaders. Record adaptation/deduplication and generated motion/tessellation variants mean these job counts are not an additive count of distinct shipped records.

Scale changes, selecting DLSS Quality and API switches subsequently skipped preparation. In the restricted-affinity audits, a changed DXIL driver-stamp value rebuilt DX12 preparation, then its next check skipped it. A private list with one deliberately unknown vertex hash returned `ok=0`, retained the prior valid ready markers and did not mark the bad plan ready; restoring the valid list skipped preparation.

Important limit: this verifies an empty/private explicit Vulkan driver cache and simulated invalidation. The NVIDIA driver's separate implicit system disk cache was not cleared or demonstrably redirected. It does not certify fresh-driver gameplay latency or prove that a genuinely empty vendor-global cache has no additional effects. No owner Documents cache was accessed.

## Builds and validation

- Full `kk-beta` and `kk-dev-vulkan` builds succeeded using -038. No build package was handed over, so no owner-cache cleanup or packaging/release checklist was triggered.
- Beta backend CTest: **23/23 passed**. Run with `ctest --test-dir kk/out/build/kk-beta/native-renderer --output-on-failure`; the beta top-level directory itself has no CTest registration.
- Launcher/hardware/startup CTest in the dev configuration: **3/3 passed**.
- Resources CTest: **2/2 passed**.
- Shader CTest: **22/23 passed** across the full run and failed-test rerun. Pack construction, additional-container coverage, corpus, scale, lighting, database and interpreter checks passed.
- Total: **50/51**, plus three successful manual setup/cache audits and the scheduling benchmark. `git diff --check` passed.

Two existing Windows test-harness defects initially prevented the optional differential tests from executing: `random_program.cpp` omitted `<bit>`, and `setvbuf(stdout, nullptr, _IOLBF, 0)` terminated in the CRT with 0xC0000409. A standalone task-owned probe reproduced the latter. Fixed the include and used valid unbuffered stdout. An early build-only forced include was removed; final shader tests build from the corrected source without that workaround. An initial launcher-test invocation lacked the runtime DLL search path; stopped only its own processes and reran all three successfully with the beta output on PATH. An early concurrent build also attempted to relink the still-running setup test executable; the subsequent build succeeded after that tool exited.

**Remaining failing test: shader `gpu`.** On NVIDIA, with the validation layer reported off, it ran 1,757 synthetic shaders, with zero translation/compile failures, and reported 2,319 component mismatches in 84 shaders: texture-fetch 63, random 20, fuzz 1. The interpreter self-check passed. The translator, interpreter, Vulkan harness and shader execution semantics used by this path were not changed by the CPU/cache work. This task does not establish whether the differences are interpreter tolerances, hardware-specific behavior or translation defects. No baseline numerical control was run, so the numerical failure is not labelled a proven pre-existing regression. Investigating/fixing shader semantics would exceed this CPU/cache task; the all-tests requirement remains unmet and must not be reported as passing.

## Evidence and remaining checks

Evidence root: `F:\KK-native-renderer\runs\task095-offline\`.

- `baseline-service.log`, `complete-service.log`, `complete-cache.log`: baseline defect, full fresh setup, per-API coverage and cache invalidation.
- `affinity-4-service.log`, `affinity-6-service.log` and the corresponding `*-cache.log`: real API worker budgets, repeat starts, DX12 stamp and negative checks.
- `scheduling-bench.log`, `scheduling-tests.log`: exact synthetic samples and scheduling assertions.
- `backend-ctest.log`, `app-ctest-fixed.log`, `resources-ctest.log`, `shader-ctest.log`, `shader-gpu-ctest.log`: test results, including the unresolved failure. Synthetic failing HLSL is in `native-renderer/shaders/out/build/task095/gpu-out`.
- `baseline-build.log`, `beta-build-final.log`, `dev-build.log`, `shader-build-final.log`: builds. Interrupted/failed diagnostic attempts are preserved separately.

Still needed: resolve the shader differential failure before calling the all-CTest gate passed; owner/Claude gameplay checks for chapter 0/8/27 first visits and frame p50/p95/p99; actual P/E-core hardware and a genuinely cold vendor-global cache; other GPUs and Linux shared-code validation. This work does not promise to eliminate texture conversion/upload or other non-pipeline chapter-start stalls noted by task 085. The XEX extractor intentionally supports this title's retail XEX2 basic/uncompressed layout and refuses unsupported formats rather than guessing.

The shared Windows SDK still widens process affinity in its Runtime initialization. The headless helper avoids that Runtime, so its affinity measurements are valid; future game affinity tests must apply their restriction after SDK initialization. This task did not alter that shared SDK behavior.

Zero game launches, zero game-lock acquisitions, no focus operations/screenshots, no downloads, and no user game/save/cache changes. The final shared-lock read shows task097 owns it; this task never modified it or touched that job's processes. No Linux session or unrelated work was started.

Local commits: `e360f6f` (CPU/cache implementation and regression checks), `bd4fb7f` (Windows differential-test startup fixes). Both have the required Claude co-author trailer. The report follows in its own local commit. Generated line-ending-only build changes were restored; no game-derived files were staged.
