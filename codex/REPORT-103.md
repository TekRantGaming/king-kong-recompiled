# Task 103: cleanups from tasks 085-097

15 October 2026. Completed the four scoped investigations. Local changes are on `pace-beta6`, `hw-beta6` and the new `cleanup-beta6` branch. Nothing pushed, merged, released or packaged. Zero game launches.

Read `DECISIONS.md` first, then `TASK-103-cleanups.md`, `GAME-RULES.md` and reports 085-claude, 086, 095 and 096. No applicable AGENTS.md was found. No standing-decision conflict required skipping a task part. Vulkan/D3D12 policy, game logic, AI frequency, frame-rate defaults and graphics defaults are unchanged. No Linux work, downloads, installations, user game/save/cache access, focus operations or screenshots. The opt-beta6 and fx-beta6 worktrees were not edited.

## 1. Pacing SDK patch number

Renamed the pacing patch to `native-renderer/sdk-patches/0041-pacing-benchmark-present-observer.patch` on `pace-beta6`. Checked every available local/remote branch ref, including beta5, linux-beta4, hw-beta6, opt-beta6, vram-beta6, vkold-beta6, f8-beta6, hfr-beta6 and pace-beta6. Linux owns 0039/0040; 0041 was free. The renamed patch is byte-for-byte unchanged. The old file had one hard-link name and no junction/link dependency; its references were checked before the rename.

Updated all pacing-specific tracked references: `codex/REPORT-096.md`, `codex/build-presentation-096.ps1` and the unavailable-observer message in `kk/src/benchmark.cpp`. The Linux 0039 references and unrelated numeric values were retained. The build helper now records `sdk-patch.txt` beside its private SDK output; the existing private output has that record too.

**The patched SDK copy is `F:/KK-native-renderer/wt/pace-beta6/kk/out/presentation-096`.** Task 096 did not apply the pacing patch to a complete shared SDK tree. It compiled private presenter sources/header and the changed D3D12 header's consumers, then linked them against the read-only objects/libraries in `F:/KK-native-renderer/sdk-src-upscalers-038/out/build/win-amd64`. The worktree's `tools/rexglue-vulkan` junction still points to the permanent `F:/KK-native-renderer/sdk-vulkan-upscalers-038` install, which remains unpatched for pacing. Integration still requires applying 0041 and building a paired SDK.

Validation: `git apply --check` against the -038 source passes. Applied 0041 to task-owned copies of its three source files under `pace-beta6/kk/out/task103/sdk-check`; each matches the corresponding private SDK file after line-ending normalization. Private `rexruntime.dll` and the DLL beside kk-beta both retain SHA-256 `771F33D5B1C99DF7F99CCDD8272EAB0F08E44C02CEE7837F4205E4C8D78A0A4B`. The updated benchmark source compiles; the existing kk-beta executable was relinked to refresh its patch-number message. No game was started. PowerShell parsing and `git diff --check` pass.

Local commit: `a0d5020` (Renumber the pacing SDK patch to 0041).

## 2. Shader differential failure: proven pre-existing

Built a control from an archive of released Beta 5 `e890ece` inside `cleanup-beta6/out/task103/beta5-src`. The raw Beta 5 Windows GPU-test harness cannot run as written: it lacks `<bit>` and requests invalid zero-sized line buffering. Applied only the two startup corrections from `bd4fb7f` to this private control. Audited all 61 archived shader/XenosRecomp files with Git text normalization: those are the only two changed files. No extraction, scheduling, translator, interpreter, prelude or Vulkan-harness changes were included.

Ran the control and a fresh hw-beta6 rerun, sequentially, on the RTX 4070 Ti with the same Release Clang build, DXC 1.9 and default seeds/counts:

