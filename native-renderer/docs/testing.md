# Testing the renderer against golden frames

`native-renderer/tests/` answers one question unattended: does a GPU plugin draw scene X the way today's
renderer (the Xenos plugin) does? It plays scripted scenes, saves the game's own frames (the guest output, never
the desktop) and a frame log, and compares them with a golden set captured from the Xenos plugin.

| File | What it does |
|---|---|
| `tests/run.ps1` | Launches the game once per run, waits for the shots, stops the game, collects, compares |
| `tests/compare.py` | The Python half: `plan`, `collect`, `compare`, `calibrate`, `framelog`, `images` (run with `python -I`; numpy, Pillow) |
| `tests/scenes.json` | Runs (script, start clock, extra arguments, frame-log scene) and scenes (shot times, thresholds) |
| `tests/test_compare.py` | Tests of the compare tool with synthetic images and logs (no game data); runs on any OS |

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
# The chapter tour (tests/tour.json, all 42 chapter-select entries in four launches, about 35 minutes):
native-renderer\tests\run.ps1 -Plugin native -Tour
# The tour on the native plugin with its pipeline cache recording, one file per launch collected in
# <run folder>\pipelines\ (see "The chapter tour"):
native-renderer\tests\run.ps1 -Tour -RecordPipelines
# On a shared PC: wait for the game up to 10 hours and leave it to others 6 minutes between launches:
native-renderer\tests\run.ps1 -Plugin native -Tour -WaitMinutes 600 -PauseMinutes 6
```

It needs the `kk-dev` build (`kk\build.bat kk-dev`). The report is `report\report.html` in the run folder
(`F:\KK-native-renderer\runs\<plugin>-<time>\`); exit code 0 pass, 1 fail, 2 frames missing, 3 the game was
already running, 4 setup error. A full run takes about 8 minutes (six launches; golden capture about 10).

Native runs use the `kk-dev-vulkan` build, which needs shaders: the shader pack the launcher builds into
`<cache root>\native-shaders` (the runner's cache root, `F:\KK-native-renderer\cache`, has none: the runner
skips the launcher), else a developer's pack beside `king_kong.exe` (`kkshaders-spirv.pack`), and
`dxcompiler.dll` beside it for shaders the pack lacks (from the build with `KK_DXC_DIR`, or copied). Without
either every draw is dropped and the frames are black (the log says "is not in the pack and DXC is missing"):
the first native tour run (`runs\tour-native2`) was lost that way. The tour runs below used the pack and DXC
copied from the native renderer's own build folder (`wt\04-backend\kk\out\build\kk-dev-vulkan`).

Before every launch the runner checks for a `king_kong` process and never starts a second copy or stops one it
did not start (`-WaitMinutes` re-checks every 5 minutes); a lock file saying `hold` or `user` counts as busy too.
While it runs, `F:\KK-native-renderer\game.lock` names it (`-LockName`); afterwards the file says `free`. The
game is launched with `SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0` and `SDL_WINDOW_ACTIVATE_WHEN_RAISED=0`, so its window
never takes the focus from whoever is using the PC (the captures are the game's own frames either way). Without
`-Exe` the runner takes `kk\out\build\kk-dev\king_kong.exe`, or the `kk-dev-vulkan` build's when only that one
has the plugin asked for (`rexgpu-native.dll`). It closes the game window when the shots are in (so the shader cache is
written) and stops the process if it has not exited 15 s later. Each run gets a private copy of the test save
(`F:\KK-native-renderer\userdata`, profile `B13EBABEBABEBABE`), so runs never change it.

Golden data, run folders and logs are never in git: `F:\KK-native-renderer\golden\<scene>\` (shots, `meta.json`,
`framelog.txt`) and `golden\_runs\` (raw run folders with `game.log`, and `<run>.json` with the measured clock
offset). The golden set captured on 10 October 2026 (Xenos plugin, D3D12, `kk-dev` build of commit 9aa5734):
296 frames of 1280x720 in 13 scene folders, about 1.0 GB, plus 1.2 GB of raw run folders. Test runs go to
`F:\KK-native-renderer\runs\`.

## Settings every run uses

`--kk_launcher=false --kk_frame_rate=30 --kk_cheats=true --kk_cheat_chapters=true --fullscreen=false
--window_width=1280 --window_height=720 --kk_original_look=true`, with `KK_DEV_AUTOSKIP=1` and the scene's
`KK_DEV_SCRIPT`, `KK_DEV_SHOTS`, `KK_DEV_SHOTS_FROM` and `REX_DEV_FRAME_LOG`. Every other `KK_*` and `REX_DEV_*`
variable of the calling shell is cleared for the launch, and `SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0` /
`SDL_WINDOW_ACTIVATE_WHEN_RAISED=0` are set so the game's window does not take the focus from whoever uses the PC.

Native runs add `--native_pipeline_cache=false`, and since Phase 2c `--native_pipeline_list=false` and
`--native_vk_pipeline_cache=false` (`plugin_args` in `scenes.json`; the other new defaults stay on): with the cache, a run makes every
pipeline earlier runs recorded, and the chapter loads then lose no draws and wait for no pipelines, which moves
the Kong to the Rescue timeline against the golden frames (the V-Rex is in another pose at the same shot time:
0/5 frames, against 5/5 without the cache). The golden set was made with the Xenos plugin's own waits for
pipelines, which the native renderer's 30 ms budget resembles.

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
| `loading` | vrex | 37.8, 38.0, 38.2 | the V-Rex loading screen (it shows for about half a second) |
| `vrex_110`, `vrex_140`, `vrex_170` | vrex | five shots, t-1 .. t+1 | V-Rex gameplay: Jack idle in the rain under the cave roof, random lightning |
| `pause` | vrex | 180, 181, 182 | the pause menu (START at 176 s) |
| `venture` | venture | 119 .. 121 (five) | the Venture opening (entry 0): the rocking hull with VENTURE painted on it, the "Right stick to aim" hint |
| `kong_cutscene` | rescue | 53, 54, 55 | Kong to the Rescue (entry 27): the opening cutscene, Kong facing the V-Rex |
| `kong` | rescue | 61 .. 63 (five) | the same chapter in play: Jack behind the rocks, a V-Rex coming in, fires, rain |

Frame logs: `video` (logos), `title`, `save_menu` (menus), `vrex_140` (vrex), `venture` and `kong` (rescue); a
run can log one window only. Selecting a run name (`-Scenes rescue`) selects all its scenes.

Times from the save menu (`KK_DEV_SHOTS_FROM=menu`) follow the script's clock; the boot scenes count from the
first time the game reads the pad (`from: boot`, nothing pressed). Shot times are multiples of 0.1 s
(`shot_<tenths>.bmp`). Each scene also has `golden_times`: a denser burst around the test times, captured for the
golden set only.

Scripts (seconds after the save menu opens): `12:A` picks the save, `18:A` confirms "Load successful", `24:A`
opens the chapter select from the main menu, `RIGHT` presses move along it (0.6 or 0.7 s apart), `A` starts the
chapter. The V-Rex script is the known-good one from Phase 0 (its START presses after the chapter starts only
open and close the pause menu: this chapter has no story movie when started from the chapter select). Chapter
order: entry 0 Venture, 8 V-Rex, 27 Kong to the Rescue (see the chapter list in the main project's notes).
Timelines seen (seconds after the save menu opens): Venture shows a black screen from 32 to about 92 s, then the
hull; Kong to the Rescue loads in about 2 s after `48:A` and plays its cutscene from about 51 s, Jack's view from
about 58 s.

## How frames are compared

Two runs of the same renderer never show exactly the same frames: loading times differ, so the game's clock is a
little ahead or behind; videos and the menu background (a looping video) move every frame; rain and lightning
are random. So:

1. **Closest frame.** Each test frame is compared with the golden frame of its scene that looks most like it
   (smallest mean difference on a quarter-size copy), and scored with MAE (mean absolute difference per channel,
   0-255), SSIM (8x8 windows on brightness), tile MAE (the worst of 16x9 tiles of 80x80 pixels) and the share of
   bad pixels (off by more than `bad_level`). Tight for still scenes, not checked for animated ones.
2. **Golden range** (the tolerance model for animated parts). Over all golden frames of the scene (the dense
   burst), each pixel has a lowest and highest value per channel. A test pixel that leaves that range by more
   than `env_tol` is "outside". Still parts (menu text, the HUD, a paused frame) have no range and must match;
   animated parts accept anything the golden frames showed. Scored as the share of outside pixels (`env_bad`)
   and the worst 80x80 tile of the distance beyond `env_tol` (`env_tile`: noise inside the tolerance adds
   nothing, so a small missing or wrong object stands out). Four settings shape it:
   - `env_anim_level` (default 16): a pixel counts as animated when its golden span is above this. At 16 the
     menu text over the moving background video stays still (its span is about 11), so missing text fails.
   - `env_flash_level` (default off; 6 for the storm scenes): golden frames whose mean brightness is more than
     this many levels from the scene's median (lightning) are left out of the range; they stay candidates for
     the closest frame. One flash in the burst would otherwise widen the range of every pixel.
   - `env_radius` (default 2): in and near the animated parts only, the range also takes in the values within
     this many pixels. A golden burst never shows every position of a raindrop or a flame; with a radius of a
     few pixels a test frame may draw it a little away from every golden position. Still parts keep their exact
     range, so a still object that moved or is missing still fails.
   - `ignore` (on the scene, not in `thresholds`): rectangles `[x, y, w, h]` in 1280x720 coordinates (scaled to
     the frame) that accept anything and count as equal in every score, for parts that cannot be compared at all.
3. **Draw list.** If both sides have a frame log for the scene, the canonical draw and resolve lines are diffed
   (Python `difflib` ratio, 1 = the same list), best golden frame per test frame. The report lists the shader
   pairs drawn more often on either side and links the diff.

A scene passes when at least `min_pass` of its test frames pass every check that has a threshold, and the draw
list (if any) reaches `drawlog`. Thresholds are in `scenes.json` (defaults, then per scene); `null` means not
checked. They were set from Xenos captures against the golden set, see "Calibration" below.

The report (`report.html`) starts with one PASS / FAIL badge and a table, one row per scene, every value next to
its limit and red when over. Failing scenes open below it with, per frame: the closest golden frame, the test
frame, the difference heat map and the outside-the-range heat map (black = same, white = 64 or more off; click
a frame for the full-size BMP, a heat map of a failing frame for its full-size PNG). The 640-pixel heat maps keep
the largest difference of each 2x2 block, so a one-pixel error still shows. Below the frames: the draw-list
similarity, problems found in the frame logs themselves, the shader pairs drawn more often on either side and the
first 400 lines of the draw-list diff (the whole diff is in `drawlog\<scene>.diff.txt`). `summary.json` beside it
has every number.

Shots may be BMP (the game's own 32-bit top-down files, or any plain 24 / 32-bit BMP) or PNG (any kind; alpha
is dropped). A scene folder may hold `shot_<tenths>.png` in place of `.bmp`, for example frames converted on
another machine; when both exist the BMP is used.

To compare frames outside the scene layout (two screenshots, or two folders):

```powershell
python -I native-renderer\tests\compare.py images <golden image or folder> <test image or folder> --out <folder>
python -I native-renderer\tests\compare.py images golden\vrex_140 run\vrex_140 --scene vrex_140 --set env_radius=3
```

Every image of the first set is a golden frame (one golden range over all of them), every image of the second is
scored against the closest of them. Thresholds are the defaults, a scene's with `--scene`, then `--set KEY=VALUE`
(`null` = not checked). With `--out` it writes heat maps and a `report.html` like the full compare.

## The frame log format (`REX_DEV_FRAME_LOG`)

`REX_DEV_FRAME_LOG=<seconds>[,<frames>]` makes the Xenos plugin (and, since phase 2b, the native plugin, see `backend.md`) (D3D12: `src/graphics/d3d12/command_processor.cpp`,
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
(the depth function) after the z state; D3D12 does not. Vulkan also writes `Frame log: draw <i> pipeline
placeholder (skipped)` (or `missing`) after a draw whose pipeline was not ready, so that draw was not drawn. The
compare tool reports per frame: such skipped draws, a `frame end` count that does not match the lines read, and
lines it does not understand. A frame without its `frame end` (log cut short) is left out. The log lines are found
by their `Frame log: ` text, so a log in another layout (a Linux build) works too. Field meanings, as the native
plugin should fill them:

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
python -I native-renderer\tests\compare.py framelog <golden framelog.txt> <test game.log> --min-ratio 0.98
```

