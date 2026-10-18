# Ultrawide mod (tasks 223/231/236/239): state after task 239 (18 Oct, Beta 11 queue)

Phase 1 done on branch ultrawide (commit 0754571 + this file; merged beta4-next 837a49d = Beta 10 first).
Plan steps 1, 2, 3, 5 built and checked in game (Vulkan + one D3D12 run). Step 4 (HUD box) not started.

## What is true now
- cvar kk_aspect (KK/Video, settings.cpp): "16:9" default, "21:9", "32:9", "auto" (window shape clamped
  16:9..32:9). Maths in kk/src/ultrawide_logic.h (AspectFor, WidthFactor, LargestInside, WidenProjection);
  unit tests in kk/tests/hardware_recommendation_tests.cpp (ctest hardware-recommendation).
- ApplyRenderPreset: factor A = aspect/(16:9). A == 1: nothing differs from Beta 10 (same code path, sizes
  unchanged, native_display_aspect ""). A > 1: UpscalerOutputSize / NoUpscalerFrameSize give the largest
  picture of that shape (no-upscaler draw width x A), native_display_aspect = "WxH" (plugin cvar, kInitOnly),
  UltrawideFactor() = A, log line "KK: ultrawide (experimental): ...".
- Plugin native_graphics_system.cpp OnSwap: passes native_display_aspect as the presenter's display shape
  (instead of the guest's 1280x720), so the presenter fits the wide picture, not a 16:9 box.
- kk/src/ultrawide.cpp: hooks sub_8278AB00 (thread-local "in projection") and sub_820E0D08 (projection
  push); when r4 == 0x83090510 and perspective (m[11] != 0, m[15] == 0) and not kk::fov::InMenus(), column
  0 (m[0,4,8,12]) /= A for the push, scratch matrix restored after. Log "KK: ultrawide: first 3D view widened".
- Built-in mod kk/mods-builtin/ultrawide/mod.toml (kk_aspect = "auto"), name "Ultrawide (experimental)",
  description says experimental / may not work as intended; translated in 5 languages via
  mods_text_table.inc (workshop_mods.cpp now translates built-in mods' descriptions with T()).
  MODDING.md row + built-in sentence; CHANGELOG "## v2.0.0-beta.11 (unreleased)" line.

## Game check (runs\claude-223-*, tools in runs\claude-223-tools: run.ps1, diff.py)
- Chapter 7, KK_DEV_SHOTS 10/20/30 s. Window 2560x1080 gives chosen resolution 6400x2700 here (250% DPI,
  pre-existing OutputSize behaviour), 1280x720 gives 3200x1800.
- Mod off vs Beta 10 base exe (0167e76): same sizes (4800x2700, 3200x1800) and same log lines; pixels differ
  1-2.7% (mean abs 0.6-1.1), the same as run-to-run noise (Vulkan mod vs D3D12 mod: 1.2-1.9%, 0.6-0.8).
  Runs are not deterministic, so byte-identical frames cannot be shown; the off path is identical by code.
- Mod on (Vulkan and D3D12): 6400x2700 frame, wider view, same height, Jack and the scenery not stretched.
- Not seen yet: the presented window itself (only the game's frame was captured), the HUD (none in those
  shots), culling at the new edges (nothing obviously missing in chapter 7's start).

## HUD box (task 231 built it, task 239 moved it to the HUD pass)
- How the game draws: it RECORDS draws into passes on one thread (main view sub_82872DB8, the display list
  sub_827FA2B0 and its 19 leaves) and a render executor DRAWS them on another thread: sub_82797460 runs the
  passes in order through sub_82796C10(executor, pass) (call sites: 1 @827975DC, 8 @82797618, 0 @82797674,
  12 @827976D0, 13 @827976DC, 2 @82797730, 9 @8279773C, 14 @82797748, 4 @827977A8; 10 in sub_827973D8).
  Pass 1 = shadows (viewport 832), pass 12 = first-person arms/weapon, PASS 4 = the 2D HUD: ammo counter
  (gun icon + "30 / 0"), subtitles, loading text, other HUD text/icons.
- So 231's tagging from the display-list hooks was a RACE (other thread; it tagged whatever was being drawn at
  that moment, e.g. the arms as "leaf 10"). Removed (freecam.cpp back to a4fb36c). Now ultrawide.cpp hooks
  sub_82796C10: pass 4, mod on (UltrawideFactor() > 1), not fov::InMenus() -> tag HUD | (pass+1) << 8 for the
  pass, restored after. Mod off: the hook calls straight through, no tag ever sent.
- Renderer (unchanged except one fix): draw_state.cpp copies tags into DrawCall; a tagged draw with a
  perspective matrix in c0-c3 is untagged. Task 239 fix: it used to scan ALL 256 VS registers, whose stale 3D
  matrices untagged every 2D draw. GameRenderer::Draw divides ndcScale.x/ndcOffset.x by A unless
  HudDrawCoversWidth (full-width positions, clip -1..1, untextured clip-space quad = fade).
- Why kk_hide_hud / F10 / photo mode leave ammo + subtitles: they skip the display-list LEAVES; the ammo
  counter and subtitles are recorded elsewhere but drawn in pass 4. A way to hide them: skip pass 4 in the
  sub_82796C10 hook when freecam::HideGameHud() (check first that pass 4 holds nothing else wanted, e.g. a
  fade or the loading text; not done, behaviour unchanged in 239).
- Diagnostics: KK_ULTRAWIDE_HUD_LOG=1 (renderer, first 300 distinct tagged draws: leaf = pass+1, bounds, full);
  KK_ULTRAWIDE_PASS_DIAG=1 tags EVERY pass (find what a pass draws; breaks shadows; never for play).
  Guest call-stack draw log (not committed): runs\claude-239-tools\stack-diag.patch (KK_DRAW_STACK_LOG=1 in
  native-renderer/hooks/kk_native_hooks.cpp: walks r1 back chain, saved LR at chain-8).

## Checks (task 239, ch 8, 2560x1080 = 6400x2700 at 250 % DPI; runs\claude-239-*, tools runs\claude-239-tools)
- run.ps1 (as 236) + measure.py (ammo/subtitle x range, side brightness). Test save:
  runs\claude-236-tools\userdata-hud (subtitles, aiming visor, inventory on). KK_DEV_SHOTS_FROM=menu; no START.
- Mod on Vulkan (on8): ammo x 0.738..0.766 of the width (before 0.816..0.855; 16:9 box maps to 0.737..0.766),
  subtitles 0.383..0.614 (before 0.344..0.652): both in the box. Cutscene start at 1 s: dark over the full
  width (fade not boxed). D3D12 (dx8): same numbers. Mod off (off8): 4800x2700, no ultrawide log line, no
  tagged draw, ammo 0.817..0.855 / subtitles 0.343..0.652 (= old). CTest 33/33.
- NOT SEEN: health tint (no damage taken; ch 27 start gave none either), aiming visor overlay, pause cover
  with the new code, pass-4 full-width draws (none logged in these runs).
- Earlier (236): pause menu over the paused game OK; mod off vs Beta 10 sizes/logs PASS; edge pop-in not
  conclusive (dark cave).

## Still to do
1. Health tint after damage and the aiming visor in the box (find a spot with damage).
2. Edge pop-in in a bright outdoor spot (e.g. ch 7 or a Kong level) with two shots per turn.
3. Menus, loading screens, photo mode letterbox, freecam, PS2 movies; screen-space effects at non-16:9;
   launcher Display page sizes. Optional: hide pass 4 with kk_hide_hud / F10 / photo mode (owner's call).
