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
   `0x82C63EA0`), copy it to the restart buffers (`0x82C63EA0` to `0x82C58490`, `0x7F40` bytes; `0x82C6399C` to
   `0x82C603D0`, `0x500` bytes), `sub_8271D698(*(0x828E838C))` (re-initialise the universe), `sub_82413190()`
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
