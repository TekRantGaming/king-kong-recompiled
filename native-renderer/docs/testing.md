# Testing the renderer against golden frames

`native-renderer/tests/` answers one question unattended: does a GPU plugin draw scene X the way today's
renderer (the Xenos plugin) does? It plays scripted scenes, saves the game's own frames (the guest output, never
the desktop) and a frame log, and compares them with a golden set captured from the Xenos plugin.

| File | What it does |
|---|---|
| `tests/run.ps1` | Launches the game once per run, waits for the shots, stops the game, collects, compares |
| `tests/compare.py` | The Python half: `plan`, `collect`, `compare`, `calibrate`, `framelog` (run with `python -I`; numpy, Pillow) |
| `tests/scenes.json` | Runs (script, start clock, extra arguments, frame-log scene) and scenes (shot times, thresholds) |

## Use

```powershell
# Today's renderer against the golden set (the self-consistency check):
native-renderer\tests\run.ps1 -Plugin xenos
# The new renderer (rexgpu-native.dll beside king_kong.exe):
native-renderer\tests\run.ps1 -Plugin native
# Some scenes only (scene or run names), and wait up to an hour if the game is busy:
native-renderer\tests\run.ps1 -Plugin native -Scenes title,pause,vrex -WaitMinutes 60
# Capture the golden set again (asks for -Force when one exists):
native-renderer\tests\run.ps1 -Plugin xenos -Golden -Force
```

