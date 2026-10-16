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
| `KK_DEV_HFR_TRACE` | Each fix logs once a second what it acted on (`KK dev hfr <name>: n`); the particle fix also logs generator updates, distinct generators and particle speeds |
| `KK_DEV_HFR_OFF=<name,...>` or `all` | Turns fixes off one by one in dev builds, to measure with and without |
| `KK_DEV_CAMERA_START=<s>` (with `KK_DEV_SAMPLE_DIR`) | For the first s seconds after a chapter's level loads: `camera_view.csv` (each drawn view matrix, field of view, game time, frame time, hero position, its previous position and speed) and `camera_ground.csv` (each `CM_Cam` ground-branch update: speed the game has and the speed the branch used, contact normal, camera axes); `dev_camera_start.cpp` |
| `KK_DEV_GFX_TRACE` | Once a second, the effects added to the world effect list (`sub_827F9978`) per caller and effect type |
| `KK_DEV_FORCE_DT=<seconds>` | Every frame counts as that long (small steps such as 0.004 s on a PC that cannot reach 250 FPS) |
| `KK_DEV_FISH=<csv>[,from,to]` (`kk/src/dev_fish.cpp`) | One row per call of the water creatures' swim state (`sub_8243B2C8`): swim phase before/after, facing, position, the wiggle angle asked for and used. `KK_DEV_FISH_VIEW=ann-water` moves the player near the fish in Ann's chapter (the game's own teleport trigger) so they swim; far away they are faded out and idle. `KK_DEV_FISH_OFF_UNTIL=<s>` keeps the fix off until then, to measure both in one run |
| `KK_DEV_CLOCK_TRACE=1` (`kk/src/dev_clock_trace.cpp`, with `KK_DEV_SAMPLE_DIR`) | `clock.csv`: one row a frame for the first 120 s (game clock `0x82CCAE1C`, paused total `0x82CCC02C`, game time, frame time, AI passes) plus the clock resets and pause/resume calls and the boat's first `Chaloupe_Rotation` call |

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
| Frame time in whole milliseconds | `sub_8272CA20` (the game's clock, every system) | The tick count has 1 ms steps: at 144 FPS a 6.94 ms frame reads as 6, 7 or 8 ms, so motion is uneven by up to 15% frame to frame; above about 500 FPS frames read as 0 ms, are clamped up to 1 ms and the game runs fast (1.4x at 1400 FPS, 2x at 2000) | fixed (`hfr_frame_time.cpp`: the host's high resolution clock, same clamps and time scale) |
| Lifeboat crew rowing (the reported "rowing skips") | `Chaloupe_cb_aftercam` `sub_824269C8` setting a rower's animation time with `sub_8271AE08`; the action hand-over in the key player `sub_8271BA18` and `sub_8280FF38` | The boat script keeps a rower 10 frames ahead of the lead rower by setting its animation time to whole 1/60 s frames every frame. A once-only action hands over to the next only when the animation's own step crosses its last frame, which from a whole frame needs a step of at least 1/60 s. At 110 FPS three rowers stayed in the 1.2 s "start rowing" action 275 for the whole crossing (looping it, 50 loops a minute) instead of moving to the rowing loop 256, as they do at 30 FPS within a second | fixed (`hfr_rowing.cpp`: the same sync without the rounding) |
| Camera shake | `CM_Sfx` `sub_8245EC90`, `SCC_SFX` `sub_82553F20` | Fall-off multiplied once a frame: a strong shake never ends | fixed in v1.9.10 (`camera_shake.cpp`) |
| Particle drag | `sub_82862460` (every particle generator: rain splashes, sparks, spray, smoke) | Drag (`+48`) applied once per 1/60 s sub-step plus once a frame: 40 times a second on the console, 60 at 60 FPS, 120 at 120, hundreds unlimited. Particles stop short and drop straight | fixed (`hfr_particles.cpp`) |
| Footstep splashes in water | `sub_825845E0` from `H_callback_after_blend` `sub_826BB350` (both feet, bones 60 and 80, of a character standing in water) | Each call with the foot down starts a new splash effect (a particle effect of type 9), once a frame per foot: 30 a second on the console, 110 at 110 FPS. In the V-Rex chapter (rain, wet ground) this tripled the particle effects alive at once at 110 FPS (table below) | fixed (`hfr_splash.cpp`: each foot splashes on a 1/30 s grid of game time) |
| Water ripples | `GST_MM_exec_riple` `sub_82520A50` from `GST_MM_go` `sub_82521040` (an object moving in water) | Starts a ripple effect (type 9) at a random point near the object each call that finds water there. Its timer compares with a last-ripple time (`+332`) that no GST_MM script writes, so it runs once a frame: 21 a second at 30 FPS in the V-Rex chapter, 64 at 110 | fixed (`hfr_splash.cpp`, "ripple": each object ripples on a 1/30 s grid of game time) |
| 0.016 s floor | `k_ETAT_grab` `sub_825CA350`, `k_ETAT_grab_mashing` `sub_8259D998`, `k_ETAT_finished` `sub_825B3288`, `k_ETAT_cine` `sub_825B62E0`, `KT_ETAT_Finish` `sub_824876F0`, `KBC_ETAT_KK_Finished` `sub_8252B1D0`, `BC_exec_MOVE_beziers` `sub_8263E1B8` | The frame time is clamped to 0.016 .. 0.048 s before use, so above 62.5 FPS these run fast (1.9x at 120 FPS, 3.8x at 240): Kong's grabs, button mashing and finishers, the BC creature's path moves | fixed (`hfr_dt_floor.cpp`) |
| Decay per 0.016 s | inlined in `BC_TAG_On` `sub_8263C930`, `KBC_TAG_On` `sub_82512FA8`, `KBC_ETAT_Fight_BB_Poursuit` `sub_8252E1E8`, `k_ETAT_grab_ANN` `sub_8259F698`, `CM_Kong_Mode_Swing_Horiz` `sub_826245B8`, `k_ETAT_grab` (five more); helper `sub_8261AD78` | `f = k; while (t > 0.016) { t -= 0.016; f *= k; }`: one factor a frame plus one per extra step, 90 a second on the console, 120 at 120 FPS, 240 at 240. Knock-back slides and Kong's swing camera stop sooner. `k_ETAT_grab` also grows a vector by 1.005 per 1/60 s in whole steps rounded up (70 a second on the console, 110 at 110 FPS) | fixed (`hfr_step_decay.cpp`; the velocity is corrected after the call when the game's step was the only change to it) |
| Follow smoothing in 1/300 s steps | `sub_828271E8` (from `sub_82827B10`, a render-time modifier; runs on four objects in the Venture opening, two in Kong to the Rescue) | `ceil(dt * 300)` steps a frame: 310 a second at 30 FPS, 340 at 60, 360 at 120, 432 at 144, 480 at 240, 1000 at 1000 FPS. Follows its target faster at high rates | fixed (`hfr_follow.cpp`) |
| Fish swimming wiggle (Ann's chapter) | `PNJ_EatMe_ETAT_Water` `sub_8243B2C8` turning the fish's facing with `sub_827184C0` (call at `8243C10C`) | Each call turns the facing 6 x frame time of the way to the fish's course, then by `sin(swim phase) x min(forward speed, 0.25)` radians. The phase advances by pi x frame time (a 2 s period at any rate) but the turn is added once a call with no frame time: the heading swings about +-65 degrees on the console, twice that at 60 FPS, four times at 120 (the fish turn circles). Measured with `KK_DEV_FISH` (below) | fixed (`hfr_fish.cpp`: that angle times 30 x frame time) |
| Menu background drift | `IntMIG_Background` `sub_824A0E70` | Its drift velocity is multiplied by 0.999 once a frame (0.97 a second at 30 FPS, 0.90 at 110) before it is steered back to its speed; hardly visible | found, left |
| Motion blur strength | `sub_82768DE8` (after effect; only with `kk_motion_blur` or the Original look) | A feedback blur: the shader (`ProcessPixel`, `iMotionBlur`) draws the kept previous output over the new frame with alpha `g_fMotionBlurAlpha` (`0x82F1CDC8`) = `min(1, s * (0.6 + 0.4 * fps / 30))`, `fps` being the device's frames-per-second estimate (`0x82D623D0`, 29 at a 30 cap, 119 at 120; not the frame time as first noted), and the result is resolved (`sub_82116178`) as the next kept frame, so an image fades by the alpha per frame. The linear fps term gives the wrong fade per second: at 120 FPS the trail is about 2x shorter at low strength, equal at s 0.35, longer above and frozen (alpha 1) from s 0.46. Fixed: alpha `min(1, s) ^ (30 dt)` (task 166) | fixed |
| Boat roll on synthetic waves | `Chaloupe_Rotation` `sub_8242ECA0` (every lifeboat-like boat: the Venture opening, Necropolis) with `sub_82623098` | With synthetic waves (`+108`) the corner "water" heights keep the corners' own heights from the boat's already tilted axes and add `+472 * sin(2 t)`; the plane through them is the new tilt, slerped back toward the old one by `+88 * dt` (`+88` = 1). So each frame adds (1 - dt) times the wave's slope: the roll integrates per frame. The pull toward a leader's up axis (`+504`, `+468 * dt`) survives only times that slerp factor, so it shrinks with the frame rate. Necropolis boat (benchmark route): about 35 degrees each way at 30 FPS, 60-75 at 60, 80-85 at 120 | fixed (`hfr_sway.cpp`, "boat": `+88` set for the call so 29 dt of the change is kept, `+468` so the leader pull per second is the 30 FPS one) |
| Hanging objects on a rope | `sub_825625F8` from `SA_ETAT_WaitAccroche` `sub_8268A450`, `SA_ETAT_Loop` `sub_8268E948`, `SA_ETAT_Fall` `sub_82692930` (boxes on a rope the player shoots down) | The rope is stepped once a call with a fixed 1/30 s (`+1084`, velocity = move x 30), its divider `+1080` counts calls: the rope's time runs at frame rate / 30, so the box swings 2x as fast at 60 FPS and 4x at 120. `SA_ETAT_WaitAccroche` also adds a turning wind push, 0.02 x (-sin, cos, 0)(pi t), to the end node's velocity every call (+1180 + 12 x nodes), fps/30 times per step | fixed (`hfr_sway.cpp`, "rope": one step per 1/30 s of game time, frame time read inside at least 1/30, so the rope moves in 30 Hz steps; the wind push scaled to 30 dt a call) |
| Game time at a level's start | `sub_8272CA20` (clock = (ticks - start) / 1000 every frame), reset at boot by `sub_8287DFE8` (from `sub_82858A18`); paused spans added to `0x82CCC02C` by the pause pair (messages 0x1BC1 `sub_824126A0` / 0x1BC2 `sub_82412670`, also `sub_825E66D0`, `sub_826EA8C0`) | The game time the world reads is real time since boot minus pauses. A start the port carries out (launcher, `--kk_start`, benchmark) boots through a fixed number of covered frames (title level and the port's 5-frame wait; the boat's first call is frame 29 at every rate), so the chapter's level begins at 2.0-2.1 s at 30 FPS but 1.5-1.6 s at 60 and 1.3-1.5 s at 120: things on game time start at another phase (the Necropolis boat's start-up lean). Through the game's own menus the start time is up to the player anyway | fixed (`hfr_frame_time.cpp`, "start_clock": until that level has loaded, each frame counts at least 1/30 s of game time) |
| Camera pitch after landing (chapter starts) | `CM_Cam` `sub_8246F180` ground branch `82471528`..`824715C8`; hero speed `+11668` of the hero state (`[[hero +24] +4] +64`), written at `826C1478` | The branch turns the camera's forward z toward the ground's slope by `clamp(0.2 x speed x dt)`. The speed is measured from a position one frame old, so this is 0.2 x the hero's step in the frame before. At a chapter start the hero drops a few centimetres onto the floor: only the last airborne step and the landing step reach the camera, and the tilt stays. That is 1-2 frames of the drop: 1/30-2/30 s at 30 FPS, 1/120-2/120 s at 120. The Wall: forward z 0.0037-0.0048 at 30, 0.0030 at 60, 0.0010-0.0019 at 120 (the camera starts up to 3 pixels lower at 1280x720). Swamps and In the Mud match (their first contact frame is the 63 ms loading frame at every rate), Venatosaurus 0.24 px, Millipedes 0 | fixed (`hfr_cam_ground.cpp`, "cam_ground": the branch reads the hero's path one 30 FPS frame late (from t_prev - 1/30 to t - 1/30 of game time) instead of one frame late; the hero's speed is put back before anything else reads it) |
| Raft | `Raft_exec_compute_position` `sub_825EFE78` | A fixed 0.016 s step from an accumulator: right speed at any rate, but moves in 62.5 Hz steps | ok (note) |
| Animation frame tests | 177 calls of `sub_823F8AF0` (an object's animation frame, in 1/60 s) | A test like `frame == N` would fire on one frame at 30 FPS and on two or more at 120. All the tests found compare with `<`, `<=`, `>` or `>=` | ok |

Looked at and frame-rate correct: the dt pipeline itself, the event (animation) key player `sub_8271BA18`, the
particle spawner `sub_82862B10` (a time accumulator), the 1/30 s sub-stepper `sub_827C8C20` (its damping uses
`pow`), the menu key repeat in
`IntMIG_loop`, the AI track timers in `sub_8271D1D8`.

## Measurements

Runs on this PC (Xenos plugin, `kk-dev` build, private copy of the test save). The PC never goes above about
110 FPS, also uncapped (a driver limit), so "120" and "unlimited" runs are 110 FPS; behaviour at 250 FPS and
more was tested with `KK_DEV_FORCE_DT=0.004` (every frame counts as 4 ms; the game runs slower than real time
but each system steps as it would at 250 FPS). Raw logs, samples and captures are in
`F:\KK-native-renderer\analysis\hfr\runs` (not in git).

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

### Venture opening, camera tutorial and rowing section

Chapter entry 0 stops on the VENTURE hull with "Right stick to aim" until the player pushes the right stick up
or down (`Chaloupe_CineD1` `sub_82432CD8` takes that push as the vertical sighting choice and sets the pitch
inversion at `G[19884]`), and then needs the boat's target in view for 0.1 s (`TrigTest_PivotInScreen`
`sub_82466F60`, a timer of frame times); the lifeboat is then lowered and rowed to the island (about 75 s; the
rowers play actions 255, 256 and 262), and Ann's scene follows. A dev trace of the tutorial
(`KK_DEV_VENTURE_TRACE`) shows the same steps at 30 and 110 FPS; with the bot's long stick pushes the target
sometimes left the view before 0.1 s had passed (1 of 4 runs got through at 110 FPS, 2 of 2 at 30), which is
the push's timing, not a frame-rate fault: the view check and its timer use the frame time.

The rowing section (55 s from the lead rower's first stroke; animation speed is animation seconds per game
second, loops are animation restarts):

| Rower | 30 FPS | 110 FPS, no fix | 110 FPS, fix |
|---|---|---|---|
| `421329F0`, `42132958`, `42134460` | action 256, speed 0.997, 25 loops | action 275 (start rowing) the whole time, 50 loops | action 256, speed 1.001, 25 loops |
| `418D044C`, `41F31E30`, `421326A0` | action 255, 0.990, 25 loops | 255, 1.005, 25 | 255, 0.998, 25 |
| `418D0474` (lead) | action 262, 0.987, 26 loops | 262, 0.998, 26 | 262, 1.005, 26 |
| `418D0540` | action 260, restarted 90 times (via 0) and never looping | 260, 56 restarts, 18 loops | 260, 34 restarts, 29 loops |

At 60 FPS without the fix the hand-over happened in the measured run (action 275 for 1.02 s, as at 30 FPS: 1.03 s;
110 FPS with the fix: 1.02 s), but there it hangs on luck: the tick count makes a third of the frames 16 ms, a
step of 0.96 frames that does not cross the last frame, and the next sync then wraps the rower to frame 0, so
some loops are missed (which fits the reports of skips at 60 FPS too).

The fix's dev trace counted 83 corrected syncs a second at 110 FPS (one a frame while the boat is rowed). The
last rower (`418D0540`) is restarted by its script (through action 0) every 1.2 s or so; how often varies from run
to run at the same frame rate (83 and 90 restarts at 30 FPS, 34 and 86 at 110 FPS with the fixes), so it is not
frame-rate dependent.

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

The V-Rex chapter's rain and water effects (generators with drag 0.99, 20 000 to 600 000 particles counted a
second):

| Run | Mean particle speed (1 s samples, steady part) |
|---|---|
| 30 FPS | 1.24 to 1.34 (mean about 1.30) |
| 110 FPS, fix off | 0.99 to 1.07 (mean about 1.03) |
| 110 FPS, fix on | 1.23 to 1.32 (mean about 1.28) |

The same V-Rex runs, particle effects alive at once (generator updates a second times the frame time) and
particles alive at once, over the seconds with effects (about 100 a run), and the effects started a second by
the two water effects (`KK_DEV_GFX_TRACE`, per second in which they ran):

| Run | Effects alive: mean (10th, 50th, 90th percentile) | Particles alive: mean (50th percentile) | Footstep splashes a second | Ripples a second |
|---|---|---|---|---|
| 30 FPS (two runs) | 34 (13, 28, 63) and 34 (13, 28, 61) | 754 (592) and 746 (571) | 25.6 | 20.7 |
| 110 FPS, all fixes but `splash` and `ripple` | 103 (36, 90, 182) | 2 356 (1 931) | | |
| 110 FPS, all fixes but `ripple` | 57 (27, 55, 85) and 57 (26, 55, 86) | 911 (834) and 910 (878) | 22.5 | 64.2 |
| 110 FPS, all fixes | 33 (12, 29, 60) | 720 (655) | 24.5 | 19.2 |

The rain itself (`sgo_gener` and `sgo_move`, about 6 drop splashes a second) is the same at both rates. With both
fixes about 140 footstep splash calls and 70 ripple calls a second were skipped at 110 FPS.

### Fish in Ann's chapter (fix verified)

Chapter-select entry 9, `kk-dev-vulkan`, `KK_DEV_FISH` with `KK_DEV_FISH_VIEW=ann-water` (the player moved near the
water so the five fish there swim), script-clock seconds 8 to 50 (or 8 to 28 without and 34 to 60 with the fix in one
run, `KK_DEV_FISH_OFF_UNTIL=30`; the 60 and unlimited runs in the quiet window). Averages over the five fish; heading swing is the RMS of the facing angle about its
2 s running mean, turn rate the facing's total turning per game second. The fish's paths are random, so values move
by about 10% between runs at the same rate.

| Run | Measured FPS | Wiggle angle used, rad per game s | Heading swing RMS, rad | Turn rate, rad/s | Phase rate, rad/s |
|---|---|---|---|---|---|
| Vulkan 30 (fix off by design) | 29.9 | 4.14 | 1.11 | 3.24 | 3.141593 |
| Vulkan 60, no fix | 60.0 | 8.00 | 2.09 | 6.16 | 3.141593 |
| Vulkan 60, fix | 60.0 | 4.06 | 1.05 | 3.01 | 3.141593 |
| Vulkan 120, no fix | 120.0 | 15.82 | 5.36 | 14.74 | 3.141593 |
| Vulkan 120, fix | 120.0 | 4.15 | 1.05 | 2.97 | 3.141593 |
| DirectX 12 120, no fix | 120.0 | 16.06 | 5.48 | 15.07 | 3.141593 |
| DirectX 12 120, fix | 119.9 | 4.16 | 1.04 | 2.93 | 3.141593 |
| Vulkan unlimited, no fix | 488.6 | 64.96 | 23.92 | 65.01 | 3.141592 |
| Vulkan unlimited, fix | 489.7 | 4.19 | 0.96 | 2.92 | 3.141593 |

Every fish ran its swim state once per frame in every run (no skipped frames). The swim phase advances by exactly
pi per game second whenever it advances; it holds while a fish is above the water line (a leap of about 0.86 s, the
same length at 30 and 120 FPS, or a fish skimming the surface), 13% of calls at 30 FPS, 17% at 120 without the fix
(the circling fish break the surface more), 13% with it.

### Step counts, worked out

The game's own step logic (float32, the millisecond frame times the tick count gives) against the fixes'
targets, per second of game time. Without the fixes the counts rise with the frame rate; with them each is the
30 FPS figure at every rate (the fixes use `pow` on the exact frame time):

| FPS | Particle drag | Decay per 0.016 s | Follow steps | `k_ETAT_grab` growth |
|---|---|---|---|---|
| 30 | 40 | 90 | 310 | 70 |
| 60 | 60 | 100 | 340 | 100 |
| 110 | 110 | 110 | 330 | 110 |
| 144 | 144 | 144 | 424 | 144 |
| 240 | 240 | 240 | 480 | 240 |
| 1000 | 1000 | 1000 | 1000 | 1000 |

For example a particle with drag 0.95 keeps 36% of its speed after 0.5 s at 30 FPS, 6% at 110 FPS and 0.2% at
250 FPS without the fix, 36% at all of them with it.

Measured at 110 FPS (rowing runs, 11 200 frames each): without the fix the frame time takes 30 distinct values
(whole milliseconds, standard deviation 0.99 ms around 9.06 ms); with it 268 values, standard deviation 0.77 ms,
the rest being the frame pacing itself.

Millisecond frame times (`precise_dt`): the frame time's worst error against the real one is 2% at 30 FPS, 4% at
60, 10% at 110, 14% at 144, 20% at 240; at 1400 FPS the game runs 1.4x real time, at 2000 FPS 2x.

### Kong to the Rescue (chapter 27), fixes acting

With `KK_DEV_WANDER` at 110 FPS: the 0.016 s floor fix acted on 70 to 112 script calls a second while Kong's
grab and finisher scripts ran, and the step-decay fix corrected a V-Rex knock-back velocity 30 to 86 times a
second during hits. Gameplay with AI fights diverges between any two runs (random numbers are drawn per
frame), so their effect is not compared run against run.

## How the systems were found

1. Static scans of the recompiled code (`kk/generated/default`, one function per `DEFINE_REX_FUNC`), with each
   function's script name from the executable's tables: stores of a float field that was loaded, multiplied or
   added to and stored back with no frame time in the chain (per-frame decays, about 1200 candidates, mostly
   vector maths); small loops that subtract a step from a time (`while (t > step)`, 50 of them); every compare
   of the frame time with a constant (clamps, 25); integer fields counted up by one (frame counters); calls of the
   animation frame function and how its result is tested. Each candidate that runs during play was then read.
2. The call profiler (`kk-prof`) in the same windows of the same scripted scene at 30 and 60 FPS: functions run
   once a frame show a ratio of 2, events 1; event-like functions whose rate changes with the frame rate, and
   functions that run at one rate only, are suspects.
3. The sampler and the compare tool over game time, for positions, animation clocks and AI states, and the
   fixes' own dev traces (`KK_DEV_HFR_TRACE`) for what they act on.

## Summary

### Fixed (kk_hfr_fixes; each one off at 30 FPS; `KK_DEV_HFR_OFF=<name>` turns one off in dev builds)

| Fix | File | Verified |
|---|---|---|
| `rowing`: the lifeboat crew's start-rowing hand-over | `hfr_rowing.cpp` | In game at 110 FPS: the three affected rowers play the rowing loop like at 30 FPS (table above) |
| `particles`: particle drag per second | `hfr_particles.cpp` | In game at 110 FPS and with 4 ms frames: particle speeds match 30 FPS (table above) |
| `precise_dt`: frame time from the high resolution clock | `hfr_frame_time.cpp` | In game at 110 FPS: frame times continuous (268 values instead of 30); game time = real time |
| `dt_floor`: Kong's grab and finisher scripts and the BC path script | `hfr_dt_floor.cpp` | Acts in Kong to the Rescue (70 to 112 calls a second at 110 FPS); the effect follows from the code (time step = frame time instead of 0.016 s) |
| `step_decay`: decays and growth per 0.016 or 1/60 s step | `hfr_step_decay.cpp` | Acts during V-Rex hits in Kong to the Rescue; per-second rates worked out above |
| `follow`: follow smoothing in 1/300 s steps | `hfr_follow.cpp` | Runs on four objects in the Venture opening; per-second rates worked out above |
| `boat` and `rope`: boat roll on waves, hanging boxes on a rope | `hfr_sway.cpp` | Benchmark route, Necropolis, Vulkan 30/60/120 and D3D12 120 FPS (task 151): the box's speed and spread match 30 FPS within 10% (the rope moves in 30 Hz steps at high frame rates, accepted for now); the boat's roll change per second matches 30 FPS exactly (traced per call) and its swing matches from about 12 s on. Its start-up lean (first 10-15 s) differed because the game clock was 2.0 s at the boat's first frame at 30 FPS but 1.6 s at 60 and 1.3-1.5 s at 120 (another wave phase); fixed by `start_clock` (task 158) |
| `start_clock`: game time at the start of a chapter the port starts | `hfr_frame_time.cpp` | Benchmark route and `--kk_start` Necropolis (task 158, `KK_DEV_CLOCK_TRACE`): game time at the boat's first call 1.99 s at 30 FPS; 2.00/2.01 at 60, 2.01/2.03 at 120 Vulkan, 1.94 at 120 D3D12 (unfixed 1.48-1.61, 1.29-1.30, 1.47); boat first peaks -45/+18 (60), -44/+19 and -44/+20 (120), -47/+16 (D3D12) against -42..-47/+16..+21 at 30; later peaks and the box unchanged |
| `cam_ground`: camera pitch after a landing | `hfr_cam_ground.cpp` | `--kk_start` The Wall, Vulkan (task 163, `KK_DEV_CAMERA_START`): settled forward z 0.0037/0.0042/0.0046/0.0048/0.0046 at 30 FPS; fixed 0.0048 at 60, 0.0045/0.0045/0.0049/0.0047 at 120, 0.0047 at 240, 0.0061 at 120 D3D12 (that run's landing frame took 24 ms; inside 30's range of 1-2 frames, 0.0033-0.0072); unfixed 120 0.0010-0.0019. Frames at 10 s: 120 fixed vs 30 0.25 px apart (both 1.0 px from the Classic tour golden, a whole-pixel offset). Venatosaurus 120 0.00037 vs 30 0.00036, In the Mud unchanged |
| `splash` and `ripple`: footstep splashes and ripples in water | `hfr_splash.cpp` | In game at 110 FPS (V-Rex chapter): effects started a second and particles alive as at 30 FPS (720 alive on average against 746 and 754 at 30 FPS, 2 356 without the fixes; table above) |
| `fish`: the water creatures' swimming wiggle | `hfr_fish.cpp` | In game, Ann's chapter: heading swing and turn rate per game second at 60, 120 and unlimited match 30 FPS (table above); `kk-hfr-fish-tests` models the recurrence at 30 to 500 FPS |
| `blur`: the motion blur trail's fade per second | `motion_blur.cpp`, `hfr_blur_logic.h` | `--kk_start` entry 10 (Kong, level 304), Original look, `KK_DEV_BLUR_TRACE` (task 166): trail half-life (ms of real time for the kept image to fade to half) at strength 0.15/0.25/0.35/0.44: 30 FPS 12.4/16.6/21.8/27.9; unfixed 120 5.2/9.6/21.6/151.6; fixed 60 12.2/16.8/22.0/28.2, 120 12.2/16.6/22.0/27.9, 120 D3D12 12.3/16.6/21.8/27.5, 240 12.5/16.7/21.3/28.2; unfixed 60 7.5/11.0/16.0/23.5; at 30 the alpha is the game's own on every frame; `kk-hfr-blur-tests` models the blend at 30 to 500 FPS. Not looked at on screen |
| Camera shake (v1.9.10, before this work) | `camera_shake.cpp` | #39 |

At 30 FPS every fix calls the game's own code unchanged (`hfr::Active()` is false), so the console behaviour and
the golden frames stay the same. Checked with the golden V-Rex run (Original look, 30 FPS, `kk-dev` build of
this branch, compared with `native-renderer/tests/compare.py images` and each scene's thresholds): pause 3 of 3
frames pixel-identical, V-Rex gameplay at 110 s 3 of 5, 140 s 5 of 5, 170 s 4 of 5 (rain and lightning; the
scenes need 3), loading screen 0 of 3: the frames show the loading screen with its picture swept to a different
place than in any golden frame (the sweep runs on the loading thread; the capture came at another moment).
The golden run came before the `splash` and `ripple` fixes; at 30 FPS they too call the game's code unchanged.

### Found and left

- Menu background drift (`IntMIG_Background`): 0.999 a frame, too small to see.
- Follow smoothing has no visible target identified yet (a renderer modifier on four objects in the Venture
  opening, probably the boat lights); fixed by the code, not checked on screen.
- The aim-assist object test in `CM_Cam` (`sub_8270AF70`) ran only at 60 FPS in one pair of profiles; no visible
  effect found.
- Scenes not measured by script: Kong's climbing chapters, the raft and rapids, the biplane fight on the Empire
  State Building, the V-Rex walk (needs a player). The static scans cover their scripts; the families above are
  the ones found there.
- AI fights (Kong to the Rescue) diverge between any two runs at any frame rate, because the random number
  generator is drawn per frame; whether any fight decision is made per frame rather than per second could only be
  told from many runs.
- The bot's push in the opening's camera tutorial passed the "look at the boat" step in 1 of 5 runs at 110 FPS and
  3 of 3 at 30; the step's checks use the frame time, so this looks like the push's timing, but a player at a
  high frame rate who gets stuck there would be the sign of a problem (`KK_DEV_VENTURE_PASS` passes it in tests).

### Needs a player

- The Venture opening at 120 FPS or unlimited: the lifeboat crew should row from the start like at 30 FPS. If it
  looks right, the launcher's warning about rowing above 30 FPS (`kk/src/launcher.cpp`) and the known issue in
  `README.md` can go; they are unchanged on this branch.
- With motion blur on (Original look, Kong chapters): the trail at 60/120 FPS should look as long as at 30 (fixed by
  the numbers, not looked at; `KK_DEV_HFR_OFF=blur` shows the unfixed trail).
- The chapters no script reached (the list above), especially Kong's fights, climbing and the raft, at 60 and
  120 FPS and unlimited, against 30 FPS. `kk_hfr_fixes=false` turns every fix off for a side-by-side check.