With `--min-ratio` the diff exits 1 when the similarity is lower (for scripts).

Timing: the log's clock is not the scripts' clock. The runner asks for `scene time + clock_offset`, where
`clock_offset` (seconds from the log clock's start to the shot clock's zero) is measured in each run and stored
in `golden\_runs\<run>.json` (`frame_log.clock_offset`); the next run uses it. `meta.json` records when the log
was actually taken (`frame_log.scene_time`), and the report shows it. Measured: the log clock starts 7.65 to
7.78 s before the save menu opens and 0.2 s before the first pad read; with the stored offsets every frame log
landed within 0.15 s of the wanted scene time.

## Calibration and the self-consistency check

How the thresholds were set (10 October 2026, all Xenos on D3D12, the golden set above):

1. `xenos-calib1`, the first run against the golden set, showed what varies between runs: nothing in the still
   scenes (video, title, loading, pause match to the pixel); the menu background video (closest-frame MAE about
   1.1 to 1.4, a few pixels up to 12 levels outside the range); in V-Rex the drifting rain haze (up to about 25
   levels over the lit part of the frame, MAE 3.5 even between frames without lightning) and lightning; in Kong
   to the Rescue the V-Rex's own movement; particle draws in the gameplay frame logs (draw-list similarity 0.96
   to 0.99). From that: closest-frame checks only for still scenes and the Venture hull; `env_tol` 32 for the
   storm gameplay; `env_flash_level` 6; `min_pass` 0.6 where lightning comes at random; per-scene draw-list
   limits for gameplay (0.93 V-Rex, 0.97 Venture, 0.95 Kong).
