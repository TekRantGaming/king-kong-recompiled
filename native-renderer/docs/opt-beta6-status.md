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
