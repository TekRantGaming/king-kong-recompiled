# Chapter jump: starting a chapter straight from boot

Investigation (13 October 2026, branch `chapter-jump`): can the launcher start the game directly in a chosen
chapter, skipping the startup logos, the title, the save menu and the main menu? **Yes.** The game's own chapter
select only fills in a few universe variables and asks for a level; the same can be done on the title screen,
about 1.5 s after the program starts. The proof of concept is developer-only (`kk/src/dev_chapter_jump.cpp`,
`KK_DEV_TOOLS` builds, `KK_DEV_JUMP=<entry>`); nothing is shipped.

## How the game starts a chapter

The menus are script pages (names and functions in the AI function table at `0x828CB140`: function, name per
12-byte entry):

| Function | Name | Role |
|---|---|---|
| `sub_824BD150` | `IntMIG_Page_Map` | the chapter select; its variables block `v` = `*(*(0x82CCBF64)+0x40)`, entry index at `v+0x3C` |
| `sub_824B4DF0` | `IntMIG_Page_load` | the page the chapter select goes to: writes the level request |
| `sub_8253DB80` | `uni_go` | universe script function, every frame: performs a requested level change |
| `sub_8253E398` | `uni_init` | universe init at boot: fills the chapter table |
| `sub_824AE560` | `IntMIG_init` | start-menu init (runs again after a level loads) |
| `sub_824CB738` | `IntMIG_Page_Memcard` | the save menu: sets the save slot `G+0x4E10` |

The universe variables G are one block (`0x414CF9DC` in every run so far). The universe script reaches it as
`*(*(0x82CCBF64)+0x40)`; the script globals `*(0x82CC97C0)` and `*(0x82CC8D24)` (the "G" and "P" of
`cheats.cpp`) point at it too, but only once the first real level has loaded (they are 0 on the title screen).

**Pressing A on entry i** (`sub_824BD150` from `0x824BD9D4`): if `i > G+0x4FF4` (chapters reached) and the
profile flag `G+0xBF0 & 0x80` (the KKst0ry cheat) is off, it plays the error sound and stops. Otherwise:

1. `*(0x828D177C) = G[0x526C + 4i]` (a per-chapter number);
2. for an entry other than the one in progress: `sub_82413190()` (snapshot of the universe variables into
   `0x82C63EA0`), copy it to the restart buffers (`0x82C63EA0` to `0x82C6BDE0`, `0x7F40` bytes; `0x82C6399C` to
   `0x82C73D20`, `0x500` bytes; the first version of the proof of concept had these two wrong, see "Saves"), `sub_8271D698(*(0x828E838C))` (re-initialise the universe), `sub_82413190()`
   again;
3. `G+0x5668 = 1` (chapter-select run), `G+0x5670 = i`;
4. `v+0x144 = G[0x5008 + 4i]` (the level key), `v+0x148 = G[0x50D4 + 4i]` (the start point), then
   `sub_826FA3B0(2, *(0x82CC97C8))` switches to the load page.

**The load page** (`sub_824B4DF0`) then sets `G+4 = 1` (go), `G+8 = level key`, `G+0xC = start point`,
`*(0x828E3B48) = 1`, `G+0x5F1C = 0`. **`uni_go`** sees `G+4`, clears it, starts the loading screen
(`sub_8278E660`) and loads the level.

The chapter table (8 columns of 51 words from `G+0x5008`, count at `G+0x5000` = 42) is filled by `uni_init` at
boot, so it is there before any menu. Level keys and start points of the entries used here:

| Entry | Chapter | Level | Start |
|---|---|---|---|
| 0 | Venture | 100 | 0 |
| 8 | V-Rex | 305 | 1 |
| 27 | Kong to the Rescue | 1402 | 1 |

(All 42: 100, 101, 201, 203, 204, 301, 302, 303, 305, 306, 304, 401, 402, 403, 404, 501, 502, 504, 702, 705,
901, 1002, 1101, 1200, 1201, 703, 1401, 1402, 1501, 1701, 1702, 1703, 1801, 1803, 1804, 1805, 1901, 1902, 2001,
2003, 2004, 7. Start point 1 everywhere except entry 0 (0), entry 40 (49) and entry 41 (0).) At boot the
universe loads level 1 and then level 2 (the title); the start menu is level 3.

## The proof of concept

