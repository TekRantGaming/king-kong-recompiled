# Hayes stuck dinosaur (tasks 224, 251) - Beta 10 rescue did NOT fix it; Beta 11: weapon cheats off in 303

## Task 251 (19 Oct, branch hayes-cheats): weapon cheats switched off in level 303 (owner; NO root-fix work)
- A player confirmed the 224 rescue did not fix the softlock (Beta 10). Owner: stop the root fix; in level 303 the
  Revolver / Machine gun / Shotgun / Sniper rifle cheats stop applying; settings kk_cheat_<id> unchanged.
- Table: kk/src/cheat_rules.h kLevelCheatsOff {303, "Hayes", {ids}} (add "ammo" there if the owner asks).
  cheats.cpp: Applies(id, level) = chosen and not suppressed, level = G+8 at the moment of giving: ApplyCheats
  (start menu / launcher Play), WatchLoadout (re-give after chapter or checkpoint start) and WatchChanges (switched
  on in play) skip suppressed ids. WatchLevel (input poll) logs "KK: level 303 (Hayes): these cheats are off ..."
  and "... apply again as set (now level N)"; keeps g_level for the pause menu (kk::CheatSuppressedNow).
- Pause menu: launcher.cpp CheatItems greys the rows in place (in_game_ only) with "Not available in this
  chapter." (5 languages, pause_text_table.inc). Test: kk-cheat-rules-tests (ctest cheat-rules).
- Assumption to check in game: the game empties weapon/ammo at the 303 start (cheats.cpp header says every chapter
  and checkpoint start does), so a cheat machine gun from 302 is gone and the chapter loadout is what Jack has.
- GAME CHECK LIST (not run yet, code-only job):
  1. Chapter select entry 7 with Cheats + Machine gun on: no machine gun, chapter loadout; pause > Cheats: the
     four weapon rows greyed with the note, can't be switched on; healing/ammo/spears rows unchanged; log line.
  2. Entry 6 and entry 8 with Machine gun on: machine gun given as before (no suppression line).
  3. In 303, Load last checkpoint: still no cheat weapon (no "given again" log line).
  4. Story play 302 with Machine gun on -> 303: weapon is the chapter's own after the transition; 303 -> 304: the
     machine gun comes back (log "apply again as set").
  5. Continue / launcher Continue into 303 from a save: no cheat weapon. Launcher Settings > Cheats: not greyed.

## Result of task 224 (18 Oct 21:25)
- Stall = raptor 1 (42657DD0) right after waking stops in PNJ_Raptor_ETAT_VALA (sub_825E06B0) on the ledge
  (214.6,73.1,-1.0): path list (+7028, count +6776) keeps being rebuilt 1 <-> 9 waypoints, next waypoint
  42660288 (210.5,82.8,-2.9) is never walked to, raptor stands still forever; Hayes waits (CapaBit on all three).
- Happens with AND without the machine gun cheat in our flow (ng30-a and v-vk30none: no cheats, stalled). Player's
  log 013: healing + machine gun were switched on in play in level 302, before Hayes (my earlier "no cheats" was wrong).
