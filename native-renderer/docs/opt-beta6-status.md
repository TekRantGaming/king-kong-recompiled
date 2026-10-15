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

Baseline standalone tests, offline benchmarks and Windows builds pending. No
game launched. Measurements and limitations will be recorded here as work lands.
