# Extras state (18 Oct 2026, after task 204, branch extras-b9 on beta4-next 1757c50)
- Catalogue kk\src\extras.h (15 retail entries from sub_824CBFE8), dispatch play_session.cpp CarryOut, modes
  profile_options.cpp / video_modes.h, Artbook artbook*.cpp (launcher only). No screensaver option in code.
- Mapping: Kong 1-4 = levels 50-53, Environments 1-3 = 54,55,56, Creatures = 57, Credits = 7, Alternate ending =
  2003 start 1 with G+0x5674=1; movies Trailer 19 / PJ 0 / PB 3 via sub_827252C8; modes G+0x4DA4 / G+0x4DA0.

## Unlocks (task 204, owner 18 Oct: all bonus content unlocked)
- extras.h kAllBonusesUnlocked / BonusUnlocked(extra) = true: launcher and game ask this, never a save.
  ExtraUnlocked(e, progress) stays as the original rule (hints, tests).
- Game side: cheats.cpp UnlockBonusContent keeps the game's own KKmuseum flag (profile flags G+0xBF0 |= 0x100)
  on at level start, start menu and every input poll. G+0xBF0 is NOT in the save descriptor (KK_DEV_EXTRAS_TRACE
  "save field" list has no BF0), so saves never change. Readers of 0x100: Xtra page, Video options page
  sub_824CE4A0, sub_825E8E00, sub_825EAC88, sub_826075E0/7788/7C08, sub_82603BD0.
- The "All bonus content" cheat row is gone (kCheats has 8); kk_cheat_bonus cvar kept but ignored.
- Bonuses start with slot -1 (no save: RegisterPlayer path) or a save slot (loads it for the video modes).

## Cheats and scoring
- Cheats-used mark = G.flags (G+0xBF0) 0x10000000, set only by gameplay codes (healing, one-hit, spears, ammo,
  weapons). Read at IntMIG_wait 0x824B4808 (chapter-select end: message 102 instead of score page + save) and
  pause page 0x824BB0BC (Score item: message 103). KKst0ry (0x80) and KKmuseum (0x100) never set it.
- Log line at each level start: "KK: game flags XXXXXXXX at the level start: cheats-used mark off/ON ...".
- Proof runs (Vulkan, V-Rex chapter-select, KK_DEV_START_END=24): defaults -> flags 00000118, pause Score shows
  IntMIG_Page_ScoreNew, chapter end score page + save, best score 0 -> 20000 (task204-scoreA-*). Fast healing
  on -> flags 10000318, Score shows a message, chapter end no save, score stays 0 (task204-scoreB-*).

## Pause in bonuses (task 204)
- Galleries: IntMIG_wait on Start requests the start menu when G+0x5680 (Extras row) != 0 and G+0x5674 == 0
  (sub_824B44E8 at 0x824B4D04). pause_hooks.cpp hides G+0x5680 from that check on the Start frame: the game's
  pause page opens, the port's menu works (Resume, Settings, Return to main menu -> launcher Extras).
- Galleries + credits set G+0x4E10 = -1 after loading a save (no Save item; Quit had opened the save page).
- Credits run no IntMIG pages: Start fades and returns to Extras (no pause). Movies: Start skips (no pause).
- Alternate ending: normal level, game's pause works (resume, quit).

## Tests (all 15 + Artbook pass, task 204)
- Runner: wt\extras-b9\kk\out\task204\run.ps1 (game-slot.ps1 claim, owner-game wait, private userdata,
  -Saves fixture|none, -Direct <kk_start>, -Artbook, -EnvList, -ExtraArgsList "a|b"; shader cache from
  runs\claude-gpu-setup-20261018-003733). Fixture prepare.py: slot 1 locked 0%, slot 2 100%/250,000.
- Launcher rows: focus:<exact label>; labels "Weta artworks: ...", "Artworks: Kong Part 4", "Alternative end".
  Always give KK_DEV_SCRIPT, else autoskip presses Start and skips movies/credits.
- Not driven: keyboard Esc (same pad path), mouse, kk-beta pause (no dev input in kk-beta).