- Fix (rescue, level 303 only): in ETAT_VALA, alive + path > 0 + still < 0.3 m for 8 s game time -> move onto next
  waypoint (sub_8270FCE0 / sub_827117E0 / sub_82401AA8, as the game's own warp-link code at 0x82569494);
  3rd stall -> life 0. KK_RAPTOR_RESCUE=0 turns it off. In stalled runs it takes 2 moves (down to 42660288,
  stalls, up to 426612D0), then the raptor goes round to the sailor and the fight ends normally.
- Verified (kills by KK_DEV_KILL stand in for the player's shots once the raptor is reachable):
  v-vk60mg-2 (stall, rescued), v-vk30none (no cheat, stall, rescued), v-dx30mg (D3D12, stall, rescued),
  v-vk60mg-1 and v-dx60mg (no stall, rescue silent): Hayes H_CINE_Vala -> walks down -> H_ETAT_IA_suivi in all.
  Without rescue: mg60, mg60c, ng30-a stalled for good.
- Not root-caused: why the path finder routes back through 42660288 (a link the mover can't take).

## Facts
- Chapter Hayes = chapter-select entry 7 = level 303. Player (Beta 9, Ally X, kk_frame_rate 60 then 30) got stuck
  in story play 302 -> 303 (log 013) and then Continued 21x from "level 303 start 5" (logs in
  runs\hayes-dino-evidence\player-logs). No cheats when it first happened (flags 118); later machine gun,
  shotgun, one-hit, spears cheats did not help.
- Start points of 303 (KK_DEV_START_POINT): 1 = chapter start (50.5,136); 3 = two-raptor intro fight (128,92);
  4 = (178.9,59.8); 5 = (217.7,38.7) the stuck encounter. sp2 = no play (Jack not placed).
- Start-5 encounter: 3 PNJ_Raptor objects (heap addresses repeat between runs):
  42657DD0 "raptor 1" spawn (210.75,82.34,-3.93); 4265A2F8 (236.65,74.17); 4265C808 (230.98,89.14).
  Hayes 418A893C at (223.6,40.6); sailor with gun 41E15F74 at (239.5,39.6,0.8); Jack 418A0640.
- Flow (verified with dev aids, 30 and 60 FPS): Jack entering PRG_SSDetect 4261AB50 zone (~(222.5,50.4)) wakes
  raptors 2+3 (ETAT_CINE_VALA). Raptor 1 wakes only when Jack enters BV 4265EFC0 (ActivatorTrigger at
  (238.5,62.3,-4.3)); it then "goes around" (jumps up to (221,43.6,3) / (223.7,31.8,3) -> (235,27) -> (242.6,38))
  and bites/grabs the sailor (MORD/GRAB). Hayes waits in H_CINE_WaitDefault (then H_ETAT_IA_hide) until all three
  are dead: CapaBit test (caller 8244F860/8244F8A4/8244F8F8) on each raptor, raptor 1 -> 0.
- Raptors retreat to ETAT_VALA near y~99 when Jack is in the low pit (z ~ -4.9): normal, they return to FIGHT when
  Jack comes near.
- CANDIDATE STUCK STATE (seen once, mg60 run, 60 FPS, machine gun, wander): raptor 1 woke at 100 s, went VALA to
  (214.5,73.1,-1.0) and stayed in ETAT_VALA there for 117 s (to the end of the run), never reaching the sailor
  or Jack. In the 30 FPS run r1wake the same raptor passed that spot (VALA -> ATTENTE -> VALA -> HESITE -> FIGHT).
  Not yet known if this is the player's stuck dino (check (214.5,73.1) = "foot of the stairs behind the rock"?).
- States without damage check (no PNJ_Raptor_exec_check_paf call): PAF_FALL 825A9E18, PAF_FLY 825A94D0, FADE.
  ETAT_VALA 825E06B0 leaves only when timer +6804 = 0 and (vars+32 == 16 or target 7148 lacks flag or one of the
  +5236 candidates (+5328 valid, +5748 distance < [820B0CA0]) qualifies).
- Raptor AI vars = [[obj+24]+4]+64: +20 life (float, 50), +5108 state id, +6776/+2556 moving flags, +6804 timer,
  +7148/+7168 target, +6448 dest xyz.

## Dev aids added (uncommitted WIP -> commit "WIP task 224")
- KK_DEV_START_POINT=<n> (play_session.cpp StartChapter): chapter-select start at another start point.
- KK_DEV_TELEPORT="t,x,y,z[,fx,fy]/..." (dev_teleport.cpp, original TrigExec_Teleport sub_82538B70).
- KK_DEV_KILL="t,obj/..." sets raptor life (vars+20) to 0 (dies through MORT/FADE).
- KK_DEV_TRIG_TRACE=1 (dev_trig_trace.cpp): 20 TrigTest_* functions, logs record + result on change.
- dev_sample.cpp: new column `tracks` (all AI track functions; 3rd = current ETAT), KK_DEV_SAMPLE_VARS=<track fn>
  dumps 12 KB AI vars per sample to vars.bin (raptors: 8254BCB0).
- Runner runs\claude-hayes-tools\run.ps1 (claims slot, private userdata, -Env 'K=V;..', -Extra 'a|b').
  Analysis scripts (scratchpad, copy if needed): track.py, segs.py, vars.py, vdiff.py, dis.py (PPC listing from
  kk\generated\default with names from analysis\hfr\names.json).
- Repro command (60 FPS candidate): run.ps1 -Label mg60 -Fps 60 -Seconds 220 -Env "KK_DEV_START_POINT=5;
  KK_DEV_TELEPORT=25,220.3,59.2,-2.0,0,1/100,238.5,62.3,-3.0,-1,1;KK_DEV_SCRIPT=0:SKIP/28;KK_DEV_WANDER=1;
  KK_DEV_SAMPLE=0,220,0.25" -Extra '--kk_cheats=true|--kk_cheat_healing=true|--kk_cheat_machine_gun=true'

## Next steps
- Owner 19 Oct: no more root-fix work. Run the game check list above in a game step (task 251 follow-up).
