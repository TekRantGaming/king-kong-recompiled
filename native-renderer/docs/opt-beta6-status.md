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

Branch `opt-beta6`, starting at `176c9ef`. Implementation `3338252`.
Read DECISIONS.md, GAME-RULES.md, reports 091/a/b/c and this status before
editing. Nothing pushed, merged, released, packaged or deployed. No game logic,
AI cadence, frame cap, shader, quality, graphics default or presentation bound
changed. No Linux work or shared SDK/link change.

Large first-use textures and vertex/index buffers can use a reusable early
graphics-queue transfer list on both APIs. Each draw's resource bindings form
a batch; it is submitted before its graphics frame list. Queue ordering and
explicit texture state tracking guarantee first-use contents. The early path
has a 32 MiB logical-payload / 32 batch frame budget. Small, over-budget,
dynamic and in-place updates remain inline and mandatory, preserving earlier
reads in the same frame. No placeholder, skipped draw or delayed dependent draw.
CPU conversion, resource creation, staging copies and cache/watch work remain
on the render thread: the broader asynchronous CPU upload goal is partial.

Windows Release / Clang 19.1.5 / SDK 038 builds. **All 22 Windows CTest entries
pass (69.75 s)**, including both-API upload contents/order/budget/reuse checks,
real first-draw vertex/index pixels and same-frame invalidation, the large tiled
texture's first-use/reupload pixels, and queue-bound/fence/wakeup coverage.
No Khronos validation layer is installed; NVRHI validation is used.

Five alternating synthetic 16 MiB upload pairs complete in 1.443 -> 1.055 ms
on Vulkan and 1.647 -> 0.983 ms on DirectX 12. CPU recording/submission increases
0.092 / 0.200 ms respectively. This is a GPU transfer-overlap tradeoff, not a
CPU upload-time or FPS gain. Five paired queue fixtures measure 527 -> 537
ns/call with overlapping ranges; no queue-throughput gain is claimed.
The existing graphics-list GPU timer excludes the earlier transfer lists;
the synthetic completion interval above includes them.

Exactly **six** authorized games, all exit 0, chapter 8, same private Beta 5
executable/runtime/cache seed as 091; candidate replaces only the native DLL.
Windowed 1280x720, both SDL activation hints zero, kkbg2 running, uncapped,
render scale 1, GI Low, full GTAO/three samples, upscaler off. No user data,
user game or screenshot touched. The shared priority reservation was moved
aside after the runs; no task-owned game remains. No further task-097 launch.

| Run | First-10s frames >50 ms | Worst startup ms | Peak room ms | Steady FPS |
|---|---:|---:|---:|---:|
| vk-before-1 | 4 | 214.0 | 134.1 | 175.07 |
| vk-after-1 | 2 | 181.9 | 97.9 | 203.64 |
| vk-after-2 | 2 | 163.4 | 83.0 | 201.90 (loaded) |
| vk-before-2 | 2 | 177.6 | 98.8 | 205.50 |
| dx-before | 3 | 314.8 | 222.3 | 110.55 (loaded) |
| dx-after | 2 | 277.8 | 197.7 | 110.58 (loaded) |

The second Vulkan candidate's steady segment overlaps compilation; both DX12
steady windows also contain sampled background activity, with one sample in
the DX12 candidate's startup window. The first pair predates activity sampling.
These are raw local intervals, not a controlled FPS gain. Comparison to Beta 5
includes task 091's improvements too, so it cannot isolate task 097's effect.
Every measured window has zero skipped draws/startup pipeline-wait time and
all summaries have zero texture failures. Full results: codex/REPORT-097.md.

Every candidate reports zero command-capacity wait: all room waits are at the
one-Present bound. Peak retained queued command storage is 5.0-5.8 million bytes,
not the 8,000-call capacity. Changing command count to bytes cannot remove this
Present wait; neither queue/presentation bound is enlarged. Early transfers total
64,721,525 bytes in 94 batches in each candidate. No overall FPS gain is established.

Remaining: synchronous CPU conversion/creation/staging/cache work and required
in-place uploads. DX12 buffer work still reaches 101.3 ms and Present room waits
197.7 ms. There is no ahead-of-use resource scheduler, CPU staging worker or
hard budget on all mandatory uploads. The early GPU-transfer benefit does not
establish that these remaining chapter-start CPU stalls are fixed.

# Task 099: render-thread chapter-start CPU work

Branch `opt-beta6`, starting at `0fe206a`. Read DECISIONS.md first, then
GAME-RULES.md, reports 097 and 091, and the status above. Work is limited to
resource CPU scheduling, caches, profiling, regression coverage and measurements.
Both queue limits and the one outstanding Present allowance stay unchanged.
No game logic, graphics defaults, shader or quality changes; no Linux work,
push, merge, release, package or owner build handoff.

