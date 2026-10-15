# Task 110: Beta 6 review against released Beta 5, no game

15 October 2026. Reviewed `beta6-int` at `d0c0ed5` against released Beta 5 `e890ece`, including every one of the 153 changed files. Completed the read-only review before editing production source. Two small fixes are committed separately. Both game configurations build and all 65 configured CTests pass. Five remaining findings are ranked below for Claude. No game was launched and no test build was handed over.

Read `F:/KK-native-renderer/DECISIONS.md` first, then TASK-110 and REPORT-094/095/096/098/100/101/103/105/108. Also read `codex/GAME-RULES.md` and the integrated copies of the reports, including the 095/096 follow-up amendments. No applicable AGENTS.md was found. No standing-decision conflict required skipping the Windows review or fixes. Linux guards were reviewed statically; no Linux build, execution or implementation work was undertaken under the standing hold.

## Findings

P1 = high; P2 = medium; P3 = low. Line numbers refer to source after the two fixes. Unfixed findings are source-review findings, without a game reproduction.

| Severity | File:line | Issue | Fixed?/commit |
|---|---|---|---|
| P1 | `native-renderer/plugin/shader_pack_service.cpp:416` | Setup created a host with reduced descriptor capacities, then passed default full capacities to both `PipelinePrewarm` passes. On a GPU below the full-table limits, shader preparation could request unsupported layouts even though gameplay and launcher prewarm use the supported reduced layouts. Pass `host->table_capacities()` into setup prewarm too. | **Fixed**, `1f1402d` |
| P2 | `kk/src/benchmark.cpp:143` | The settings snapshot is taken at level load, but opening graphics/pause UI during the ten-second warmup did not invalidate the benchmark. A player could change settings before measurement and receive a complete result labelled with the old snapshot. Warmup now uses the same interruption check as measurement. | **Fixed**, `b9e461f` |
| P2 | `native-renderer/plugin/shader_pack_service.cpp:458`, `:465` | Failure to create the secondary API's prewarm device makes the entire setup job return `ok=0`, including when the selected Vulkan API prepared successfully. An unsupported D3D12 path can therefore cause an incorrect Not prepared status and repeated failed preparation attempts. The launcher currently proceeds into play after the job finishes; this is a setup-status/retry defect. The per-API success/error policy needs review, retaining Vulkan priority and genuine selected-API failures. | No; left for Claude because this changes the setup failure policy across both APIs. |
| P2 | `kk/src/benchmark.cpp:80`, `:114`; `kk/src/king_kong_app.h:86` | The results writer is detached, and accepted window close does not wait for it. SDK 041's `src/ui/rex_app.cpp:511` calls `std::_Exit(0)`. Closing while Saving results is displayed can terminate the writer with missing or partial output files. Graceful teardown also has no owned-worker completion barrier. | No; needs an owned writer and a shutdown contract that accounts for the SDK hard-exit path. No shutdown/thread ownership redesign made here. |
| P2 (test tools) | `codex/test-panel-100.ps1:93`; `native-renderer/tests/vulkan-098.ps1:102` | These runners check the shared slot, then claim it with `Set-Content` without holding a file handle across check-and-claim. Two such runners can both observe free and launch games. This violates the one-game rule if the runners are reused concurrently. Their historical launch allowances are already consumed; neither was run here. | No; bring these historical runners onto one shared atomic locking contract before reuse. |
| P3 | `kk/src/benchmark.cpp:181`; `kk/src/benchmark_data.h:62` | The main benchmark labels are translated, but the complete raw report is also displayed in the results dialog. Its status, route/settings explanations and detailed statistics remain English in German, Spanish, French and Italian. Stable English diagnostics on disk can be retained separately from translated visible details. | No; requires additional UI translations and presentation work. |
| P3 (Linux) | `kk/src/benchmark.cpp:211` | Present-observer installation is entirely inside `_WIN32`. The generic SDK patch exports the observer on Linux too, but an AppImage benchmark will always report host timing unavailable. This is visible from the guards without running Linux. | No; deferred under the standing Linux hold. |

Both fix commits have the required `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` trailer. No other production behavior was changed.

## Review coverage and decisions

- Read all application, renderer, plugin, shader translator/reference-model, test, CMake, SDK patch, tooling and documentation diffs. The original 153-file inventory with SHA-256 hashes is `out/task110/review-manifest.json`.
- Recompiled all 22 changed generated shader headers from their current HLSL with the existing official DXC 1.9 and the checked-in flags. Every SPIR-V and DXIL byte array matches exactly. The native CTest SPIR-V validation gate also passes. Details: `out/task110/artifact-audit.json` and `audit.py`.
- Reviewed descriptor reservation/drains, cache owner removal, slot/resource retirement, depth/stencil plane selection, staged memory migration/cancellation, live effect ownership and host queue exclusion. Both APIs' memory migration and reduced-table rendering/AO/GI fixtures pass. No additional proven image-lifetime or memory-content defect was found in this review.
- Remastered/Original default caps, Strong GTAO, GI preset/default/migration behavior and explicit player settings remain as in Beta 5. Vulkan remains the default primary API; startup recovery is native-only. Shadows, water reflections and shafts remain hidden and Original/Off. Ann tessellation remains off by default and confined to F8. No AI throttle, frame interpolation/generation or game timing change was introduced by these fixes.
- Beta 5's settings schema is retained. Translator version 5 and the added executable shader containers intentionally invalidate old shader-pack identities. No player's settings, saves or caches were accessed or changed.
- The panel text CTest covers 135 keys in five languages. An additional audit covers all 18 benchmark UI keys, with no missing translations or em dashes. The raw-report localization finding above is separate from key coverage. All 12 changed PowerShell files parse; all four changed Python files parse.
- Reviewed hot-path additions and the known GPU-completion waits in reduced-table mode. No gameplay performance measurement or new performance claim was made. Physical older/low-memory GPUs, other drivers, sustained descriptor pressure, effect visual acceptance and arbitrary-frame-rate gameplay retain the prior reports' acceptance limits. The `opt-beta6` and `hfr-beta6` worktrees were not edited or imported.

