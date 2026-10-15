# Task 091: optimisation round 2

Branch `opt-beta6`, starting at Beta 5 `e890ece`. Work in progress; nothing pushed,
merged, released or deployed.

Read the standing decisions, task, game rules, reports 063/067/068 and
083/084/085, and the Ally optimisation notes before changing code. Keep identical
pictures, draw readiness and the existing queue/presentation bounds. No game
logic, AI cadence, graphics defaults, interpolation or generation changes.

## Investigation

- Chapter-start conversion and uploads are the first priority. Conversion is
  currently synchronous in `GameRenderer::UploadTexture`, immediately before the
  first draw that uses it. Guest-memory lifetime and invalidation must remain
  correct when introducing workers.
- Beta 5 already contains the 085 device delta snapshots and command reuse.
  The old whole-device-copy hot spot is therefore not present in this baseline;
  remaining comparisons, allocation and queue signalling are being reviewed.
- Separate task-authorised worktrees review queueing and remaining per-draw
  overhead. Root coordinates measurements and all game runs (six maximum).
- The supplied junction pointed to permanent SDK 037. It was moved into ignored
  task output and replaced with a junction to permanent SDK 038 as the task asks.

## Validation

Baseline standalone native renderer built with Clang 19.1.5, Release, SDK 038:
**21/21 CTest entries pass** (80.54 s). The initial configure used a relative SDK
prefix and accidentally found the unrelated default SDK; its plugin link failed.
The corrected absolute prefix builds successfully. Only corrected-build results
are used. Baseline binaries are retained in ignored `kk/out/task091/baseline-bin`.

Short preliminary queue timings overlapped another task's compilation and cannot
support a gain claim. Extended alternating fixtures are ready; measurements wait
for all builds to finish. No game launched yet.

The first texture-worker design converts owned, swapped guest-memory snapshots
in bounded parallel slabs, then joins before the existing upload. This preserves
first-use readiness and command-list ownership. It does not by itself implement
cross-frame asynchronous GPU uploads or an upload budget: deferring a required
upload would also defer its dependent draws. Those parts remain open.

## Retained changes

- `62e5be1`: queue-bound, ordered-drain and wakeup regression coverage, plus a
  longer NR_BENCH queue fixture. The production queue experiment was reverted:
  seven alternating million-call runs gave 592 -> 576 ns/call, but the ranges
  overlap and the small difference is not reliable enough to retain the change.
- `cab47fa`: reuse byte-identical 944-byte draw constants within a command
  list/frame. New pixel coverage exercises changed constants and reopened lists;
  motion's existing invalidation remains. Seven alternating pairs measured the
  repeated-material fixture at 1590.7 -> 1534.6 us/frame without an upscaler and
  3062.5 -> 3011.4 us/frame with DLSS. This is 3.53% and 1.67% in synthetic CPU
  recording work, not a gameplay FPS claim. Ordinary alternating-material
  controls show no consistent change. Repeated-material uploads fall from
  320,400 to 800 across 400 frames, with every requested draw retained.

- `e6d46bd`: bounded workers convert owned guest snapshots in independent slabs.
  Extra workers are capped at three and respect process/thread affinity. The
  pool belongs to the renderer; all jobs join before GPU upload and first use.
  Small textures stay serial. Three alternating Windows pairs, whole-texture
  conversion time in us (before -> after):

  | Texture | Before | After | Reduction |
  |---|---:|---:|---:|
  | DXT1 1024, mips | 560.5 | 356.6 | 36.4% |
  | DXT5 1024, mips | 914.9 | 616.4 | 32.6% |
  | RGBA8 1024, mips | 6223.5 | 3112.9 | 50.0% |
  | 565 1024 -> RGBA8 | 8349.1 | 2898.0 | 65.3% |
  | Unaligned DXT5 -> RGBA8 | 4005.9 | 1634.1 | 59.2% |
  | RGBA8 cube 256, mips | 2597.6 | 1354.9 | 47.8% |
  | RGBA8 volume 128x128x16 | 1748.8 | 788.1 | 54.9% |

  Complete output hashes match. The small-texture control is 26.0 -> 26.4 us;
  no gain claimed. Candidate serial control is within about 1.5% of baseline.
  Windows resource CTest passes 2/2 (76 resource cases, three format-map cases);
  WSL Ubuntu 24.04 / GCC 13.3 resource CTest passes 2/2.
  Matched Linux baseline/worker benchmarks also pass with identical full hashes:
  large-case reductions are 16.3% DXT1, 33.7% DXT5, 44.9% RGBA8, 71.3% 565,
  70.6% decoded DXT5, 25.3% cube and 54.2% volume. Every large case favors workers
  in all three pairs. No small-case Linux timing claim because samples vary.

