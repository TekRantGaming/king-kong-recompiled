# The launcher instead of the game's menus (Beta 3 plan)

The user's request (13 Oct 2026): the game's own main menu is never used again. The launcher's Play opens a
Play menu (Continue first when a save exists, New game, Load game, Chapter select); the game then starts
straight in the chosen place. In play, a new pause menu in the launcher's style replaces the game's: all of the
game's pause items working, plus Settings (Display, Graphics, Controls with remapping, Cheats) that change on
the fly. Built on `beta3` (Beta 2 + upscalers + the chapter-jump research). Research and proofs:
`chapter-jump.md`.

User decisions: every chapter is always open in the launcher's chapter select and the "All chapters" cheat
(`kk_cheat_chapters`, KKst0ry) goes; the pause menu shows only settings that change live (render quality,
resolution, window mode, upscaler, frame rate cap, renderer, language and anything else that needs a restart
stay in the launcher only).

## What the player sees

### Launcher: Play

Play on Home opens the **Play menu** (a launcher page in its existing style, game art backdrop):

1. **Continue** (top, only when a save exists): the most recently saved slot, with its name, chapter and time.
2. **New game**: picks a slot (an empty one first; a used one asks "Overwrite <name>?", after a backup), then
   starts the story from the beginning, as the game's own new game.
3. **Load game**: the five slots (name, chapter reached, last saved, total score); A loads and continues,
   X deletes (asks first, backs up first).
4. **Chapter select**: all 42 entries, every one open: the chapter's name (and picture if the game files have
   one), its best score in the current save, grouped the way the game groups them (Jack / Kong where it
   applies). Starts the entry with the current save loaded (the most recent one; the save is picked on the
   Load game page); with no save at all it first creates one (as New game).
5. **Back**.

Whatever is chosen, the game starts with no logos, no title screen and no menus.

### In game: the pause menu

Start (Esc) in play opens the port's pause menu instead of the game's: launcher look (same fonts, colours,
focus highlight, controller / keyboard / mouse prompts), over the frozen, dimmed game frame:

- **Resume**, **Start again**, **Score**, **Options**, **Quit game**: everything the game's pause menu has, doing
  what the game's items do (the game's own page code and confirmations, answered by the port). Quit game saves
  as the game does, then goes back to the launcher (the process restarts into the launcher's Play menu).
- **Settings**: the launcher's Display, Graphics, Controls (with remapping) and Cheats pages, showing only what
  can change live. Changes apply at once and are saved like in the launcher.
- Start in a cutscene still skips it (not a pause). Loading screens: no menu.

### What changes elsewhere

- The "All chapters" cheat and its code go; the game's own chapter lock is not used (it is never shown).
- After a chapter-select run ends (Score pages, then the game would load its menus), the port takes the player
  back to the launcher's chapter select. Story runs (Continue / New game) go on to the next chapter as the game
  does.
- Saves: every write or delete by the port is preceded by `BackupSaves`. The game deletes a save when it saves
  with a continue level of 0 (chapter-jump.md "Mistakes"): the port never lets that happen (it sets the
  continue point before any save the game makes after a jump, or blocks the save).

## How it is built

Contract: `kk/src/play_session.h` (save list/backup/delete, the start request, chapters, the pause actions).

| Part | Owner | Files |
|---|---|---|
| Game side: production version of the research (`dev_chapter_jump.cpp`, `dev_pause.cpp`): load a save from its file on the title screen and jump (continue / new game / chapter), new game creating a save, chapter-end and Quit routing back to the launcher, pause takeover (no game pause page, input blocked, frozen frame kept on screen), live settings, save safety, removing the chapters cheat | Claude agent "menus-game" | `play_session.cpp`, `pause_hooks.cpp` (new), `cheats.*`, `settings.*`, `king_kong_app.h`, `input_remap.cpp`, `overlay.cpp` |
| UI side: the Play menu, Load game, New game, Chapter select pages in the launcher; the pause menu as its own screen in the launcher's style, reusing the launcher's Display/Graphics/Controls/Cheats items with restart-only rows hidden | Claude agent "menus-ui" | `launcher.cpp`, `launcher_ui.*`, `pause_menu.cpp` (new) |
| Chapter names (all five languages) and any chapter pictures in the game files; translations of every new string; reviews of each part | Codex (cloud), local model for log summaries and text checks | reports only, translations on its own branch |
| Plan, contract, merging, builds, test build, verifying everything | Claude (coordinator) | this file |

Branches: `beta3-game` and `beta3-ui` (worktrees `wt\beta3-game`, `wt\beta3-ui`) from `beta3`; the coordinator
merges them into `beta3`. Each agent may stub the other side's functions in its own branch to build and test,
but keeps the contract; contract changes go through the coordinator.

## Done when

A test build (`testbuilds\v2.0.0-beta.3-menus-<time>`) where, windowed and checked with the launcher's own
captures and game frames: Continue, New game, Load game (load and delete) and every kind of chapter start work
from the launcher with no game menu ever shown; the pause menu opens in play, every item works the game's way,
Settings change live; Quit returns to the launcher; saves made this way load and keep working; saves are backed
up before every change and none is ever deleted by the game.

## UI (menus-ui, branch `beta3-ui`)

Files: `launcher.cpp` (Play menu and its pages, `SettingsPages`, `InGameSettings`), `launcher_ui.*` (`ui::Input`
can be fed the game's pad), `ingame_settings.h`, `pause_menu.*`, and `play_session_stub.cpp` (built only while
`play_session.cpp` does not exist; made-up saves with `KK_DEV_FAKE_SAVES=1`, the pause "opening" after
`KK_DEV_PAUSE_MENU=<s>` seconds, `KK_DEV_PAUSE_NOSAVE=1`; it never touches a file). Captures (windowed,
PrintWindow of the test window): `F:\KK-native-renderer\runs\menus-ui-1013-*`.

### The launcher

- **Play** on Home (and Start / P on any page) opens the **Play menu**: Home's look (clear stills, the logo, the
  large entries), the header `PLAY` instead of the tabs. Continue (only with a save: the one saved last), New
  game, Load game (greyed with no save), Chapter select, Back. The status row shows what the focused entry does:
  Continue its save, chapter (continue point) and when it was saved; New game the free slots; Chapter select the
  save it uses. B goes back to Home.
