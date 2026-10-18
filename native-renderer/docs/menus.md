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
5. **Extras**: the original game's bonuses (artwork galleries, interviews, the trailer, the alternative end, the
   bonus video modes), unlocked by the chosen save's progress and scores.
6. **Back**.

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
- Extras (task 204): the Weta galleries get the same menu. The game itself leaves a gallery on Start
  (IntMIG_wait asks for the start menu while G+0x5680, the Extras row, is set); the port hides the row from that
  one check so the game's pause page opens. Galleries and the credits drop the save slot (G+0x4E10 = -1) after
  loading the save's video modes, so there is no Save item and Quit has no save page. The alternate ending is a
  normal level with the game's pause. Movies and the credits have no pause: Start skips them back to Extras.

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

Home is the main menu (task 144, owner 16 Oct: a tidy top level): Play, Update to vX (only when one was found),
Settings, Achievements, About, Quit. Settings has eight tabs (LB / RB or a click, wrapping round): the seven
categories of `kk/src/settings_layout.h` (Display, Graphics, Audio, Controls, Gameplay, Cheats, Advanced), then About
(owner, 16 Oct evening), with `SETTINGS` at the right. About is also on Home as a page of its own (same rows, no tabs);
Achievements is a page of its own with its name as the header. B goes back to Home from each. A benchmark started
from Settings > About comes back to that tab with its results (`kk_benchmark_from`), one started from Home's About to
Home's About.
Reinstall game and Check for updates now are on About; Graphics > Advanced has Rebuild shaders (asks first, then
the shader screen with its one progress bar). The online player count is explained and switched in the first
setup and on About; no reminder opens by itself. What's new never opens during the first-time setup or over the
New in this update screen; it opens once on the main menu after Finish setup (new install) or after that screen's
Continue (a player who updated), with the game files installed and the shaders prepared (`launcher_policy.h`).