It needs the `kk-dev` build (`kk\build.bat kk-dev`). The report is `report\report.html` in the run folder
(`F:\KK-native-renderer\runs\<plugin>-<time>\`); exit code 0 pass, 1 fail, 2 frames missing, 3 the game was
already running, 4 setup error. A full run takes about NN minutes.

Before every launch the runner checks for a `king_kong` process and never starts a second copy or stops one it
did not start (`-WaitMinutes` re-checks every 5 minutes). While it runs, `F:\KK-native-renderer\game.lock` names
it; afterwards the file says `free`. It closes the game window when the shots are in (so the shader cache is
written) and stops the process if it has not exited 15 s later. Each run gets a private copy of the test save
(`F:\KK-native-renderer\userdata`, profile `B13EBABEBABEBABE`), so runs never change it.

Golden data, run folders and logs are never in git: `F:\KK-native-renderer\golden\<scene>\` (shots, `meta.json`,
`framelog.txt`) and `golden\_runs\` (raw run folders with `game.log`).

## Settings every run uses

`--kk_launcher=false --kk_frame_rate=30 --kk_cheats=true --kk_cheat_chapters=true --fullscreen=false
--window_width=1280 --window_height=720 --kk_original_look=true`, with `KK_DEV_AUTOSKIP=1` and the scene's
`KK_DEV_SCRIPT`, `KK_DEV_SHOTS`, `KK_DEV_SHOTS_FROM` and `REX_DEV_FRAME_LOG`. Every other `KK_*` and `REX_DEV_*`
variable of the calling shell is cleared for the launch.

`--kk_original_look=true` is the console's own picture: render scale 1 (1280x720 frames), no FXAA, the game's
own texture filtering, no ambient occlusion, motion blur, fog and the full-screen blur on. Without it the port's
presets apply: on a high-DPI screen the 1280x720 window is 3200x1800 physical pixels and the Quality preset
renders at 2x (2560x1440 shots), with FXAA and 4x anisotropic filtering on top. A new renderer first has to match
the console's picture, so the golden set is the Original look.

## Scenes

| Scene | Run | Shots (s) | What |
|---|---|---|---|
| `video` | logos | 8.4, 8.8, 9.2 after the first pad read | the Ubisoft logo movie, logo held (`--kk_skip_intros=false`) |
| `title` | title | 8, 9, 10 after the first pad read | the title screen, blinking Press START |
| `save_menu` | menus | 6, 7, 8 after the save menu opens | the save menu over the animated title background (a video) |
| `main_menu` | menus | 21, 22, 23 | Play / Options / Extras after loading the save |
| `chapter_select` | menus | 27, 28, 29 | the chapter select on the first chapter |
| `loading` | vrex | 38.0, 38.4, 38.8 | the V-Rex loading screen (sweeping picture) |
| `vrex_110`, `vrex_140`, `vrex_170` | vrex | five shots, t-1 .. t+1 | V-Rex gameplay: Jack idle in the rain under the cave roof |
| `pause` | vrex | 180, 181, 182 | the pause menu (START at 176 s) |
| `venture` | venture | NN | the Venture opening (chapter select entry 0) |
| `kong_cutscene`, `kong` | kong | NN | Kong to the Rescue (entry 27): the opening cutscene with Kong, then Jack's fight |

Times from the save menu (`KK_DEV_SHOTS_FROM=menu`) follow the script's clock; the boot scenes count from the
first time the game reads the pad (`from: boot`, nothing pressed). Shot times are multiples of 0.1 s
(`shot_<tenths>.bmp`). Each scene also has `golden_times`: a denser burst around the test times, captured for the
golden set only.

Scripts (seconds after the save menu opens): `12:A` picks the save, `18:A` confirms "Load successful", `24:A`
opens the chapter select from the main menu, `RIGHT` presses move along it (0.6 or 0.7 s apart), `A` starts the
chapter. The V-Rex script is the known-good one from Phase 0 (its START presses after the chapter starts only
open and close the pause menu: this chapter has no story movie when started from the chapter select). Chapter
order: entry 0 Venture, 8 V-Rex, 27 Kong to the Rescue (see the chapter list in the main project's notes).

## How frames are compared

Two runs of the same renderer never show exactly the same frames: loading times differ, so the game's clock is a
little ahead or behind; videos and the menu background (a looping video) move every frame; rain and lightning
are random. So:

1. **Closest frame.** Each test frame is compared with the golden frame of its scene that looks most like it
   (smallest mean difference on a quarter-size copy), and scored with MAE (mean absolute difference per channel,
   0-255), SSIM (8x8 windows on brightness), tile MAE (the worst of 16x9 tiles of 80x80 pixels) and the share of
   bad pixels (off by more than `bad_level`). Tight for still scenes, not checked for animated ones.
2. **Golden range.** Over all golden frames of the scene (the dense burst), each pixel has a lowest and highest
   value per channel. A test pixel that leaves that range by more than `env_tol` is "outside". Still parts
   (menu text, the HUD, a paused frame) have no range and must match; animated parts accept anything the golden
   frames showed. Scored as the share of outside pixels (`env_bad`) and the worst 80x80 tile of the outside
   distance (`env_tile`, which catches a small missing or wrong object).
3. **Draw list.** If both sides have a frame log for the scene, the canonical draw and resolve lines are diffed
   (Python `difflib` ratio, 1 = the same list), best golden frame per test frame. The report lists the shader
   pairs drawn more often on either side and links the diff.

A scene passes when at least `min_pass` of its test frames pass every check that has a threshold, and the draw
list (if any) reaches `drawlog`. Thresholds are in `scenes.json` (defaults, then per scene); `null` means not
checked. They were set from two independent Xenos captures with `compare.py calibrate` (worst value x2, with
floors), see "Calibration" below.

The report (`report.html`) starts with one PASS / FAIL badge and a table, one row per scene, every value next to
its limit and red when over. Failing scenes open below it with, per frame: the closest golden frame, the test
frame, the difference heat map and the outside-the-range heat map (black = same, white = 64 or more off; click
a frame for the full-size BMP). `summary.json` beside it has every number.

## The frame log format (`REX_DEV_FRAME_LOG`)

`REX_DEV_FRAME_LOG=<seconds>[,<frames>]` makes the Xenos plugin (D3D12: `src/graphics/d3d12/command_processor.cpp`,
Vulkan: `src/graphics/vulkan/command_processor.cpp` in the SDK) log every draw and resolve of `<frames>` frames
(default 1), starting at the first swap at least `<seconds>` after the log's clock started (the first draw or
swap of the run). The lines go to the game log (`--log_file`; level warning, category gpu). The native plugin
must write the same lines for the comparison to work:

```
Frame log: frame start
Frame log: draw <i> ps <16 hex> vs <16 hex> pitch <p> msaa <n> z <off|test>[+write][ func <n>] @<base>:f<fmt> off <x>,<y> scissor <w>x<h>[ rt<k>@<base>:f<fmt>]... count <n>
Frame log: resolve <i> <rt<k>|depth>@<base>:f<fmt> -> <8 hex> format <n> pitch <n> height <n> command <n> clear color <0|1> depth <0|1> off <x>,<y> surface pitch <n> msaa <n>
Frame log: frame end, <d> draws, <r> resolves
```

`frame start` is written once; every logged frame ends with `frame end`. The Vulkan backend adds ` func <n>`
(the depth function) after the z state; D3D12 does not. Field meanings, as the native plugin should fill them:

| Field | Meaning |
|---|---|
| `ps`, `vs` | XXH3-64 of the shader microcode exactly as it sits in guest memory (big-endian dwords, not swapped), the SDK's `Shader::ucode_data_hash`. `ps 0000000000000000` when no pixel shader is bound |
| `z` | depth test on (`test`) or off, `+write` when depth writes are on (RB_DEPTHCONTROL z_enable, z_write_enable) |
| `pitch`, `msaa` | the colour surface's pitch in pixels and its sample count |
| `@<base>:f<fmt>` | the depth surface: EDRAM base (tiles) and `xenos::DepthRenderTargetFormat` |
| `off`, `scissor` | window offset; the scissor's bottom-right corner (PA_SC_WINDOW_SCISSOR_BR) |
| `rt<k>@<base>:f<fmt>` | each colour target the pixel shader writes with a non-zero colour mask: index, EDRAM base, `xenos::ColorRenderTargetFormat` |
| `count` | the draw's index count (vertex count for non-indexed draws) |
| resolve source | `rt<k>` or `depth`, EDRAM base and format |
| `-> <8 hex>` | destination guest address (RB_COPY_DEST_BASE); `format` is its `xenos::ColorFormat`, `pitch` and `height` its size in pixels |
| `command` | `xenos::CopyCommand` (0 raw, 1 convert, 2 constant one, 3 null); `clear color` / `depth`: the resolve also clears |

The default diff compares the parts that mean the same in any renderer: shaders, z state, msaa, scissor, colour
target formats, depth format (when z is on) and count for draws; source kind and format, destination format and
size, command and clears for resolves (`compare.py framelog --strict` and the scene threshold
`drawlog_strict: true` add EDRAM bases, offsets, pitches, the depth function and addresses). Print a log in that
canonical form or diff two:

```powershell
python -I native-renderer\tests\compare.py framelog F:\KK-native-renderer\golden\vrex_140\framelog.txt
python -I native-renderer\tests\compare.py framelog <golden framelog.txt> <test game.log>
```

Timing: the log's clock is not the scripts' clock. The runner asks for `scene time + clock_offset`, where
`clock_offset` (seconds from the log clock's start to the shot clock's zero) is measured in each run and stored
in `golden\_runs\<run>.json` (`frame_log.clock_offset`); the next run uses it. `meta.json` records when the log
was actually taken (`frame_log.scene_time`), and the report shows it. Measured: NN.

## Calibration and the self-consistency check

NN

## Adding a scene

Add a run (if the scene needs its own launch) and a scene to `scenes.json`; check the timeline first with
`KK_DEV_SHOTS` every second or two (the frames show what is on screen when); capture the golden frames for it
(`run.ps1 -Golden -Scenes <name>`), run it twice more against the golden set and set its thresholds from
`compare.py calibrate`.

## Later: Linux on the Ally

The same compare tool works on frames from a Linux run (the BMPs and the game log are the same). A runner for the
ROG Ally over SSH would copy the build, run the game with the same environment, and copy `shot_*.bmp` and the log
back (the main project's `ally_*.sh` scripts show the pattern); then `compare.py collect` and `compare`.
