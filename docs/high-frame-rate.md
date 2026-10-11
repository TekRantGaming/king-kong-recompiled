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
| Motion blur strength | `sub_82768DE8` (after effect; only with `kk_motion_blur` or the Original look) | Its blend weight is `s * (0.6 + 0.4 * frame_ms / 30)`, `frame_ms` being the device's measured frame time (`0x82D623E0`): 1.04 s at 30 FPS, 0.72 s at 110. For the same trail length in seconds a weight for the previous frame would have to rise with the frame rate (`a ^ (30 / fps)`), so the trail is about 3x shorter at 110 FPS. Whether the shader's weight is the previous frame's or the new one's decides the right formula; not changed until that is checked on screen | found |
| Raft | `Raft_exec_compute_position` `sub_825EFE78` | A fixed 0.016 s step from an accumulator: right speed at any rate, but moves in 62.5 Hz steps | ok (note) |
| Animation frame equal to 0 | `PNJInd_State_Attack` `sub_826F3BE8` | Tests `frame == 0` of the attack animation (frames in 1/60 s): seen on one game frame at 30 or 60 FPS, on two at 120 | found |

Looked at and frame-rate correct: the dt pipeline itself, the event (animation) key player `sub_8271BA18`, the
particle spawner `sub_82862B10` (a time accumulator), the 1/30 s sub-stepper `sub_827C8C20` (its damping uses
`pow`), `Chaloupe_Rotation` `sub_8242ECA0` (the lifeboat; lerps with factor `dt`), the menu key repeat in
`IntMIG_loop`, the AI track timers in `sub_8271D1D8`.

## Measurements

Runs on this PC (Xenos plugin, `kk-dev` build, private copy of the test save). The PC never goes above about
110 FPS, also uncapped (a driver limit), so "120" and "unlimited" runs are 110 FPS; behaviour at 250 FPS and
more was tested with `KK_DEV_FORCE_DT=0.004` (every frame counts as 4 ms; the game runs slower than real time
but each system steps as it would at 250 FPS). Raw logs, samples and captures are in
`F:\KK-native-renderernalysis\hfr
uns` (not in git).

### Venture opening (chapter select entry 0)

- Frame pacing: 30 FPS cap 29.9 FPS (frames of 32 to 34 ms), 60 cap 59.7 (15 to 18 ms), 120 and unlimited
  109.5 (7 to 11 ms). The game clock (sum of frame times) matched real time within 0.3% in every run.
- 65 active objects. The nine rowers (`H_CINE_WaitDefault`, actions 272 and 280) and the boat moved 120 units in
  75 s; their positions over game time differ by at most 0.13 units (RMS) between 30 and 110 FPS, a phase shift
  of their bobbing, and the camera by 0.02.
- The rowers' animation (first layer, read with the game's own `sub_823F8AF0`): the rowing cycle (3.08 s) runs at
  0.997 to 1.005 times real time at 30, 60 and 110 FPS, loops the same 23 times in the window and never changes
  action. So in this part of the opening the rowing clock itself is frame-rate correct; the skip players see
  must come from the later rowing section (crew in `H_ETAT_IA_suivi`, from the main menu's Play) or from the
  pose rather than the clock. Still open, see "Left to do".
- Call counts (kk-prof, the same three 10 s windows at 30 and 60 FPS): every script function ran exactly twice
  as often at 60 (all per frame). Engine functions that did not double: the random number generator (x1.52) and
  a few callers; the camera's aim-assist object test `sub_8270AF70` ran 41 times a second at 60 and never at 30,
  in a scene without stick input (not explained yet).

### Particle drag (fix verified)

Kong to the Rescue (chapter 27), fires and embers; `KK_DEV_HFR_TRACE` logs the mean speed of all live particles
of generators with drag (all 0.95 per step here):

| Run | Mean particle speed (several 1 s samples) |
|---|---|
| 30 FPS | 0.91, 1.41, 1.21, 0.92 (mean 1.11) |
| 110 FPS, fix off | 0.54, 0.52, 0.66, 0.45, 0.61 (mean 0.55) |
| 110 FPS, fix on | 1.16, 1.25, 0.91, 1.21, 1.31 (mean 1.17) |
| 4 ms frames (250 FPS), fix off | 0.35, 0.16, 0.32, 0.16, 0.35 (mean 0.27) |
| 4 ms frames (250 FPS), fix on | 1.23, 0.96, 1.39, 1.00, 1.42 (mean 1.20) |

### Kong to the Rescue (chapter 27), fixes acting

With `KK_DEV_WANDER` at 110 FPS: the 0.016 s floor fix acted on 70 to 112 script calls a second while Kong's
grab and finisher scripts ran, and the step-decay fix corrected a V-Rex knock-back velocity 30 to 86 times a
second during hits. Gameplay with AI fights diverges between any two runs (random numbers are drawn per
frame), so their effect is not compared run against run.

## Fixed

- Camera shake (v1.9.10, `kk/src/camera_shake.cpp`): `CM_Sfx` (`sub_8245EC90`) and `SCC_SFX`
  (`sub_82553F20`) multiplied the shake's fall-off rate by a constant once a frame, so above 30 FPS a strong
  shake never ended (#39). The rate now falls by `mult ^ (dt * 30)`.
