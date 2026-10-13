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