The combined native renderer/plugin builds successfully, including the added
diagnostic counter fields. Final Windows native CTest passes **21/21** (86.74 s),
including the new large-texture first-use/invalidation and draw-constant pixel
regressions. Linux native CTest passes **20/20** (71.38 s), using software Vulkan
with Khronos validation and the game renderer enabled. The Linux plugin fixture
needed its missing `native_debug` and `native_diagnostics` definitions:
`e1dc6c8` adds only those test definitions; production flags are unchanged. The
full plugin-enabled Linux suite passes, with no excluded test. Native temporal
upscaler SDKs remain disabled in that existing Linux SDK setup.

All **six** authorized Windows games completed with exit 0. No further game
launches. Released Beta 5 executable/runtime and the same private seed data are
used throughout; the candidate replaces only the native renderer DLL. Chapter 8,
stationary initial camera, windowed 1280x720, uncapped, render scale 1, GI Low,
GTAO full/three samples, upscaler off. Both SDL activation hints are zero and
kkbg2 remains running. Other tasks' locks and priority were respected.

Native frame intervals in the first ten seconds after the level-loaded marker:

| API / run | Frames >50 ms | Worst ms | Peak texture ms | Peak buffer ms |
|---|---:|---:|---:|---:|
| Vulkan before 1 | 2 | 206.3 | 109.4 | 38.6 |
| Vulkan after 1 | 2 | 169.5 | 64.0 | 38.4 |
| Vulkan before 2 | 3 | 191.4 | 90.1 | 46.3 |
| Vulkan after 2 | 2 | 204.6 | 74.0 | 52.4 |
| DirectX 12 before | 3 | 303.6 | 116.8 | 105.2 |
| DirectX 12 after | 2 | 269.2 | 96.5 | 101.7 |

Texture CPU time falls in both Vulkan comparisons, but **no consistent worst-
frame or steady-FPS gain is established**. Draw-constant reuse is about 37.2-37.3%
in the candidate games. Every run reports zero texture failures and zero skipped
draws in the measured windows. AI/scene workload and upload counts vary across
runs; these are local measurements, not release claims. Full steady results and
raw evidence paths are in the private coordinator report `codex/REPORT-091.md`.

## Remaining work

Priority 1 is partial: no asynchronous GPU upload system, ahead-of-use resource
scheduler or per-frame upload budget was added. Guest snapshot/endian work,
required conversion joins and GPU uploads remain synchronous. Upload-related
queue room waits still reach 100.2 ms in the repeated candidate Vulkan start and
189.6 ms in DirectX 12. Queue/presentation bounds remain exactly as before.
Buffer uploads, the remaining device comparisons, per-draw map/hash/mutex costs,
and identical-picture GI/GTAO GPU optimisation remain open. No game logic,
graphics defaults, effect quality or GPU shader changes were made.

# Task 097: chapter-start transfers and render-queue room

Starting at `176c9ef` on `opt-beta6`. Read DECISIONS.md, GAME-RULES.md,
reports 091/a/b/c and the preceding status before editing. Nothing pushed,
merged, released or packaged. Windows renderer-only work; no game logic,
AI cadence, shader, picture quality, graphics default or presentation bound changes.

The candidate records large first-use textures and vertex/index buffers into
an early graphics-queue transfer list, submitted after the draw is recorded and
before its frame list can execute. New-resource copies can start while the
remaining frame is recorded. Updated resources stay inline to preserve earlier
draws in the same list. The early path has a 32 MiB / 32 batch frame budget;
required over-budget and small uploads remain inline, never skipped/deferred.
CPU conversion and staging copies still run on the render thread. This is a
bounded early GPU transfer experiment, not a complete asynchronous CPU scheduler.

Queue instrumentation separates the existing 8,000-command bound from the
one-Present bound and measures retained queued command/snapshot bytes. Neither
bound is enlarged. Tests cover both blocked conditions, first-use transfer
contents and same-frame updates on Vulkan/D3D12, budget exhaustion and reopening.
Build, offline paired measurements and permitted game comparisons are pending.
Task 097 game launches so far: zero (maximum six).
