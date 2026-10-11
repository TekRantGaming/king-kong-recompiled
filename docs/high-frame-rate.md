# Playing above 30 FPS

King Kong was made for 30 frames a second on the Xbox 360. The port runs it at any frame rate
(`kk_frame_rate`: 30, 60, 90, 120, 144, 165, 240 or 0 for unlimited). Most of the game steps itself with the
time since the last frame, so it keeps the right speed at any rate. This page lists the systems that do not,
how they were found, and how each one is fixed (`kk_hfr_fixes`, on by default; at exactly 30 FPS every fix
is off, so the game runs as on the console and the golden test frames stay the same).

The user's rule for fixes: no frame interpolation. Every frame-rate-dependent system is fixed in the game
logic, preferably by making it truly variable-step (scale by the frame time) or by running it at a higher
fixed rate. Holding a system at 30 Hz is acceptable only for things that are not visible motion (AI
decisions, timers, spawners), and is noted where used.

## How the game keeps time

`sub_8272CA20` runs once a frame. It reads the kernel tick count in milliseconds (`sub_82106C08`), takes the
difference from the last frame, clamps it to 0.001 .. 0.063 s, stores it unscaled at `0x82C4EDBC` and,
multiplied by the game's time scale (`0x828D178C`, 1.0; slow motion changes it), at `0x82C636E0`, the frame
time almost every system reads (`lis r,-32058` then `lfs f,14048(r)`; 889 functions). A third copy sits at
`0x83090314`. If the current world's byte `+2444` is non-zero the frame time is instead that many 1/60 s (a
fixed-step mode; nothing in the code writes it, so it would come from level data; not seen used).

Consequences:

- The tick count is in whole milliseconds, so at 30 FPS the frame time alternates between 33 and 34 ms, and at
  high frame rates it is coarse: at 240 FPS a frame is 4 or 5 ms, at 500 FPS 2 ms, never under 1 ms.
- Above 63 ms a frame (under about 16 FPS) the game slows down instead of taking bigger steps.

The engine's per-world call (`sub_82881B70`, once a frame per active world) runs, in order: the event tracks
(animations; `sub_8271CB00` -> `sub_8271C1C8` -> the key player `sub_8271BA18`), the AI of every active
object (`sub_8271D498` -> the track scheduler `sub_8271D1D8`), the hierarchy (`sub_82853C88`), then dynamics,
collision, the views and two more AI groups (`sub_8271D3C8(2)`, `(3)`).

Jade's AI runs every track of every active object once a frame unless the track is waiting on its timer
(`+44 + 4 * track`, lowered by the frame time). Track functions are the compiled scripts (`+20` of the track's
function block; names come from the script tables in the executable, `{hash, function, name}`). A script that
assumes one call per 1/30 s is frame-rate dependent; one that uses the frame time is not.

## Tools

| Tool | What it does |
|---|---|
| `KK_DEV_SAMPLE` (`kk/src/dev_sample.cpp`, dev builds) | Logs the world's active objects (position, first event track, first AI track, file key) every 50 ms of real time and every frame's time, so runs at different frame rates can be compared |
| `KK_DEV_SAMPLE_GLOBALS` | Also logs chosen guest addresses every frame |
| `KK_DEV_SAMPLE_DUMP` | Raw bytes of every active object and what it points to, to find fields |
| `tools/hfr_compare.py` | Compares two sample logs on the level's own clock: position differences, event track speed and restarts, AI function changes |
| `kk-prof` build, `KK_DEV_PROF_AT` | Calls per function in time windows; the same windows at 30 and 60 FPS show which functions run per frame and which run per event, and events whose rate changes with the frame rate |