Investigation has started with opt-in per-stage resource CPU timing on both
Windows APIs. The six-launch allowance is shared across profiling and final
Beta 5 comparisons; no task-099 game launched yet. Existing conversion workers
join before upload; resource creation, staging, hashing and watches are still
synchronous at this point. Changes will preserve first-use readiness and the
ordering of in-place rewrites.

Task-099 profiling completed two chapter-8 Windows launches, both exit 0,
using a diagnostics-only build of `0fe206a` and the task-097 method. Four game
launches remain. Detailed opt-in CPU attribution is under `kk/out/task099`.
During the first ten seconds, Vulkan conversion/texture creation/staging total
81.2/15.2/37.0 ms, buffer creation 30.3 ms; DirectX 12 totals
80.4/43.8/62.6 ms, buffer creation 100.5 ms. Largest one-frame conversion is
38.4/37.6 ms; buffer creation is 23.6/76.8 ms. Buffer staging also dominates
the ten-second aggregate (690.2/424.0 ms), largely recurring work; its frame
peaks are 7.7/11.5 ms. Cache-bind totals include nested hash/watch work and
must not be added to those counters. Background activity is recorded.

Implementation in progress: lazily prepare first-use resources in bounded,
joined worker batches, reading compact owned snapshots; create resources,
convert/hash and record worker-owned upload lists, then submit/publish on the
renderer before dependent draws. NVRHI D3D12 recording counters require
serialized worker list operations; allocation/conversion can overlap. Required
in-place updates stay at their original draw-list positions. Reuse dirty-check
hashes until invalidation, coalesce contiguous watched ranges, retain conversion
output/snapshot/staging allocations. Tests and performance evidence are pending;
no claim yet that chapter-start stalls are fixed.

Task-099 candidate validation: all **22/22 Windows native CTests** pass
(70.95 s), plus **2/2 resource CTests** (3.59 s). New coverage checks both APIs'
worker-created buffer bytes, captured staging after caller input changes,
exception joining, reuse with GPU work in flight, zero-worker fallback, scratch
capacity, real texture+vertex first-use pixels and same-frame rewrites. A write
while arming a snapshot's watch forces discard and synchronous retry. Every
format/shape comparison also checks compact snapshot bytes/metadata/hashes;
truncated snapshots fall back to the original converter. No validation-layer
certification is claimed on this Windows installation.

Four clean alternating 16 MiB resource fixture pairs (eight buffers + eight
tiled RGBA textures; seven warmups and 21 timed iterations) give median CPU
recording/submission 8.644 -> 5.032 ms Vulkan and 11.302 -> 5.810 ms DirectX 12.
Including GPU completion: 9.458 -> 5.816 ms and 12.017 -> 6.488 ms. Serial
controls retain the existing texture slab workers. Creation and conversion are
inside this timer; external guest snapshots and write watches are not. Every
readback passes. The fifth pair overlapped compilation, is labelled loaded and
excluded from the clean medians. These are synthetic gains, not gameplay FPS.

The caller participates in a batch with at most three affinity-limited workers.
Workers own their upload lists; their list operations serialize for NVRHI's
non-atomic D3D12 recording counters. All jobs join before render-thread
submission/publication. Both old early-transfer bounds (32 MiB / 32 batches per
frame) remain shared with the worker path. Owned input snapshots are also
limited to 32 MiB per draw and retained CPU scratch to 64 MiB; NVRHI retains
in-flight staging until GPU completion. Small/over-budget/existing resources
use the original mandatory path. First-use scheduling is per draw; it does not
run a cross-frame asset streamer or remove required CPU joins/in-place updates.
Final Beta 5 chapter comparisons are in progress within the six-run allowance.

Task-099 background-audit correction: the full process CPU inventories show an
external `kkshaders` compiler consuming roughly eight logical CPUs during all
five original fixture pairs and both Beta 5/candidate Vulkan games. The earlier
filter watched clang/ninja/cmake/CTest, so its four "clean" fixture labels were
wrong. Their timings remain recorded as loaded observations; the clean-gain
claim above is withdrawn pending isolated offline repeats. Both Vulkan startup
AND steady windows are loaded. The DirectX pair and the two original profiling
runs have no sampled build/test/shader-compiler overlap. All six games exit 0;
no additional game launch is permitted or planned. Offline repeats wait for
`kkshaders` and other builds/tests as well as the shared slot. Correctness-test
passes are unaffected by this performance-label correction.