`KK_DEV_JUMP=<entry>` hooks `uni_go`. After `KK_DEV_JUMP_WAIT` calls (30 in the tests, about 1 s on the title
screen) it does steps 1 to 3 above itself (calling `sub_82413190` and `sub_8271D698` as the page does), then
what the load page does, and lets `uni_go` run. Nothing in the menus runs.

Test launches: `kk-dev-vulkan` build, Xenos plugin, windowed, private copies of the test user data, no menu
presses at all. Frames are the game's own captures in `F:\KK-native-renderer\runs\chapter-jump-1013-*`:

| Entry | Run | What the frames show |
|---|---|---|
| 8 V-Rex | `...-025415-jump8-wander` | the opening cutscene (Jack and Ann) 1 to 3 s after the load, then play: Jack with the machine gun, walking and firing |
| 0 Venture | `...-025904-jump0-wall` | the Intro story movie (New York, the map), "Pacific Ocean, October 12th, 1933", the VENTURE hull with the "Right stick to aim" hint |
| 27 Kong to the Rescue | `...-030056-jump27` | the loading screen "Kong to the rescue", the Kong and V-Rex cutscene, then play: Jack with a spear behind the rocks, the V-Rex, fires |

Reference run through the menus (V-Rex, scripted presses): `...-024028-menus-vrex`.

### Load time

| | Program start to the chapter's level loaded |
|---|---|
| Jump, V-Rex | 2.7 s (request at 2.0 s, `uni_go` done 0.6 s later); play about 4 s after start |
| Jump, Kong to the Rescue | about 2.9 s |
| Jump, Venture | 59.4 s: the Intro movie plays inside the load (57 s, no pad is read, so it cannot be skipped). Through the menus it is the same 57 s |
| Through the menus, V-Rex (test script) | 46.7 s, mostly the script's waits; the save menu itself only opens about 8 s after start, and a quick player needs roughly 20 s more for the save menu, main menu and chapter select |

The level load itself is the same either way (about 0.6 s for V-Rex); the jump saves the menus.

## What differs after a jump

The universe variables after a jump were compared with those after the menus (`KK_DEV_JUMP_DUMPG`, 15 s into
V-Rex both ways). Apart from pointers and heap, the differences are what the save menu and the title set:

| Variable | Through the menus | After a jump | Meaning |
|---|---|---|---|
| `G+0x4E10` | 0 | -1 | save slot (set by `IntMIG_Page_Memcard`); -1 = none |
| `G+0x5F00`, `+0x5F04` | 1, 1 | -1, 0 | the signed-in player (from the user block at `0x8308FE18`, set when Start is pressed on the title) |
| `G+0x5F08`, `+0x5F24` | 1, 0 | -1, 1 | storage device state |
| `G+0xBF0` | `0x80` | `0x18` | profile flags: the loaded save's, plus the chapters cheat |
| `G+0x4FF8`, `+0x4FFC` | 100, -1 | 0, 0 | the save's "continue" position |
| `G+0x5678`, `+0x567C` | `0xC0010` | 0 | score totals of the save |

What that means in play:

- **Gameplay, HUD, loadout, music, cutscenes:** as through the menus. The chapter's own loadout arrives (V-Rex:
  machine gun; Kong to the Rescue: spears), hints show, the in-engine cutscenes and story movies play.
- **Pause menu:** only Options, Quit game and Resume. "Start again" and "Score" need a save slot: with
  `KK_DEV_JUMP_SLOT=0` (`G+0x4E10 = 0`) all five items are back, and Start again restarts the chapter from its
  opening cutscene (run `...-031325-jump27-restart`), so the restart buffers the jump fills are good.
