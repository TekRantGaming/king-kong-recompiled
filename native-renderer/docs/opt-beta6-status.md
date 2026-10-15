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
  WSL Ubuntu 24.04 / GCC 13.3 resource CTest passes 2/2. No Linux timing claim.

The combined native renderer/plugin builds successfully, including the added
diagnostic counter fields. Final combined CTest remains pending. One of the
allowed six Windows games has run: released Beta 5, Vulkan, chapter 8. It confirms
109.4 ms texture work and 38.6 ms buffer work in individual startup frames; first
10 seconds after level load have two frames above 50 ms, worst 206.3 ms. No
texture failures or skipped draws. The comparison uses private roots, windowed
1280x720, uncapped, render scale 1, GI Low, GTAO full/three samples, upscaler off.
Other tasks' game locks are respected between runs; remaining comparisons wait
for the shared slot. These are local measurements, not release claims.