2. `xenos-selfcheck1` and `xenos-selfcheck2`: the first passed 13 of 13; the second failed Venture (two frames
   beside a lightning flash) and nearly Kong, so their `env_bad` / closest-frame limits were widened once.
3. `xenos-selfcheck3`, a run not used for any setting: **13 of 13 scenes pass**. Its worst values (k-th worst
   where `min_pass` lets frames miss):

| Scene | MAE | SSIM | Tile MAE | Outside % | Worst tile outside | Draw list |
|---|---|---|---|---|---|---|
| video | 0.000 | 1.0000 | 0.0 | 0.000 | 0.00 | 1.000 |
| title | 0.003 | 1.0000 | 0.2 | 0.000 | 0.00 | 1.000 |
| save_menu | 1.261 | 0.9760 | 9.2 | 0.001 | 0.00 | 0.988 |
| main_menu | 2.078 | 0.9496 | 11.2 | 0.002 | 0.01 | - |
| chapter_select | 1.577 | 0.9617 | 11.0 | 0.027 | 0.08 | - |
| loading | 0.000 | 1.0000 | 0.0 | 0.000 | 0.00 | - |
| vrex_110 | 3.359 | 0.9432 | 20.6 | 0.018 | 0.08 | - |
| vrex_140 | 3.639 | 0.9278 | 20.8 | 0.074 | 0.36 | 0.995 |
| vrex_170 | 3.344 | 0.9341 | 19.9 | 0.003 | 0.01 | - |
| pause | 0.000 | 1.0000 | 0.0 | 0.000 | 0.00 | - |
| venture | 2.093 | 0.9311 | 7.4 | 0.000 | 0.00 | 0.988 |
| kong_cutscene | 7.066 | 0.6074 | 28.7 | 0.026 | 0.18 | - |
| kong | 9.564 | 0.7849 | 51.0 | 0.119 | 1.38 | 0.991 |

What the limits catch: each test frame of a passing capture was damaged one way at a time and compared again
(a scratch script; the same `compare_scene`). Caught everywhere except where noted: a black 96x96 square in the
middle (missed on the already black loading and pause frames, on the rocking Venture hull and in the Kong
fight), colours with red and blue swapped (missed on the loading screen, which is mostly black and white), a UI
element removed (Ubisoft logo, title logo, save menu DELETE, Venture hint, loading text; caught), brightness
x1.1 (missed in the storm scenes, pause, Venture and the Kong fight), every channel +6 levels (missed in the
storm scenes, both Kong scenes, the main menu and the chapter select) and a blur like wrong texture filtering
(missed in the storm scenes and the Kong cutscene). So: still scenes and menus are strict, the
storm and fight scenes catch gross errors only (missing or black objects, wrong colours), and small global
shifts are left to the still scenes, which share the same shaders for text, video and post effects.