Objects are matched between runs by their file key (the BIG key of the loaded object, found by reversing the
loader's key -> index -> address tables at `0x82C4ED94` and `0x82C4EDC0`), which is stable between runs even
though the heap layout is not.

## Catalogue

Status: **fixed** (in `kk/src/hfr_*.cpp`, verified as noted), **found** (in the code, not yet fixed or
measured), **ok** (looked like a suspect but is frame-rate correct).

### Families found in the code

These were found by scanning the recompiled code for how the frame time is used (scripts in
`F:\KK-native-renderer\analysis\hfr`, not in git; the method is below) and then reading each site.

| System | Functions | What goes wrong above 30 FPS | Status |
|---|---|---|---|
| Camera shake | `CM_Sfx` `sub_8245EC90`, `SCC_SFX` `sub_82553F20` | Fall-off multiplied once a frame: a strong shake never ends | fixed in v1.9.10 (`camera_shake.cpp`) |
| Particle drag | `sub_82862460` (every particle generator: rain splashes, sparks, spray, smoke) | Drag (`+48`) applied once per 1/60 s sub-step plus once a frame: 40 times a second on the console, 60 at 60 FPS, 120 at 120, hundreds unlimited. Particles stop short and drop straight | fixed (`hfr_particles.cpp`) |
| 0.016 s floor | `k_ETAT_grab` `sub_825CA350`, `k_ETAT_grab_mashing` `sub_8259D998`, `k_ETAT_finished` `sub_825B3288`, `k_ETAT_cine` `sub_825B62E0`, `KT_ETAT_Finish` `sub_824876F0`, `KBC_ETAT_KK_Finished` `sub_8252B1D0`, `BC_exec_MOVE_beziers` `sub_8263E1B8` | The frame time is clamped to 0.016 .. 0.048 s before use, so above 62.5 FPS these run fast (1.9x at 120 FPS, 3.8x at 240): Kong's grabs, button mashing and finishers, the BC creature's path moves | fixed (`hfr_dt_floor.cpp`) |
| Decay per 0.016 s | inlined in `BC_TAG_On` `sub_8263C930`, `KBC_TAG_On` `sub_82512FA8`, `KBC_ETAT_Fight_BB_Poursuit` `sub_8252E1E8`, `k_ETAT_grab_ANN` `sub_8259F698`, `CM_Kong_Mode_Swing_Horiz` `sub_826245B8`, `k_ETAT_grab` (five more); helper `sub_8261AD78` | `f = k; while (t > 0.016) { t -= 0.016; f *= k; }`: one factor a frame plus one per extra step, 90 a second on the console, 120 at 120 FPS, 240 at 240. Knock-back slides and Kong's swing camera stop sooner | fixed except `k_ETAT_grab`'s own (`hfr_step_decay.cpp`) |
| Follow smoothing in 1/300 s steps | `sub_828271E8` (from `sub_82827B10`, a render-time modifier) | `ceil(dt * 300)` steps a frame: 315 a second at 30 FPS, 360 at 120, 432 at 144, 480 at 240, 1000 at 1000 FPS. Follows its target faster at high rates | found |
| Raft | `Raft_exec_compute_position` `sub_825EFE78` | A fixed 0.016 s step from an accumulator: right speed at any rate, but moves in 62.5 Hz steps | ok (note) |
| Animation frame equal to 0 | `PNJInd_State_Attack` `sub_826F3BE8` | Tests `frame == 0` of the attack animation (frames in 1/60 s): seen on one game frame at 30 or 60 FPS, on two at 120 | found |

Looked at and frame-rate correct: the dt pipeline itself, the event (animation) key player `sub_8271BA18`, the
particle spawner `sub_82862B10` (a time accumulator), the 1/30 s sub-stepper `sub_827C8C20` (its damping uses
`pow`), `Chaloupe_Rotation` `sub_8242ECA0` (the lifeboat; lerps with factor `dt`), the menu key repeat in
`IntMIG_loop`, the AI track timers in `sub_8271D1D8`.

## Fixed

- Camera shake (v1.9.10, `kk/src/camera_shake.cpp`): `CM_Sfx` (`sub_8245EC90`) and `SCC_SFX`
  (`sub_82553F20`) multiplied the shake's fall-off rate by a constant once a frame, so above 30 FPS a strong
  shake never ended (#39). The rate now falls by `mult ^ (dt * 30)`.