| Result | Beta 5 control | hw-beta6 |
|---|---:|---:|
| Interpreter CTest | Pass | Pass |
| Synthetic shaders | 1,757 | 1,757 |
| Translation/compile failures | 0/0 | 0/0 |
| Invocations | 337,088 | 337,088 |
| Components compared | 7,506,659 | 7,506,659 |
| Component mismatches | 2,319 | 2,319 |
| Shaders with mismatches | 84 | 84 |
| Groups: texture-fetch / random / fuzz | 63 / 20 / 1 | 63 / 20 / 1 |

All 279 printed mismatch/result lines match exactly, including values and invocation locations. GPU CTest elapsed times were about 111 seconds per side. Task 095's additional-container extraction did not cause this failure, so no shader-semantic fix was made on hw-beta6. Its report was amended with this control and cause.

One confirmed cause is the reference model's handling of fetched subnormals. For `vs 1D predicated (bare microcode)`, round 0, invocation 1, the reference fetches `2.304716e-41`, gives it zero error and treats `x != 0` as true without marking the invocation fragile. The GPU float comparison flushes that value to zero and skips the predicated fetch, leaving the exported z at zero instead of one. In an isolated diagnostic, replacing only that synthetic HLSL predicate with `(asuint(ka_) & 0x7FFFFFFFu) != 0u` changes **31 mismatches to zero**, with the same 192 invocations. The interpreter already allows flushing for arithmetic results but does not apply that uncertainty to this exact fetched input before a predicate.

This establishes a concrete pre-existing cause of the failing test. It does not classify every other filtering/random mismatch. For example, the retained 2D filtering trace has reference `0.7315643 +/- 0.00318` versus GPU `0.736938477`; that separate difference was not fixed or conclusively attributed here. The synthetic GPU CTest still fails, and task 095's all-CTest gate remains unmet. Khronos validation was unavailable for these differential runs. No relaxed tolerance, disabled comparison or production HLSL change was committed.

Local commit: `cf7d234` on hw-beta6 (Document the Beta 5 shader differential failure control).

## 3. Tiling test: present in Git and builds successfully

Created `cleanup-beta6` using exactly `codex/mkwt.ps1 -Name cleanup-beta6 -Base beta5`, at `e890ece`. Verified its asset, generated-code, SDK and upscaler junctions point at permanent locations.

`native-renderer/resources/tests/test_tiling.cpp` is present in the fresh checkout: 8,453 Windows bytes. Its Git blob is `482697f82334acc009acf6ba7b94fd32a5ac03ff`, introduced by `2621b9c` and unchanged in all 104 inspected refs containing the file, including beta5 and linux-beta4. The all-branch history contains no deletion of this path. The current linux-beta4 Windows checkout also contains it. REPORT-085's missing file therefore does not describe a loss from Git; the old local Linux build/staging state was not recreated.

Retained the valid CMake entry and existing test source. No restoration or CMake removal was necessary. A fresh Windows resources configure/build succeeds and **2/2 CTests pass**, comprising 71 resource checks and 3 NVRHI-format checks. The tiling/address permutation and known-point checks execute successfully.

## 4. Live-switch crash: reproduced and test-host race fixed

The first 20 full `nr_game_upscale_tests` CTest invocations passed. A dependency audit then found that standalone defaults selected the downloaded FSR Vulkan DLL, while task 086 used the project's patched FSR copy. Corrected the reproduction helper to use `tools/rexglue-vulkan/fidelityfx`, as the Beta 5 build does. All six upscaler DLL hashes now match the existing Beta 5 dev-test directory; DXC is loaded from the same permanent 1.9 release.

With those matched dependencies and otherwise unchanged Beta 5 test sources, **2/20 full invocations failed**, repetitions 6 and 17. Both ended after DLSS Quality preset K had been installed/evaluated, before XeSS initialization completed, with no failed-check message. Windows recorded `ucrtbase.dll+0xA527E`, exception `0xC0000409`, fatal-app-exit parameter 7. This matches the original task 086 crash at 07:29:10, PID 11040. The new repetition-6 crash was at 11:53:53, PID 24456.

