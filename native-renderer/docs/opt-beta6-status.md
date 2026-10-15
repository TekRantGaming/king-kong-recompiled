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