- **Load game** (`PLAY - LOAD GAME`, the settings pages' layout): the five slots as tall rows (name, slot,
  chapter, when; total of best scores at the right; empty slots dim). The description lists chapter reached,
  continue point, last saved, started and the score total. A continues the save, Y opens Chapter select with
  it, X deletes it (pop-up, Cancel focused; `play::DeleteSave`, which backs up first).
- **New game**: Name (typed with the keyboard in a pop-up, 15 ASCII characters, empty = the game's own name),
  then the slots, empty ones first (focus on the first empty slot). An empty slot starts at once; a used one asks
  "Overwrite <name>?" (Cancel focused).
- **Chapter select**: all 42 entries, every one open, in groups of seven, each with its best score in the save it
  starts with (header right: `SAVE: <name>`; Y changes the save when there are several). The right side shows a
  card with the chapter's number (the place for its picture), its name, best score and which save it starts
  with (with none: "a new one is made in slot N first").
- Every choice calls `play::SetStartRequest` and then Play as before (the frame-rate warning, the shader wait, the
  restart for the renderer or a temporal upscaler).
- `--kk_launcher_open=play|chapters` (never saved) opens the launcher on the Play menu or Chapter select: for the
  game side when it goes back to the launcher.
- The Cheats page no longer has "All chapters".

### The pause menu

`CreatePauseMenu` (`pause_menu.h`) makes one ImGui dialog for the session (the launcher makes it at Play; the app
should also make it when the launcher is skipped). While `play::PauseMenuOpen()` it draws over the dimmed frame:
the game's logo, `PAUSED`, and Resume, Start again and Score (when `PauseActionAvailable`), Options, Settings, Quit
game. Start again and Quit game ask first (Cancel focused); B, Esc or Start resume. Options shows the port's
settings (Gameplay) until the game's own Options page is known.

**Settings**: tabs Display, Graphics, Gameplay, Controls (with Button remapping and Keyboard bindings), Cheats,
the launcher's own rows (`SettingsPages`, shared code) with the rows that need a restart left out (`LiveOnly`:
window mode and size, resolution, refresh rate, monitor, frame rate, VSync, upscaler and its mode, render quality,
internal resolution, look, preset, renderer, language, startup logos). Changes are saved like in the launcher.
Controls: the game gives the menu the pad it read (`PauseMenuPad`); keyboard and mouse come through ImGui.