- **End of the chapter** (simulated by asking for the next chapter's level, `KK_DEV_JUMP_END`): `uni_go` handles
  it as the end of a chapter-select run and loads the start menu (level 3) instead. Without a slot the chapter
  select comes up with every entry locked (run `...-030945-jump8-end`); with a slot the Score page (chapter bonus
  20000) comes first (run `...-031154-jump8-slot0-end`).
- **Quit game** goes to the chapter select with every entry locked (run `...-031240-jump8-quit`): the jump never
  loaded a save, so the profile has no progress.
- **Saves and progress:** no run wrote anything to the user data (all save files unchanged). Without a slot the
  game has nowhere to save, so a jump is a "guest" session: progress, scores and unlocks are lost when the game
  closes, and nothing is overwritten. Claiming a slot without loading its save (`KK_DEV_JUMP_SLOT`) would let a
  later save overwrite that slot with the fresh profile, so it is for tests only.
- **Launcher cheats** are not applied: `cheats.cpp` applies them on the start menu page (`sub_824BB670`), which a
  jump skips. Applying them at the jump instead is simple (the same variables).
- **Story movies:** chapters whose load plays a story movie (Venture's Intro) still play it, unskippable during
  the load, exactly as through the menus.

## How a launcher feature would use it

1. The launcher passes the chosen entry (for example `--kk_start_chapter=8`).
2. On the title screen (the universe is up and level 2 is loaded) the port does what the chapter select does:
   the snapshot, the restart buffers, the universe re-init, `G+0x5668/0x5670`, `*(0x828D177C)`, then the load
   page's request (`G+4/8/0xC`, `*(0x828E3B48)`, `G+0x5F1C`).
3. For a session that saves, it should load the player's save first: either drive the save menu's own code
   (`IntMIG_Page_Memcard`) to load a chosen slot before the jump, or skip saving and say so on the launcher
   ("chapter practice, progress not saved"). The second is what the proof of concept does today.
4. The launcher cheats would be applied right after the jump.

## Risks

- The jump runs before the game knows the signed-in player and the storage device (`G+0x5F00..0x5F24`). Nothing
  went wrong in these tests (V-Rex, Venture, Kong to the Rescue, pause, quit, start again, the score page), but
  paths that check the player or the device (achievements, a save prompt, a controller unplugged) are untested.
- If the jump is combined with a save slot but not the save's data, saving would overwrite that save.
- Only entries 0, 8 and 27 were played; all 42 use the same table, but chapters that start as Kong, or the last
  ones (entries 40 and 41 have unusual start points), are untested.
- Addresses are those of this game build (the recompiled image); they do not move, but the universe block is on
  the heap and must always be reached through `*(*(0x82CCBF64)+0x40)`.

## Test switches (`dev_chapter_jump.cpp`)

`KK_DEV_JUMP`, `KK_DEV_JUMP_WAIT`, `KK_DEV_JUMP_MENU`, `KK_DEV_JUMP_SLOT`, `KK_DEV_JUMP_SNAPSHOT=0` (skip the
snapshot, restart buffers and re-init), `KK_DEV_JUMP_REINIT=0`, `KK_DEV_JUMP_END=<s>`, `KK_DEV_JUMP_DUMPG=<s>`,
`KK_DEV_JUMP_WALLSHOTS=<s,...>` (frames on a wall clock, for loads that read no pad), `KK_DEV_JUMP_TRACE=1`.
After a jump the `KK_DEV_SCRIPT` / `KK_DEV_SHOTS_FROM=menu` clock starts when the chapter's level has loaded
(`DevStartScriptClock`), so scripts and `KK_DEV_WANDER` work as after the menus. The `uni_go` hook of
`dev_load_trace.cpp` moved here (one hook per function).

## Saves

Second round (13 October 2026): a jump that loads a chosen save first and then saves like the menus do. All tests
ran on private copies of `F:\KK-native-renderer\userdata` (one save, slot "A", in `KKProfileSlot1.sav`), or of
saves made by earlier test runs (kept in `F:\KK-native-renderer\runs\chapter-jump-saves\`). Below, **proven**
means a test run shows it (run folder given, all under `F:\KK-native-renderer\runs\`), **code** means it was only
read from the disassembly.

### How the game keeps saves (code, confirmed by the traces)

The save system is one object, M = `0x82C63950`:

| Address | What |
|---|---|
| `M+0xC` | its command handler, `sub_82721618` (set by `sub_827293B8`) |
| `M+0x14` | a buffer with all five slots' files (`5 x 0x8040` bytes), allocated and cleared by `sub_82729138` |
| `M+0x4C` (`0x82C6399C`) | the five slots' headers, `0x100` bytes each |
| `M+0x54C` (`0x82C63E9C`) | the current slot, 0 to 4 (-1 none) |
| `M+0x550` (`0x82C63EA0`) | the current save's data, `0x7F40` bytes |
| `0x82C6BDE0`, `0x82C73D20` | the copies a chapter-select run keeps of the data and of the headers |
| `0x82C8E7CC` | the job queue: `+0` state (`0x10000000` idle), `+8` job type, `+0xC` pending |

`sub_827291A0(command, slot)` is the entry the pages use (it forwards to the handler): `0x20000040` clears the
headers (slot -1); `0x20000050` **loads** the slot (allocates and clears `M+0x14`, clears the data, job type 2);
`0x20000060` **saves** to the slot (job type 3); `0x20000070`, `0x20000080` and `0x200000C0` are sent as the save
menu opens; 1 and 2 keep and put back the chapter-select copies. The jobs run on a worker thread:
`sub_82722150` reads every slot (and asks for the storage device, `0x828E3898`), `sub_82722320` loads one slot
(the file into `M+0x14`, then header and data into `M+0x4C` / `M+0x550`), `sub_82722620` saves one (header and
data into `M+0x14`, then `sub_827223E8` writes `KKProfileSlot<n+1>.sav\KKProfile.dat` through XContent). **When
the header's continue level is 0, the save job calls `sub_82721970` instead, which deletes the save with
`XamContentDelete`.**

The data is the universe's saved variables: `sub_82413190` copies them out of the universe (G) into `M+0x550`
(following the descriptor table at `0x828CEFF0`, plus `0x2F40` bytes of the block at `0x82CC5568`) and
`sub_82413080` copies them back. `sub_8271D698` re-initialises the universe but keeps the saved variables. The
save routine is `sub_8266AD80`: it builds the header from G (`G+0x4FF8/0x4FFC` the continue point, `G+0x4FF4`
the chapter reached) with `sub_82413978` and queues the save job.

**The file** (`KKProfile.dat`, `0x8040` bytes) is the header then the data. Header fields, read from the code and
the test diffs: `+0x00` the save's name (ASCII, "A"), `+0x20` last saved (Unix time), `+0x28` continue level,
`+0x2C` continue start point, `+0x30` created (Unix time), `+0x34` the chapter reached, `+0x38 + 4 x entry` the
best score per chapter (`+0x58` is V-Rex, entry 8: 20000 after the tests), `+0xDC` ten bits the game draws again
when the chapter reached changes. No checksum or signature was found: saving through the menus changed only the
fields the game changed (timestamp, continue start, the V-Rex score, `+0xDC`, two data words), and the game
loaded every file the tests wrote. Next to it the runtime keeps `Headers\00000001\KKProfileSlot1.sav.header`
(the XContent display name and file name) and `__thumbnail.png`.

**Player and device** (code, seen in the traces): Press Start on the title calls `sub_82700A48`, which makes the
pad that pressed it (`*(0x82C63704)`) the active player (`0x8308FE18 +0x3B/+0x3C`); `uni_go` then fills
`G+0x5F00/0x5F04` (the player) from it. The storage device the selector chose is `0x828E3898`; the always-running
page `IntMIG_afairetoutletemps` (`sub_824AC7E8`) mirrors it into `G+0x5F08` and opens the "storage device
removed" pages when they differ. `G+0x4E10` is the slot, set by the save menu page.

### A jump that loads a save first (proven)

Two ways were tried (`KK_DEV_JUMP_SAVE=<slot>`):

1. **From the file** (`KK_DEV_JUMP_SAVEFILE=<that slot's KKProfile.dat>`), all in one frame of the title screen:
   Press Start for pad 1 (`sub_82700A48`), the slot buffer (`sub_82729138`) and the file into it, the header into
   `M+0x4C`, the data into `M+0x550`, `M+0x54C = slot`, `sub_82413080` (the save into the universe), then the slot,
   the device (`0x828E3898 = 1`, `G+0x5F08 = 1`, `G+0x5F24 = 0`) and the save's continue point from its header,
   then the chapter jump as before. These variables are set again after the universe re-init, which resets them.
   Nothing of the menus is shown. Run `chapter-jump-1013-040502-jumpfile8-save`.
2. **The game's own load** (no file given): the commands the save menu sends, over several frames. It works
   (`chapter-jump-1013-035851-jumpsave8-fixed`) but is not invisible: once a player is registered the title moves
   on to the start menu, and the save menu shows for about 3 s before the jump.

Results with V-Rex (entry 8) and the original test save:

| | Evidence |
|---|---|
| The full pause menu (Options, Quit game, Start again, Resume, Score) | **proven**: `...-040502-jumpfile8-save\shot_110.bmp`, `...-040141-jumpsave8-quit\shot_110.bmp` |
| Chapter end: Score pages, "New bonuses are available", "Your score has changed! Do you want to save?", Yes, "Saving content", "Save successful" | **proven**: `...-035851-jumpsave8-fixed`, `shot_130` to `shot_560` |
| The save file is written | **proven** in both runs. Compared with the same actions through the menus (`...-034403-menus-save-end2`): the same bytes except the save time (`+0x20`), the continue start (`+0x2C`: -1 after the jump, 0 through the menus; the load page turns -1 into 0 anyway), `+0xDC` (random), and two data words not identified yet (`+0x1E8`: 19.0 through the menus, 0 after the jump; `+0x3CD0`: 0 through the menus, -1 after the jump) |
| The written save loads back through the normal menus | **proven**: `...-040028-menus-load-jumpsave`: slot "A", Play, and the chapter select shows "V-Rex, Score: 20000" |
| Quit game | **proven**: `...-040141-jumpsave8-quit`: the confirmation, then the chapter select on V-Rex; nothing saved |
| Continue the save (its continue point, `KK_DEV_JUMP_CONTINUE=1`) | **proven** for the test save, whose continue point is the new-game start (the Intro movie, then Venture: `...-040246-jumpsave-continue`); no save further on was available to test |

The comparison runs needed the launcher's chapters cheat on in both (the menus only let a fresh save start
Venture); `KK_DEV_JUMP_FLAGS=80` sets the same profile flag after a jump. After a jump the profile flags keep two
bits (`0x18`) the menus clear; the only visible difference found is "New bonuses" (plural) instead of "A new
bonus" at the chapter end.

### Mistakes that destroyed test saves (all on private copies)

- **Continue level 0 deletes the save.** The first save-loading jumps lost the continue point (the universe
  re-init resets it, and the chapter-select copies held the wrong data), so the game's save job deleted the slot
  instead of writing it. Runs `...-033825-jumpsave8-orig`, `...-034016-jumpsave8-signin`, `...-034204-jumpsave8-c`,
  `...-035141-jumpsave8-game3`: the save folder is gone.
- **Wrong command.** `0x20000060` is save, not load: sent to load, it wrote an empty save over the slot
  (`...-034941-jumpsave8-game2`).
- **Wrong restart-copy addresses.** The first proof of concept copied the chapter-select copies to `0x82C58490`
  and `0x82C603D0` (a disassembly helper ignored an `addis`); the right ones are `0x82C6BDE0` and `0x82C73D20`.
  The earlier runs therefore wrote `0x8440` bytes over other game data. They played fine, but they prove less than
  they seemed to about Start again. Fixed in commit `c325dff`.

## Replacing the main menu

What the launcher would need, and what is proven:

| Need | How | Status |
|---|---|---|
| List the saves | read `<user data>\<profile>\555307D3\00000001\KKProfileSlot<n>.sav\KKProfile.dat`: name `+0x00`, last saved `+0x20`, continue level and start `+0x28/+0x2C`, chapter reached `+0x34`, best scores from `+0x38` | format read from code and diffs; plain files the port can read at any time |
| Resume (continue) a save | load it (above), then ask for its continue point with `G+0x5668 = 0` | **proven** for a new-game continue point only |
| Chapter select | load the save, then the chapter jump | **proven** (V-Rex) |
| Unlock rules | the game allows entry i when `i <= G+0x4FF4` (chapter reached) or the profile flag `0x80` is set | **code** |
| New save | the game's way: the save menu's "Create new" sets the slot, calls `sub_82413978(slot, 2, 0, 100, -1, 0)`, re-inits the universe, sets `G+0x4FF8 = 100`, `G+0x4FFC = -1`, then saves (the save job creates the content). The port could also write the file itself from a template | **code** only |
| Delete a save | the game's way is a save job with continue level 0 (`XamContentDelete`); the port can move the folder and its header file aside | the game's way is **proven** (by accident, above); the port's way is not tested |
| Switch saves | today a restart of the process (about 3 s to the chapter); in place would mean loading another slot at a level boundary (load job, then `sub_82413080`), not tested | **code** |
| Difficulty | none found on the save (`G+0x4E10` is the slot, not a difficulty) | **code** |
| Options the menus set | language, audio, video and control pages (`IntMIG_Page_OptStart_*`); kept in the save's data block | **code** |
| Cheats | apply after the jump (the same variables as `cheats.cpp`) | **code** (`KK_DEV_JUMP_FLAGS` **proven** for flag `0x80`) |

Risks: a jump that sets a slot without that slot's data, or loses the continue point, overwrites or deletes the
save (both happened in the tests). Until this is a feature the port should back up the save before every launch.
Achievements were not watched. A missing or short save file is refused by the file loader (it checks for
`0x8040` bytes); a corrupt one was not tested. The player is registered as pad 1; another pad or a second player
was not tested.