## Builds and tests

| Check | Result |
|---|---|
| `kk-dev-vulkan` incremental build | PASS |
| `kk-beta` incremental build, developer tools/tests off | PASS |
| Standalone native/resources/shaders builds | PASS |
| Development CTest | **7/7 PASS**, 1.17 s |
| Standalone native CTest | **33/33 PASS**, 176.93 s |
| Resources CTest | **2/2 PASS**, 1.10 s |
| Shader CPU CTest | **22/22 PASS**, 257.94 s |
| Shader GPU differential CTest | **1/1 PASS**, 117.21 s |
| Total | **65/65 PASS** |
| `git diff --check e890ece..HEAD` | PASS |

The final GPU differential test executes 1,761 shaders with zero translation/compilation failures and zero mismatches. It reports 337,856 invocations: 310,214 compared, 27,642 fragile and four nonterminating rounds skipped. Of 7,499,510 counted components, 348,958 are unknown, leaving 7,150,552 numeric comparisons. No test, seed, tolerance or comparison was changed in task 110.

Tests use the existing configured development, `out/task108/fixed-renderer`, `out/task108/fixed-shaders` and `out/task105/resources` build trees, rebuilt against the current source. The beta top-level configuration registers no tests with developer tools/tests off. Existing reduced-capacity and pacing/benchmark tests pass; the exact setup assignment and warmup UI trigger were checked in code and compiled, without a new game-level reproduction or a new direct integration fixture. Actual low-limit hardware remains untested here. GPU tests ran windowless on the RTX 4070 Ti; Khronos validation is unavailable, so this is not a Khronos-validation-clean claim.

Evidence is under `F:/KK-native-renderer/wt/beta6-int/out/task110/`: `build-*.log`, `dev-ctest.log`, `native-ctest.log`, `native-LastTest.log`, `resources-ctest.log`, `shaders-cpu-ctest.log`, `shaders-gpu-ctest.log`, `shader-gpu-LastTest.log`, `gpu-test-results.json`, `binary-checks.json`, `text-audit.json` and the private build/test/audit helpers. Test durations are functional evidence, not performance comparisons. Build-generated line-ending-only changes to the manifest and generated CMake file were restored after confirming their content diffs were empty and they had no other hard-link names.

## SDK, shader rebuild and package requirements for Claude

1. This worktree's `tools/rexglue-vulkan` junction still targets permanent `F:/KK-native-renderer/sdk-vulkan-upscalers-041`. Both game outputs' `rexruntime.dll` hashes match that install, and both export `RexSetPresentObserver096`. The three SDK source changes pass a read-only reverse-apply check against patch 0041. No SDK source, install, junction or `mkwt.ps1` was edited here.
2. Keep the complete SDK 041 pairing when preparing the test build. `mkwt.ps1` still defaults new worktrees to SDK 038; an ordinary new worktree will not automatically inherit 0041. The patch changes the D3D12 presenter's private layout, so do not mix its headers/consumers with the unpatched runtime.
3. Both outputs pass actual `llvm-strings` checks for `NVSDK_NGX` and `xess`. Both contain `nvngx_dlss.dll`, `libxess.dll`, `amd_fidelityfx_vk.dll`, `amd_fidelityfx_dx12.dll`, `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` and `dxcompiler.dll`, with no Xenos/Classic plugin DLL. These files, the existing glyphs/assets and pipeline list remain package inputs. New panel translations and memory shaders are compiled into the existing binaries; they add no loose runtime asset.
4. Translator version **5** requires both API shader packs to rebuild. Retain the executable shader-container extraction and all scale/motion/lighting variants. Do not distribute the private generated packs or game-derived data. No cache deletion was performed because this task is a review, with no build handover. Apply the standing handover checklist when a test build is actually prepared.
5. Static Linux review does not certify an AppImage or its native libraries. The Linux hold remains in force. The visible application version is still `2.0.0-beta.5`, as reported by task 105; no version bump or distribution was requested here.

Zero game launches, focus operations, screenshots, downloads/installations, push, merge or release. The shared slot was held only for the windowless native/GPU CTests after prior ownership ended, then released; other tasks' processes and priority files were left alone. The only task-110 edits outside this worktree are the requested central copy of this report and the task-owned shared-slot claim/release. No user's Desktop game, Documents data/cache, or other worktree was edited. Stop here for Claude's verification.