To re-tune after a change: capture a run against the golden set, then
`python -I native-renderer\tests\compare.py calibrate --golden-root F:\KK-native-renderer\golden --test-root <run>`
prints the worst values per scene and suggested limits (worst x2, with floors); check them with one more run
that was not used for tuning.

## Render scale runs

The golden set is made at the console's size, and the capture is the presenter's image at the render size
(2560x1440 at `--native_render_scale 2`), so `compare.py` fails a scaled run on `size` (Phase 2b). Judge such a
run against the Xenos plugin's at the same integer scale (`--draw_resolution_scale_x=2
--draw_resolution_scale_y=2`, `-Plugin xenos -Out <folder>`), shot by shot; `backend.md`, "Render scale in the
game", has the numbers. Whether scaling itself is right is decided by the synthetic
`Scale_*` tests in `nr_game_tests` (`backend.md`, "Render scale"); the commands for a run on the game are in
that section too.

## Testing the compare tool

`tests/test_compare.py` checks the tool itself with synthetic frames and logs, no game or golden data needed:
BMP variants (the game's top-down 32-bit, bottom-up 24-bit with row padding, bitfields, Pillow's) and PNG
variants load to the same pixels; the scores react to a small missing object; the golden range accepts rain
streaks where the golden frames had them (and, with `env_radius`, near them) but not a missing still object, a
wrong colour or rain in a still part; `env_tile` ignores noise inside the tolerance; a flash frame left out of
the range (`env_flash_level`); `min_pass` lets one lightning frame of three miss but not two; the frame log
parser reads both backends' lines and reports skipped draws and bad counts; `collect`, `compare`, `images` and
`plan` run end to end on temporary folders.

```powershell
python -I native-renderer\tests\test_compare.py
```

## Adding a scene

Add a run (if the scene needs its own launch) and a scene to `scenes.json`; check the timeline first with
`KK_DEV_SHOTS` every second or two (the frames show what is on screen when); capture the golden frames for it
(`run.ps1 -Golden -Scenes <name>`), run it twice more against the golden set and set its thresholds from
`compare.py calibrate`.

## The chapter tour

`tests/tour.json` has a scene for every chapter-select entry (0-41), written by `tests/tourgen.py` from its
chapter table (do not edit the JSON by hand: change `MOMENTS` or `BATCHES` in `tourgen.py` and run
`python -I native-renderer\tests\tourgen.py`; `--check` prints each launch's timeline, `--table` the table
below). `run.ps1 -Tour` runs it; every other switch works as with `scenes.json` (`-Tour -Scenes tourB`,
`-Tour -Scenes tour27_kong_rescue`, `-Tour -Golden`). Golden frames are in `golden\tour<NN>_<name>\` beside
the 17 scenes, raw run folders in `golden\_runs\tour<A-D>-<time>`.

**How a launch plays several chapters.** From the chapter select a launch starts a chapter (RIGHT presses
0.6 s apart, A 2 s after the last), presses SKIP every 2 s from 4 to 40 s after A (Start only while a movie
plays, so a chapter's movies are skipped and the pause menu never opens by mistake; the SKIP presses stop a
second before the quit), takes the shots, and quits: START, seven UP presses 0.5 s apart, A (Quit game), A
(Yes, "You are about to lose your latest progress"). Yes goes straight back to the chapter select, its cursor
on the chapter just played, about 3 s later; the next moves start 13 s after the START. Found in the probes:

- The pause menu is a ring with the cursor on Resume. Most chapters have Options, Quit game, Start again,
  Resume, Score (UP twice reaches Quit game); The Log has only Options, Quit game, Resume (UP twice reaches
  Options and the first A opens it). Seven UP presses reach Quit game in both (7 = 2 mod 5 = 1 mod 3), and
  did in every chapter tried except Venture, where both two and seven UP presses ended on Options (its menu
  was not looked at; Venture is simply last in its launch, as The Log is in its own).
- The quit must come while the game is in play: START during a death fade or a checkpoint restart does
  nothing, the next chapter's presses then land in the old chapter, and every later chapter of the launch is
  one entry off. So the shots come early where the player dies when idle (Rapids, Fight, Kong, Chased by
  V-Rex, The Skull Islanders, To the Plane, Fight in the Lair) and the quit 3 s after the last shot.
- Kong's Death (40) is a cutscene that returns to the chapter select by itself (about 57 s after A; the
  tour waits 60 s and moves on without a quit). Credits (41) come last, without SKIP.
- Loads are short (3 to 7 s from A to the first frame of the chapter); Venture shows 60 s of black first.

Four launches (`tourA` 0-10, `tourB` 11-20, `tourC` 21-30, `tourD` 31-41) take about 35 minutes together (11,
8, 9 and 10 minutes from the save menu, plus about 40 s of start each). One launch for all 42 would save three
starts (2 minutes) but one missed quit would cost every chapter after it, so a launch is about ten chapters.
Each launch takes one frame log (`tour04_the_wall`, `tour18_swamps`, `tour22_save_ann`, `tour33_chased_by_kong`).

**Finding the moments** (probe files written by `tourgen.py --probe <file> --chapters ... --every 3
--stay 80`, run with `run.ps1 -ScenesFile <file> -Out <folder> -NoCompare -NoFrameLog`, contact sheets
looked at offline; probe data in `F:\KK-native-renderer\analysis\tour\`): five shots 0.5 s apart where the
picture holds still (Jack or Kong idle after the opening, the camera at rest), 30 s after A where the chapter
allows it. The thresholds start as those of chap04-chap33 (closest-frame checks off, golden range with
`env_tol` 32, `env_flash_level` 6, `min_pass` 0.6); scenes where the whole picture moves (the raft, the
fights, the credits) add `env_radius` 4.

The moments (seconds after the chapter's A; times in `tour.json` are seconds after the save menu opens). Kind: gameplay (the player's character in play, standing still unless noted) or cutscene (in-engine, including Skull Island's opening, which waits for the player). Every scene has five test shots 0.5 s apart and a golden burst every 0.2 s from 1 s before to 1 s after them (23 frames).

| Entry | Scene | Chapter | Run | Shots (s after A) | Kind | What |
|---|---|---|---|---|---|---|
| 1 | `tour01_skull_island` | Skull Island | tourA | 28, 28.5, 29, 29.5, 30 | cutscene | Ann's face leaning over Jack on the beach, the in-engine opening that waits for the player; birds cross the sky |
| 2 | `tour02_necropolis` | Necropolis | tourA | 30, 30.5, 31, 31.5, 32 | gameplay | a cave mouth against a pale sky, Ann and a sailor beside it (Jack still) |
| 3 | `tour03_scorpions` | Scorpions | tourA | 30, 30.5, 31, 31.5, 32 | gameplay | a dark stone passage with a carved wall (Jack still) |
| 4 | `tour04_the_wall` | The Wall | tourA | 30, 30.5, 31, 31.5, 32 | gameplay | a jungle clearing at dusk, a spiked wooden gate against the sky (as chap04) |
| 5 | `tour05_sacrifice` | Sacrifice | tourA | 40, 40.5, 41, 41.5, 42 | gameplay | bones and torch glow on the floor of a dark pit (Jack still) |
| 6 | `tour06_kongs_tracks` | On Kong's Tracks | tourA | 40, 40.5, 41, 41.5, 42 | gameplay | under a wooden scaffold in the rain, lightning lights it now and then (Jack still) |
| 7 | `tour07_hayes` | Hayes | tourA | 40, 40.5, 41, 41.5, 42 | gameplay | Hayes by a stone pillar under the scaffold, rain beyond (Jack still) |
| 8 | `tour08_vrex` | V-Rex | tourA | 40, 40.5, 41, 41.5, 42 | gameplay | Jack in the rain under the cave roof, random lightning (as the vrex scenes) |
| 9 | `tour09_ann` | Ann | tourA | 30, 30.5, 31, 31.5, 32 | gameplay | a cave mouth with shafts of light falling in (Jack still) |
| 10 | `tour10_kong` | Kong | tourA | 12, 12.5, 13, 13.5, 14 | gameplay | Kong crouching on a ledge by a ruined wall (Kong idle; Ann is caught at about 24 s, so the shots come before that) |
| 0 | `tour00_venture` | Venture | tourA | 89, 89.5, 90, 90.5, 91 | gameplay | the hull of the rocking ship with VENTURE painted on it and the Right stick to aim hint, after about 60 s of black (the same moment as the venture scene) |
| 11 | `tour11_canyon` | The Canyon | tourB | 30, 30.5, 31, 31.5, 32 | gameplay | a canyon path between mossy rocks, a pool beyond (Jack still) |
| 12 | `tour12_millipedes` | Millipedes | tourB | 30, 30.5, 31, 31.5, 32 | gameplay | a dark jungle path towards a lit gate in a wall of vines (as chap12) |
| 13 | `tour13_brontosaurus` | Brontosaurus | tourB | 30, 30.5, 31, 31.5, 32 | gameplay | a dark cave, a thin stream of water falling from the roof (Jack still) |
| 14 | `tour14_jimmy` | Jimmy | tourB | 20, 20.5, 21, 21.5, 22 | gameplay | a dark rock wall lit by a torch, Carl with his camera at the start (Jack still) |
| 15 | `tour15_raft` | On the Raft | tourB | 4, 4.5, 5, 5.5, 6 | gameplay | the raft on the river at the start, a torch in the foreground (the raft moves) |
| 16 | `tour16_rapids` | Rapids | tourB | 6, 6.5, 7, 7.5, 8 | gameplay | the raft going down the rapids in white water (moves; Jack drowns at about 20 s and the checkpoint restarts, so the shots come early) |
| 17 | `tour17_fight` | Fight | tourB | 6, 6.5, 7, 7.5, 8 | gameplay | Kong and a V-Rex facing each other in a misty clearing (fight; Kong dies at about 50 s) |
| 18 | `tour18_swamps` | Swamps | tourB | 20, 20.5, 21, 21.5, 22 | gameplay | Jack under a shelter, Jimmy and the sailors looking out over the swamp (Jack still) |
| 19 | `tour19_chased_vrex` | Chased by V-Rex | tourB | 6, 6.5, 7, 7.5, 8 | gameplay | Kong fighting raptors in the misty jungle, Ann behind him (Kong dies at about 25 s) |
| 20 | `tour20_the_log` | The Log | tourB | 55, 55.5, 56, 56.5, 57 | gameplay | after the log falls: a mossy rock wall over mist (as chap20) |
| 21 | `tour21_islanders` | The Skull Islanders | tourC | 12, 12.5, 13, 13.5, 14 | gameplay | Kong by a broken stone block in the ruins, Ann at his feet, raptors coming (Kong idle; he dies at about 40 s) |
| 22 | `tour22_save_ann` | To Save Ann | tourC | 30, 30.5, 31, 31.5, 32 | gameplay | a narrow rocky gorge with a stream, light at its end (Jack still) |
| 23 | `tour23_the_cave` | The Cave | tourC | 30, 30.5, 31, 31.5, 32 | gameplay | a dark wooden scaffold over a pool, figures behind it (Jack still) |
| 24 | `tour24_venatosaurus` | Venatosaurus | tourC | 30, 30.5, 31, 31.5, 32 | gameplay | a rocky stream bed, two figures in front of a pale rock beyond (Jack still) |
| 25 | `tour25_in_the_mud` | In the Mud | tourC | 30, 30.5, 31, 31.5, 32 | gameplay | carved stone pillars in the dark, rain beyond (Jack still) |
| 26 | `tour26_call_kong` | Call Kong | tourC | 30, 30.5, 31, 31.5, 32 | gameplay | a dark hall with a lattice of beams over pale water (Jack still) |
| 27 | `tour27_kong_rescue` | Kong to the Rescue | tourC | 13, 13.5, 14, 14.5, 15 | gameplay | Kong to the Rescue in play: Jack with a spear behind the rocks, a V-Rex, fires and rain (the same moment as the kong scene) |
| 28 | `tour28_to_the_plane` | To the Plane | tourC | 12, 12.5, 13, 13.5, 14 | gameplay | Jack crouching by a body in a mossy stone corridor at the start (he dies at about 48 s) |
| 29 | `tour29_to_the_lair` | To the Lair | tourC | 30, 30.5, 31, 31.5, 32 | gameplay | a great tree's roots in green mist (Jack still) |
| 30 | `tour30_kongs_lair` | Kong's Lair | tourC | 30, 30.5, 31, 31.5, 32 | gameplay | dark carved stone walls in Kong's lair (Jack still) |
| 31 | `tour31_lair_fight` | Fight in the Lair | tourD | 3, 3.5, 4, 4.5, 5 | gameplay | Kong crouching in the lair as the giant snake comes at him, a waterfall behind (fight; Kong is hurt from about 6 s and dies at about 30 s) |
| 32 | `tour32_free` | Free! | tourD | 30, 30.5, 31, 31.5, 32 | gameplay | a very dark rock face, water dripping in front of it (Jack still) |
| 33 | `tour33_chased_by_kong` | Chased by Kong | tourD | 30, 30.5, 31, 31.5, 32 | gameplay | a cave with a water channel, dark green rock (as chap33) |
| 34 | `tour34_heading_back` | Heading Back | tourD | 30, 30.5, 31, 31.5, 32 | gameplay | under a torch-lit wooden roof on stone posts, a dark gap beyond (Jack still) |
| 35 | `tour35_back_village` | Back to the Village | tourD | 30, 30.5, 31, 31.5, 32 | gameplay | worn stone steps going up through dark rock, water dripping (Jack still) |
| 36 | `tour36_kong_capture` | Kong's Capture | tourD | 30, 30.5, 31, 31.5, 32 | gameplay | Kong from behind in a misty gorge, a wall of rock and roots beyond (Kong idle) |
| 37 | `tour37_kong_struck` | Kong Struck Down | tourD | 30, 30.5, 31, 31.5, 32 | gameplay | the Venture at anchor in the bay, seen from the rocks of the shore at dusk (Jack still) |
| 38 | `tour38_new_york` | In the Streets of New York | tourD | 20, 20.5, 21, 21.5, 22 | cutscene | Kong in chains on the dark theatre stage, before the lights come up at about 33 s |
| 39 | `tour39_empire_state` | The Empire State Building | tourD | 30, 30.5, 31, 31.5, 32 | gameplay | a New York rooftop at night, lit windows in the building beside it (Kong still) |
| 40 | `tour40_kong_death` | Kong's Death | tourD | 17, 17.5, 18, 18.5, 19 | cutscene | the closing cutscene: Carl Denham standing by Kong's head in the street; it goes back to the chapter select by itself at about 57 s |
| 41 | `tour41_credits` | Credits | tourD | 3, 3.5, 4, 4.5, 5 | cutscene | the credits over a misty mountain in the jungle, the title lines at the start (text scrolls) |

### Results (12 October 2026)

**Golden set**: captured 11 October 23:06-23:53 with the Xenos plugin (D3D12, `kk-dev` of this branch):
42 scene folders `golden\tour00_venture` .. `golden\tour41_credits`, 23 frames each (five test shots and the
burst), 966 frames of 1280x720, about 4.2 GB; raw run folders `golden\_runs\tour<A-D>-*`. Launch A was
captured twice (the first time Venture's quit went to Options and every later chapter landed in the Options
screen) and launch D twice (a 23 s freeze just after the save menu opened put every press late; that is what
`KK_DEV_FREEZE_SKIP` is for). Checked by eye on one frame per scene (all 42 show their chapter's moment).

**Xenos self-check** (`runs\tour-xenos-check3`, 12 October 06:08-07:54, the `kk-dev` build after the merge
of nr-integration): 36 of 42 passed with the starting limits. The six that did not repeat:
`tour02_necropolis` (Ann's idle animation), `tour10_kong` (Kong breathing, debris settling), `tour11_canyon`
(the fog over the pool brighter in the golden run), `tour16_rapids` (the white water), `tour21_islanders`
(Kong's fight with the raptors is at another moment in every run) and `tour22_save_ann` (Jimmy's animation).
Their limits were widened to twice the worst value of that run (`SELF_CHECK_LIMITS` in `tourgen.py`); with
them the run passes 42 of 42 (`report-tuned`). That is the run the limits came from, so it is not an
independent check: a second Xenos run that was not used for tuning is still to do. `tour21_islanders` is a
gross check only (30 % outside, worst tile 250): its moment should be moved to before the fight.

**Native plugin** (`runs\tour-native3`, 12 October 04:21-06:00, `kk-dev-vulkan` after the merge, default
options plus the harness's `--native_pipeline_cache=false --native_pipeline_list=false
--native_vk_pipeline_cache=false`, the developer shader pack and DXC beside the game): **36 of 41 pass**
against the tuned limits, and Venture is missing (the first version of `KK_DEV_FREEZE_SKIP` took Venture's
57 s load without pad reads for a freeze and moved its shots past the end of the run; now limited to the
menus). Full report: `runs\tour-native3\report-tuned\report.html`.

| Entry | Scene | Limits (outside % / tile) | Xenos self-check: frames, worst outside % / tile (MAE) | Native, default options |
|---|---|---|---|---|
| 0 | `tour00_venture` | 1 / 1.5 | pass 4/5, 0.01 % / 0.07 (MAE 3.90) | missing |
| 1 | `tour01_skull_island` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.73) | pass 5/5, 0.00 % / 0.00 (MAE 1.26) |
| 2 | `tour02_necropolis` | 3.3 / 28 | pass 5/5, 1.71 % / 14.15 (MAE 1.98) | pass 5/5, 0.19 % / 2.04 (MAE 1.28) |
| 3 | `tour03_scorpions` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.01 (MAE 0.48) | pass 5/5, 0.00 % / 0.01 (MAE 0.64) |
| 4 | `tour04_the_wall` | 0.2 / 1.5 | pass 5/5, 0.14 % / 0.61 (MAE 0.92), draw list 0.995 | **fail** 0/5, 0.56 % / 2.63 (MAE 2.52), draw list 0.988 |
| 5 | `tour05_sacrifice` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.33) | pass 5/5, 0.00 % / 0.00 (MAE 0.29) |
| 6 | `tour06_kongs_tracks` | 0.2 / 1.5 | pass 5/5, 0.01 % / 0.20 (MAE 2.26) | pass 3/5, 1.01 % / 0.64 (MAE 5.08) |
| 7 | `tour07_hayes` | 0.2 / 1.5 | pass 5/5, 0.04 % / 0.30 (MAE 1.21) | pass 5/5, 0.02 % / 0.15 (MAE 0.94) |
| 8 | `tour08_vrex` | 0.2 / 1.5 | pass 5/5, 0.03 % / 0.26 (MAE 6.39) | pass 4/5, 0.24 % / 2.28 (MAE 6.09) |
| 9 | `tour09_ann` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.03) | pass 5/5, 0.00 % / 0.01 (MAE 0.26) |
| 10 | `tour10_kong` | 0.55 / 3.2 | pass 5/5, 0.27 % / 1.56 (MAE 2.40) | pass 5/5, 0.31 % / 1.60 (MAE 2.75) |
| 11 | `tour11_canyon` | 2.7 / 11.4 | pass 5/5, 1.33 % / 5.66 (MAE 5.39) | pass 5/5, 0.16 % / 0.57 (MAE 2.82) |
| 12 | `tour12_millipedes` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.00) | pass 5/5, 0.00 % / 0.02 (MAE 0.03) |
| 13 | `tour13_brontosaurus` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.62) | pass 5/5, 0.00 % / 0.02 (MAE 0.60) |
| 14 | `tour14_jimmy` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.20) | pass 5/5, 0.00 % / 0.01 (MAE 0.52) |
| 15 | `tour15_raft` | 0.2 / 1.5 | pass 5/5, 0.18 % / 1.27 (MAE 6.89) | pass 4/5, 0.12 % / 2.04 (MAE 8.01) |
| 16 | `tour16_rapids` | 7.1 / 20.2 | pass 3/5, 9.02 % / 48.37 (MAE 21.47) | pass 4/5, 10.06 % / 31.83 (MAE 18.98) |
| 17 | `tour17_fight` | 0.2 / 1.5 | pass 5/5, 0.12 % / 0.63 (MAE 9.91) | pass 4/5, 0.21 % / 0.51 (MAE 10.62) |
| 18 | `tour18_swamps` | 0.2 / 1.5 | pass 5/5, 0.10 % / 0.28 (MAE 2.68), draw list 0.997 | **fail** 0/5, 0.58 % / 1.55 (MAE 5.16), draw list 0.996 |
| 19 | `tour19_chased_vrex` | 0.2 / 1.5 | pass 5/5, 0.01 % / 0.02 (MAE 8.75) | pass 5/5, 0.01 % / 0.08 (MAE 8.53) |
| 20 | `tour20_the_log` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 1.11) | pass 5/5, 0.00 % / 0.01 (MAE 1.41) |
| 21 | `tour21_islanders` | 30 / 250 | pass 5/5, 14.70 % / 124.45 (MAE 27.39) | pass 5/5, 14.50 % / 124.47 (MAE 27.23) |
| 22 | `tour22_save_ann` | 2.2 / 13 | pass 5/5, 1.11 % / 6.46 (MAE 5.43), draw list 0.991 | pass 5/5, 0.40 % / 3.25 (MAE 4.09), draw list 0.996 |
| 23 | `tour23_the_cave` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.87) | pass 5/5, 0.00 % / 0.01 (MAE 0.85) |
| 24 | `tour24_venatosaurus` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.02 (MAE 0.17) | **fail** 0/5, 0.22 % / 0.38 (MAE 2.39) |
| 25 | `tour25_in_the_mud` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.35) | **fail** 0/5, 0.51 % / 3.02 (MAE 2.70) |
| 26 | `tour26_call_kong` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.01) | pass 5/5, 0.00 % / 0.00 (MAE 0.28) |
| 27 | `tour27_kong_rescue` | 0.6 / 6 | pass 5/5, 0.22 % / 4.48 (MAE 7.46) | pass 3/5, 1.39 % / 6.18 (MAE 8.00) |
| 28 | `tour28_to_the_plane` | 0.2 / 1.5 | pass 5/5, 0.04 % / 0.27 (MAE 0.52) | pass 5/5, 0.04 % / 0.15 (MAE 0.66) |
| 29 | `tour29_to_the_lair` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.11) | pass 5/5, 0.00 % / 0.00 (MAE 0.12) |
| 30 | `tour30_kongs_lair` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.02) | pass 5/5, 0.00 % / 0.00 (MAE 0.14) |
| 31 | `tour31_lair_fight` | 0.2 / 1.5 | pass 4/5, 0.10 % / 2.26 (MAE 7.15) | pass 4/5, 0.31 % / 1.87 (MAE 10.48) |
| 32 | `tour32_free` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.08) | pass 5/5, 0.00 % / 0.00 (MAE 0.38) |
| 33 | `tour33_chased_by_kong` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.67), draw list 0.993 | pass 5/5, 0.00 % / 0.00 (MAE 0.77), draw list 0.992 |
| 34 | `tour34_heading_back` | 0.2 / 1.5 | pass 5/5, 0.13 % / 1.21 (MAE 1.61) | pass 5/5, 0.00 % / 0.03 (MAE 0.68) |
| 35 | `tour35_back_village` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.03 (MAE 2.33) | pass 5/5, 0.00 % / 0.00 (MAE 0.77) |
| 36 | `tour36_kong_capture` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.02 (MAE 0.26) | pass 5/5, 0.01 % / 0.05 (MAE 0.36) |
| 37 | `tour37_kong_struck` | 0.2 / 1.5 | pass 5/5, 0.01 % / 0.17 (MAE 3.25) | pass 5/5, 0.04 % / 0.58 (MAE 4.35) |
| 38 | `tour38_new_york` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.14) | pass 5/5, 0.00 % / 0.00 (MAE 0.24) |
| 39 | `tour39_empire_state` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.00 (MAE 0.08) | pass 5/5, 0.00 % / 0.00 (MAE 0.20) |
| 40 | `tour40_kong_death` | 0.2 / 1.5 | pass 5/5, 0.00 % / 0.02 (MAE 0.92) | pass 5/5, 0.00 % / 0.03 (MAE 1.11) |
| 41 | `tour41_credits` | 0.2 / 1.5 | pass 5/5, 0.06 % / 1.02 (MAE 0.28) | **fail** 2/5, 0.24 % / 2.82 (MAE 1.83) |

**Native failures** for the native renderer agent (panels golden | native | outside-the-range map, one per
scene, in `F:\KK-native-renderer\analysis\tour\native3_failures\`; the best whole-pixel shift between native and
golden frames measured with a scratch script):

| Scene | What differs | Shift |
|---|---|---|
| `tour04_the_wall` | the chap04 camera again: the whole picture one pixel off vertically, every frame; draw list 0.988 (draw order sorted by distance) | dy 1 (MAE 2.4 to 0.9 when shifted) |
| `tour18_swamps` | camera one pixel off vertically, and the sailors' idle poses differ (Jimmy's camera on its tripod in another place) | dy -1 (5.4 to 3.1) |
| `tour25_in_the_mud` | camera two pixels off vertically; streaks of falling water in front of the left pillar where the golden frames have none | dy -2 (2.6 to 0.9) |
| `tour24_venatosaurus` | no whole-pixel shift, but fine texture-scale differences over the whole picture and the sailors a little apart: a sub-pixel camera offset like chap04's half pixel, or the sailors' timing | none (2.5) |
| `tour41_credits` | 2 of 5 frames pass: the scrolling credit lines are a frame or two out of step with every golden frame (the text's edges outside the range); the picture itself matches | none |
| `tour00_venture` | not captured in this run (the freeze-skip bug above, harness side); passes 4 of 5 in the second native run below | - |

**Second native run**: the `-RecordPipelines` run (`runs\tour-native-record3`, 12 October 08:09-09:41, the
same build and options with the pipeline cache recording into an empty file per launch, so nothing is
prewarmed) compared the same way: **37 of 42 pass**. Venture passes there (4 of 5). The same four fail again
(`tour04_the_wall` dy 1, draw list 0.983; `tour18_swamps`; `tour24_venatosaurus`; `tour25_in_the_mud`), the
credits pass, and `tour31_lair_fight` fails (1 of 5: the snake fight a little out of step, MAE 14.8). So the
four camera scenes are the native renderer's open failures; the credits and the lair fight are timing that
comes and goes.

The common thread of the first four is the one found for chap04 and chap12 in Phase 2c (backend.md, "Open
problems"): the player's character lands or settles a little differently when the render thread's timing
differs in the chapter's first frames, and the camera follows him. With the render thread chap12 now matches,
but these chapters (and chap04) still do not; they are where to look next. No scene shows missing objects,
wrong colours or black frames: every failure is position or timing.

**Pipelines of every chapter** (`-RecordPipelines`, same run): one file per launch, collected in
`runs\tour-native-record3\pipelines\tour<A-D>.pipelines` (1529, 1398, 1018 and 989 records) and merged with
`nr_pipelines merge` (standalone build, `F:\KK-native-renderer\nr-standalone\tools`) into `F:\KK-native-renderer\analysis\tour\pipelines\tour-all.list`: **3214 pipelines** (all at 1:1, 21 without a
pixel shader; 1268 vertex and 605 pixel shader translations). Launch B added 846 new ones to A's, C 421, D 418,
so later chapters still bring new pipelines. The list is game data and stays out of git. Not in it: the title
and menus are in every launch, but the render scales other than 1 and the launcher's prewarm are not (one run
per scale would add them).

Scenes that passed with noticeably higher numbers than the Xenos run (worth a look, not failures):
`tour06_kongs_tracks` and `tour08_vrex` (3 and 4 of 5 frames: lightning), `tour27_kong_rescue` (3 of 5, the
fight as in the `kong` scene), `tour31_lair_fight` (MAE 6.2 against 3.8).

## Later: Linux on the Ally

The same compare tool works on frames from a Linux run (the BMPs and the game log are the same). A runner for the
ROG Ally over SSH would copy the build, run the game with the same environment, and copy `shot_*.bmp` and the log
back (the main project's `ally_*.sh` scripts show the pattern); then `compare.py collect` and `compare`.