## Task 099 final results (supersedes the earlier provisional fixture labels)

Implementation `d31ae06`; background correction `6aa32b4`. All **22 native + 2
resource Windows CTests pass**, with no exclusions; 70.95 s + 3.59 s. Production
code has not changed since those checks. No Linux work, shader/quality/default,
game logic, AI cadence, frame cap, queue/Present allowance, host backend or shared
SDK changes. Nothing pushed, merged, released, packaged or deployed.

Five NEW alternating fixture pairs on each API have no sampled game/build/test/
shader-compiler overlap, and no other process consuming over 0.5 CPU-seconds
between their process inventories. These are the authoritative offline results:

| API | Serial -> batch CPU record ms | Reduction | Serial -> batch completion ms |
|---|---:|---:|---:|
| Vulkan | 6.641 -> 3.997 | 39.8% | 7.420 -> 4.757 |
| DirectX 12 | 9.027 -> 4.509 | 50.0% | 9.728 -> 5.197 |

Same synthetic 16 MiB creation/conversion/staging fixture and correctness checks
as above. The original five pairs are ALL loaded by `kkshaders`, retained and
excluded. No gameplay FPS gain is inferred from this fixture.

Exactly **six** task-099 games, all exit 0, all shorter than two minutes. The
first two profile `0fe206a` with added timing only; the final four compare the
released Beta 5 native DLL with the candidate using the same Beta 5 executable,
runtime, private data/cache seed, chapter 8 and settings as task 097. Windowed
1280x720, both SDL activation hints zero, kkbg2 running, no screenshots or user
files/caches touched. The six-launch limit is exhausted; no further task-099 game.

| Run | Startup >50 ms | Worst startup ms | Peak Present-room ms | Steady FPS | Steady p99 ms |
|---|---:|---:|---:|---:|---:|
| vk-profile | 2 | 175.8 | 99.9 | 205.54 | 9.13 |
| dx-profile | 2 | 273.3 | 193.3 | 110.61 | 9.22 |
| vk-before (loaded) | 5 | 239.4 | 139.4 | 170.87 | 9.14 |
| vk-after (loaded) | 2 | 196.6 | 114.9 | 73.46 | 58.04 |
| dx-after | 2 | 254.7 | 176.3 | 110.58 | 9.20 |
| dx-before | 3 | 282.0 | 201.2 | 111.07 | 9.35 |

Both Vulkan comparison startup and steady windows overlap external shader/test
work, with compilation also observed during the candidate. Their raw timings
cannot establish an isolated gain or regression. The DirectX pair has no sampled
build/test/shader-compiler overlap: startup improves in this single pair, while
steady FPS is effectively unchanged. It is not a repeated or cold-cache result.
The Beta 5 comparison also includes earlier tasks 091/097; the two profiling
runs give an unrepeated pre-099 branch reference. All measured windows have zero
skipped draws/pipeline wait and all run summaries have zero texture failures.

The candidate publishes 97 resources (6 discarded attempts) on Vulkan and 92
(9 discarded) on DirectX. Joined preparation totals 111.0/112.8 ms over the run;
first-ten-second wall time is 62.7/59.8 ms, plus 9.9/8.2 ms of caller snapshot
work and the separately measured watches. Worker stage timers are summed elapsed
work, not additional render-thread wall time; the old texture/buffer diagnostic
counters omit prepared work and cannot be used alone to claim CPU savings.

Present room remains as high as 176.3 ms on DirectX. First-use scheduling is
bounded within a draw and still joins; capture/watch and required existing/
dynamic updates stay on the renderer. More worker lists can exhaust the existing
early batch allowance sooner, in which case uploads remain mandatory inline.
No full-game pixel equality, Vulkan steady gain, elimination of chapter-start
stalls, cold driver-cache, owner 4K or Linux/Ally certification is claimed.
The corrected full process audit is retained; the game's per-sample CPU deltas
were quantized by a PowerShell overload, so absence of a recorded small delta
does not certify an idle PC. Clean fixture inventories retain unrounded CPU.

Both task-owned priority reservations were moved aside after use, the task's
shared lock was released, and no task-099 game remains. No more work is scheduled.
Full coordinator report: `F:\KK-native-renderer\codex\REPORT-099.md`.
Evidence: `kk/out/task099/bench-clean-summary.json`, `game-final-summary.json`,
`resource-summary.json`, all raw fixtures/process inventories, run scripts,
build/CTest logs and `artifact-verification.json`. Candidate SHA-256:
`614d0013bb2ec00ab1bcf3dfcd0d8844e62edd840d603631f2b84e3d1cc60aec`.