A temporary terminate/abort diagnostic then captured the underlying exception: **`vk::Device::getSemaphoreCounterValue: ErrorDeviceLost`** at that transition. Of 12 completed diagnostic invocations, 11 passed and one exposed this exception. Invocation 13 hung for over 90 seconds; stopped only that task-owned script/test/wrapper tree and released its synthetic-test lock after the processes exited. The diagnostic handlers are absent from the final sources.

The test device had no `SetUpscalerExclusive` or `SetUpscalerIdleWait` registration. Consequently, the XeSS worker's `UpscalerRunExclusive` fallback merely waited for an idle GPU, then let SDK queue use overlap new render/readback submissions. The game's Vulkan host already holds its queue/execution locks across this entire operation; the test host did not provide the same contract.

Fixed only the test host on cleanup-beta6:

- Register the existing idle/exclusive callbacks for an upscaler-capable test device, and clear them before device destruction.
- Use one mutex across SDK initialization, command submission, idle/garbage collection and the direct submission inside readback.
- Add `GameUpscale_SdkInitialisationOwnsTheQueue`, which verifies an SDK worker cannot enter while another thread owns the queue.

The new regression **fails when callback registration is temporarily removed**, and passes with it restored. After the final fix and removal of temporary diagnostics, **20/20 full upscaler CTest invocations passed**, 5.19-6.02 seconds each, with 16 registered cases per invocation (the optional CPU benchmark remains skipped). This exercises 540 live-switch states across those runs. The other affected test programs were rebuilt; **8/8 renderer CTests passed**, including the upscaler SDK test, in 26.75 seconds. NVRHI validation reported no errors; Khronos validation is not installed. This addresses the reproduced standalone-host race; it is not a claim of exhaustive driver/device-loss or gameplay validation.

Local implementation commit: `202fd01` (Serialize SDK initialisation in the Vulkan test host). Production renderer/SDK behavior was not changed by this fix.

## Evidence and reproduction

Primary evidence: `F:/KK-native-renderer/wt/cleanup-beta6/out/task103/` (ignored, not committed).

- `patch-number-audit.txt`, `tiling-branch-blobs.txt`, `tiling-deletions.txt`, resources build/test logs.
- `shader-control-source-differences.txt`, `shader-control-01.log`, `shader-hw-01.log`, corresponding CTest transcripts, `cpu-predicate.log`, `cpu-filter.log`, `predicate-original-01.log`, `predicate-bits-01.log` and the synthetic HLSL probe.
- `upscale-summary.json` for the initial DLL variant; `upscale-beta5-dlls-summary.json` and all 20 individual matched-baseline transcripts; `upscale-diagnostic-*`, `diagnostic-hang-stopped.txt`, original failure/event records; `queue-control-unregistered-01.log`; `upscale-queue-fixed-summary.json` and all 20 final transcripts; `renderer-affected-01.log`.
- `upscale-dependency-hashes.txt` records the final matched DLLs. Diagnostic executables/source and CPU probe remain private under this evidence root.

Committed helpers, invoked with `powershell.exe -NoProfile -ExecutionPolicy Bypass -File` from cleanup-beta6:

```text
codex/build-103.ps1 -Target renderer
codex/run-103.ps1 -Test upscale -Label repeat -Count 20
codex/build-103.ps1 -Target resources
codex/build-103.ps1 -Target shader-control
codex/run-103.ps1 -Test shader-control -Count 1
codex/run-103.ps1 -Test shader-hw -Count 1
```

The GPU helper respects user locks, other tasks' priority reservations and running games, holds the shared slot only around offscreen tests, and releases it afterward. All task-103 tests finished and their slot was released. The final lock read showed task104 ownership, which was left untouched. No task-103 game process was created. No player build was handed over, so packaging and owner-cache deletion were not performed. Final scoped diffs pass `git diff --check`; the three edited worktrees are clean after their local commits. This report is also copied to `F:/KK-native-renderer/codex/REPORT-103.md`. Stop here for Claude's review.