## Game side (menus-game, branch `beta3-game`)

Files: `play_session.cpp` (the contract's saves, chapters and start request; routing back to the launcher; save
guards), `pause_hooks.cpp` (the pause takeover, the frozen frame behind the menu, the developer stand-in),
`play_internal.h` (what they share with the app), and changes in `king_kong_app.h`, `input_remap.cpp`,
`overlay.cpp`, `intros.cpp`, `cheats.*`, `settings.*`, `glyphs.*`, `dev_chapter_jump.cpp`, `dev_pause.cpp`. The
research code is gone from the dev files (they keep the traces only). Evidence: run folders
`F:\KK-native-renderer\runs\menus-game-1013-*` (the game's own frames `shot_*.bmp`, PrintWindow captures of the
test window `printwindow_<tenths of s>_<pid>.png`, `game.log`).

### Saves (`ListSaves`, `MostRecentSave`, `BackupSaves`, `DeleteSave`)

- The saves are `<user data>\<profile>\555307D3\00000001\KKProfileSlot<n>.sav\KKProfile.dat` (the runtime's
  profile folder `B13EBABEBABEBABE`, else any 16-digit profile folder that has the game's saves). A slot exists
  when its file is `0x8040` bytes with a continue level other than 0 (what the game itself treats as empty).
  Header fields as in chapter-jump.md; best scores exist for entries 0 to 39 only (the header has 40), entries
  40 and 41 report 0.
- `BackupSaves` copies the whole `555307D3` folder (saves and their XContent headers) to
  `<user data>\save-backups\<yyyyMMdd-HHmmss>\<profile>\555307D3\...` with a `reason.txt`; when nothing changed
  since the newest backup it keeps that one instead of making another; it keeps the newest 20.
- `DeleteSave` backs up first, then moves the slot's folder and header file to
  `save-backups\deleted-<time>-slot<n>\` (out of the game's sight, never erased).
- Every start request backs up the saves before the game can write (`before play (<kind>)`).
- Save guards: the save routine (`sub_8266AD80`) never runs with continue level 0 (it gets the slot's own
  continue point back: the save system's header, else the file, else the new-game start); the save job
  (`sub_82722620`, the one that calls `XamContentDelete` for level 0) gets the file's continue point put back in
  its header first. So the game never deletes a save.

### The start request

`--kk_start=<request>` (or `play::SetStartRequest` in the same process; `KK_DEV_START` in developer builds):
`continue:<slot>`, `new:<slot>[:<name>]`, `chapter:<slot>:<entry>[:<name>]` (names percent-encoded; never
saved). `restart_and_play` passes it on. It is carried out on the title screen, 5 frames after the title's level
(2) has loaded (a fallback starts it 3 s after the chapter table is filled), in one frame; the screen is black
from the start until the chosen level is loading (`CoverScreen`, the ImGui layer), and the logo movies are
skipped.

- **Continue**: the slot's file into the save system and the universe (as chapter-jump.md proved), then its
  continue point with `G+0x5668 = 0`, as the main menu's Play does (100 / -1 becomes 100 / 0 and `G+0x1C = 1`,
  the new-game start, as the load page does). Continue with no slot takes the most recent save; with no save at
  all it becomes New game.
- **New game**: what the save menu's "Create new" does (`IntMIG_Page_Memcard` at `0x824CB7C4`): the slot's header
  cleared and named (the name given, ASCII, 31 characters; empty gives "Player <n>"; the game types its names
  letter by letter with `sub_82413040`, so there is no default of its own), `sub_82413978(slot, 2, 0, 100, -1,
  0)`, the universe re-init, `sub_825EAB98` (with the script globals pointed at the universe for the call: on the
  title screen they are still 0, and the call crashed without), the save data snapshot, continue point 100 / -1
  and the slot; then the save job (`0x20000060`) writes it before the level load, and the story starts as
  Continue does. A used slot is overwritten after the backup.
- **Chapter**: the slot's save (or a new one in an empty slot), then what the chapter select's A does (the
  snapshot, the restart copies, the re-init, the slot and continue point again from the header, `G+0x5668 = 1`,
  `G+0x5670`), then the load page's request.
- The launcher's cheats are applied once the chapter's level is up (`play::ApplyCheatsNow`), since the start
  menu where they used to be applied never shows.

### Back to the launcher

In a session started by a request, the game's request for its start menu (level 3) is cancelled and the program
restarts into the launcher once no save is being written: `--kk_launcher_open=chapters` when a chapter-select run
reached its end (after the Score pages and the save question), `--kk_launcher_open=play` after Quit game. The
restart drops `--kk_skip_launcher`, `--kk_start` and `--kk_launcher_open` from the command line and closes the
window the way the close button does. Story runs go on to the next chapter as the game does.

### The pause takeover

As described at the top of `pause_hooks.cpp`. The game's own pause draws a black screen; the last frame of play
is captured as the pause starts (`CaptureGuestOutput`, about 20 ms at 2560x1440) and drawn under every dialog
while the port's menu is open (`CreatePauseBackdrop`), so the pause menu sits over the frozen frame. While it is
open the game reads an empty pad (buttons still held when it closes stay hidden until let go), the pad it read
goes to the pause menu (`kk::PauseMenuPad`), and mouse look is switched off (the runtime lets the cursor go).

The game's pause items (found in `IntMIG_Page_Main`'s list, `0x824BA630`): chapter-select runs have Options (2),
Quit game (3), Start again (8), Resume (4), Score (0); story runs with a save have **Save (1)**, Change storage
device (1000), Options (2), Quit game (3), Resume (4). (chapter-jump.md had Score as 7 and Options as 5: 7 is an
extra item shown with profile flag `0x40`, not identified.) `PauseAction::kSave` was added to the contract for the
story runs' Save; Change storage device has no meaning on PC. Score and Save show the game's own pages (drawn and
read by the game; the port's menu comes back when they close). Start again restarts the chapter without a level
load (its opening cutscene again).

The game's **Options** item opens `IntMIG_Page_OptStart` with the pages Audio (`_Audio`), Video (`_Video`),
Controls (`_Control`) and Language (`_Langue`); see the table below.

### Settings in the pause menu: live or restart

From where each setting is read (code), and the ones a test changed in play (run given). Live rows can stay in the
pause menu's Settings; the others stay launcher-only (the user's decision).

| Setting (cvar) | In play | Where it is read |
|---|---|---|
| Camera response, sensitivity (`kk_camera_modern`, `kk_camera_sensitivity`) | live | every camera turn (`input_remap.cpp`) |
| Dead zone, stick inversion, toggle aim (`kk_deadzone`, `kk_invert_*`, `kk_toggle_aim`) | live | every pad read |
| Button remapping (`kk_map_*`) | live | every pad read |
| Vibration and its strength (`kk_vibration`, `kk_vibration_strength`) | live | every rumble the game sends |
| Mouse sensitivity and inversion (`kk_mouse_sensitivity`, `kk_mouse_invert_y`) | live | every mouse turn |
| Input mode (`mnk_mode`), keyboard bindings (`keybind_*`) | live | the runtime's keyboard driver checks them at every read |
| Field of view (`kk_fov`) | live | every camera set-up (`fov.cpp`) |
| Fog, motion blur, full-screen blur (`kk_fog`, `kk_motion_blur`, `kk_big_blur`) | live | every draw of the effect |
| Camera shake and its strength | live | every shake |
| Button prompts (`kk_button_prompts`) | live (new) | the prompt thread now follows the setting (and puts the game's own sheet back for Xbox 360) |
| Cheats (`kk_cheats`, `kk_cheat_*`) | live (new) | applied when the chapter's level is up; changes in play apply at once: flag cheats switched off are cleared again (only the ones the port set), weapons and ammo given stay until the next chapter or checkpoint |
| Aspect ratio (`present_letterbox`) | live | the presenter, every paint |
| Ambient occlusion (`ao_mode`, `ao_strength`) | live | native renderer every frame; F8 / F9 already change it |
| Frame counter, achievement pop-ups and sound | live | every frame / every unlock |
| Anti-aliasing (`swap_post_effect`), texture filtering (`anisotropic_override`) | restart | read once when the renderer starts (both renderers: `plugin_settings.cpp` `ReadGameSettings`, the Xenos plugin's graphics system) |
| Render quality and scale, resolution, window mode, monitor, refresh rate, VSync, frame rate cap, upscaler, renderer, language, Original look, startup logos | restart | launcher-only (decided) |

The game's own Options pages hold audio volumes and other values kept in the save; see "The game's Options"
below.