**First-time setup and New in this update (job J6, 17 Oct; spec `codex/state/setup-flow.md`).** The setup's pages
(SetupStage numbers in `launcher.cpp`): 0 disc image, 1 install, 2 artwork, 4 shaders, 6 HD texture pack, 7 PS2
cutscenes (where PS2 movies play), 8 to 11 the graphics pages (`launcher_setup_graphics.inc`), 12 button prompts,
13 You're all set (`launcher_setup_end.inc`). There is no Original or Remastered step any more (stage 5 removed):
a new install starts Remastered as the setup begins (`StartFullSetup` -> `ApplyRemastered`: the hardware
recommendation's preset with that preset's global illumination, Strong GTAO, 60 FPS), the Original look is a switch
on graphics page 4 and on the Graphics page, and "Use recommended for my PC" gives Remastered again. A player who had
the setup on an older build gets stage 14 instead, **New in this update** (`launcher_setup_new.inc`), once, on the
first start after updating (after the update's shader screen): HD texture pack [Download] [I already have one]
[Not now], PS2 cutscenes [Yes, restore them] [No thanks] (where PS2 movies play), "See the new graphics options"
(graphics pages 1 to 4 with the player's own settings; Done on page 4 and Back / B on page 1 come back), [Continue].
`kk_setup_version` remembers it: "waiting" from the first start after an update until Continue, then the version
(also set by the full setup, which never shows it). Downloads ask and run as from Settings. Up / Down move between the
rows of buttons, Left / Right along a row, A / Enter / a click use one; B does nothing there. There is no "Run setup
again" option; Reinstall game (About) runs the full setup again and keeps the settings of a player who had it.

- **Play** on Home (and Start / P on any page) opens the **Play menu**: Home's look (clear stills, the logo, the
  large entries), the header `PLAY` instead of the tabs. Continue (only with a save: the one saved last), New
  game, Load game (greyed with no save), Chapter select, Extras, Back. The status row shows what the focused entry does:
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
- `--kk_launcher_open=play|chapters|extras` (never saved) opens the launcher on the Play menu or Chapter select: for the
  game side when it goes back to the launcher.
- The Cheats page no longer has "All chapters".

### The pause menu

`CreatePauseMenu` (`pause_menu.h`) makes one ImGui dialog for the session (the launcher makes it at Play; the app
should also make it when the launcher is skipped). While `play::PauseMenuOpen()` it draws over the dimmed frame:
the game's logo, `PAUSED`, and Resume, Start again and Score (when `PauseActionAvailable`), Options, Settings, Quit
game. Start again and Quit game ask first (Cancel focused); B, Esc or Start resume. Options shows the port's
settings (Gameplay) until the game's own Options page is known.

**Settings**: tabs Display, Graphics, Audio, Controls (with Button remapping and Keyboard bindings), Gameplay,
Cheats, Advanced (the same order as the launcher's Settings, `settings_layout.h`), the launcher's own rows (`SettingsPages`, shared code). Owner, 18 Oct (task 206): every row is shown; those that
cannot change while the game runs are greyed out with the note "This can't be changed while you play. Quit to the
launcher's main menu and change it in Settings." (`LiveOnly`: window mode and size, resolution, refresh rate,
monitor, VSync, render quality, internal resolution, look, preset, Recommended, graphics API, renderer, language,
startup logos). Live since task 206: the frame rate cap (the limiter and the HFR fixes read it every frame), the
upscaler between Off and the temporal upscalers the running device offers (as F8 lists them; the renderer swaps
them at a frame boundary) and its mode; with FSR 1 / NIS or a render quality below Native the Upscaler row is greyed
(the presenter's spatial upscalers are chosen at start). Display has Picture (Brightness, Gamma; applied at Present,
`backend/picture_adjust.h`; the kept pause frame is retouched to follow them) and Overlay (Performance metrics,
hidden while the pause menu is open). The menu's list has a **Graphics** entry that opens the Graphics tab. Changes
are saved like in the launcher.
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

In a session started by a request the game's menus never come back: its request for the start menu (level 3,
what Quit game asks for) is cancelled, and when the start menu's pages open anyway (a chapter-select run's end:
`uni_go` loads level 3 itself, shows the Score pages and the save question there, then opens the chapter select)
they are not run. Then, once no save is being written, the program restarts into the launcher
(`--kk_launcher_open=chapters` after a chapter-select run's end, `--kk_launcher_open=play` after "Return to main
menu" or anything else), or exits after "Quit game" (to the desktop). The restart drops `--kk_skip_launcher`,
`--kk_start` and `--kk_launcher_open` from the command line, and the program closes as its close button does.
Story runs go on to the next chapter as the game does.

### The pause takeover

As described at the top of `pause_hooks.cpp`. The game's own pause draws a black screen; the last frame of play
is captured as the pause starts (`CaptureGuestOutput`, about 20 ms at 2560x1440) and drawn under every dialog
while the port's menu is open (`CreatePauseBackdrop`), so the pause menu sits over the frozen frame. While it is
open the game reads an empty pad (buttons still held when it closes stay hidden until let go), the pad it read
goes to the pause menu (`kk::PauseMenuPad`), and mouse look is switched off (the runtime lets the cursor go).

The game's pause items (found in `IntMIG_Page_Main`'s list, `0x824BA630`): chapter-select runs have Options (2),
Quit game (3), Start again (8), Resume (4), Score (0); story runs with a save have **Save (1)**, Change storage
device (1000), Options (2), Quit game (3), Resume (4). (chapter-jump.md had Score as 7 and Options as 5: 7 is an
extra item shown with profile flag `0x40`, not identified.) Contract additions (agreed with the coordinator):
`PauseAction::kSave` for the story runs' Save, and `PauseAction::kQuitToDesktop` next to `kQuitToLauncher`: the
pause menu has "Return to main menu" (the game's Quit game, then the launcher's Play menu) and "Quit game" (the
game's Quit game, then the program exits), both confirmed first. Change storage device has no meaning on PC. Score and Save show the game's own pages (drawn and
read by the game; the port's menu comes back when they close). Start again restarts the chapter without a level
load (its opening cutscene again).

The game's **Options** item opens `IntMIG_Page_OptStart`: **Audio**, **Video**, **Controls** (Language is not
offered in play). Contract action `PauseAction::kGameOptions` (agreed with the coordinator) opens them from the
port's pause menu (its Options entry): the game draws and reads them, and the port's menu comes back when they
are left. Tested: run `...-061637-ch8-game-options` opened each page and went back (developer presses
`optpick=<row>` / `optback`), frames `shot_*`:

| Page | Rows |
|---|---|
| Audio | Subtitles (on / off), Master, Voices, Music, Effects (volume bars), Accept the changes |
| Video | Display mode (Locked), Special mode (Locked), Accept the changes |
| Controls | Vibration, Aiming axis (Normal / inverted), Aiming visor, Inventory, Accept the changes |

These values are the game's own, kept in the save. **Next** (a later step, decided by the user): launcher-style
settings for subtitles and the four volumes in the port's Settings, so the game's pages are no longer needed.
Vibration and the aiming axis overlap the port's Controls settings (the port's apply on top of the game's).

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
| Achievement pop-ups and sound | live | every frame / every unlock |
| Anti-aliasing (`swap_post_effect`), texture filtering (`anisotropic_override`) | restart | read once when the renderer starts (both renderers: `plugin_settings.cpp` `ReadGameSettings`, the Xenos plugin's graphics system) |
| Render quality and scale, resolution, window mode, monitor, refresh rate, VSync, spatial upscaler (FSR 1 / NIS), renderer, language, Original look, startup logos | restart | greyed in the pause menu with a note (owner, 18 Oct) |
| Frame rate cap (`kk_frame_rate`), temporal upscaler and its mode, brightness and gamma, Performance metrics | live (task 206) | the limiter and HFR fixes every frame; the renderer's live settings (`effect_settings.h`) every frame; the overlay every paint. Tested: 60 -> 30 FPS, DLSS -> FSR 3.1 -> Off in play (`runs	ask206-live2-vk-*`) |

The game's own Options pages hold audio volumes and other values kept in the save; see "The game's Options"
below.

### Chapter names (found, not read yet)

The chapter select's names are a text file of the start menu's level, key `0x4902DD54`, loaded only while the
start menu (level 3) is up: ids `0x4902DD68 + n`, n = 0 to 41 ("Skull Island", ..., "Credits"; `0x4902DD99` and
on are the Extras). The entry's n is the third column of the chapter table (`G + (0x1468 + entry) * 4`, also the
fourth): entry 0 has 17 ("The venture"), entry 8 has 7 ("V-Rex"), entry 27 has 26 ("Kong to the rescue"). In the
game: `sub_826FEC90(file key, id, out)` then `sub_826FEE20(out)` returns the string; the text manager is at
`0x828E89B8` (+8 count, +12 files; a file: +4 count, +8 entries of 32 bytes with +0 id and +12 offset, +12
strings, +20 key); `KK_DEV_TEXT_DUMP` writes the loaded texts (`texts-menu.txt` when the start menu's pages open).

A reader of the player's files was tried and stopped (the coordinator's time limit): the text file is not an entry
of `KKMaps.bf`'s table (no `0x4902....` keys there; the table holds level containers keyed `0x02007...`), so it sits
inside a level container (the start menu's), LZO-packed like the rest; the raw file only holds the internal
level names ("14B Kong to the rescue"). A full scan of the table's entries with `launcher_art.cpp`'s pack reader
found nothing (its chunk format does not fit these entries). What is left: find the start menu's container key
(the level-3 load reads it: a file trace at that load names it), unpack it, and find the text file in it by the
ids above. `Chapters()` still returns "Chapter N". The scan code is in `moved-aside\menus-game\`.

### Tests (game side)

All windowed, xenos plugin, private copies of `F:\KK-native-renderer\userdata` (or of saves made by these runs,
in `runs\menus-game-data`), run folders `F:\KK-native-renderer\runs\menus-game-1013-<time>-<name>`:

| What | Run | Result |
|---|---|---|
| Chapter start, port pause menu over the frozen frame (stand-in) | `052129-ch8-pause2` (`printwindow_140`) | menu over the last frame of play |
| Screen black during the start (no logos, title or menus) | `052819-ch8-score-again-quit` (`printwindow_15`, `_30`) | black until the chapter loads |
| Score (the game's page), back, Start again (restarts, opening cutscene) | `052819-ch8-score-again-quit` (`shot_110`, `shot_210`) | as the game's own items |
| The real pause menu (pause_menu.cpp) and its Settings over gameplay | `053106-realpause-ch8-quit` (`printwindow_280`) | drawn, pad reaches it |
| New game in an empty slot: save created and written (named "Tester") | `054652-newgame-slot2d` | `KKProfileSlot2.sav` written, continue 100 / -1 |
| Saves: list, backup (and no duplicate backup), delete (moved aside) | `054855-saves-test2` (`saves_test.txt` in `runs\menus-game-data\userdata-savestest`) | as specified |
| Chapter on an empty slot, chapter end: Score pages, save question, saving, then the launcher | `054914-ch27-emptyslot3-end`, `055102-ch27-end-to-launcher` (`printwindow_450_<launcher pid>`) | slot 3 created and saved with the chapter's score, never deleted; the launcher opens |
| Chapter end goes back to Chapter select | `055907-ch8-live-options-end` (log: `--kk_launcher_open=chapters`) | yes |
| Live settings: FOV, fog, button prompts (PS5 sheet applied in play), cheats switched on in play (machine gun 500 rounds) | `055907-ch8-live-options-end` (log, `shot_50`/`_80`/`_110`, `shot_200` shows PS5 prompts) | applied at once |
| The game's Options pages | `055907-ch8-live-options-end` (`shot_200`, `shot_250`) | Audio, Video, Controls |
| Story run (continue point V-Rex): Save from the pause | `055758-story-save4-return` (`shot_110`: "Save successful") | file written |
| Story run: Quit game (to the desktop): the game's quit save, then the program exits | `055614-story-save3` | exits, no launcher |
| The game's Options pages: Audio, Video, Controls open and close, the port's menu comes back | `061637-ch8-game-options` (`shot_*`) | yes |
| Native renderer (with the shader pack): backdrop capture, the real pause menu, Resume, Start again | `062000-native-ch8-pause2` (`printwindow_200`) | yes (without a shader pack the native renderer did not reach the title in 2 minutes: `061748-native-ch8-pause`) |
| A Kong chapter (entry 10, level 304, playing as Kong): pause, Resume, Start again | `062250-kong-entry10` (`printwindow_170`) | yes |
| Keyboard and mouse in the real pause menu | `062155-input-kbm` | not testable under the test rules: posted key and mouse messages to the unfocused window reach neither SDL nor ImGui, and focusing the window is not allowed; the game side blocks the game's keyboard (same path as the pad) and mouse look while the menu is open |
| Return to main menu (chapter run) | `053106-realpause-ch8-quit`, `060043-final-ch8-return-to-menu` (`printwindow_400_<launcher pid>`) | launcher's Play menu |
