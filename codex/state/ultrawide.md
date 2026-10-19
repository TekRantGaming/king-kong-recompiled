# Ultrawide mod (tasks 223/231/236/239/243/311): state after task 311 (19 Oct, Beta 11 queue)

Experimental built-in mod, off by default (DECISIONS 18 Oct). Branch ultrawide3 (from beta4-next 5abad69),
commit ca6b04b (task 311). Not merged, not pushed.

## What is true now
- cvar kk_aspect (settings.cpp): "16:9" default, "21:9", "32:9", "auto" (window shape clamped 16:9..32:9).
  Maths kk/src/ultrawide_logic.h (AspectFor, WidthFactor, LargestInside, WidenProjection), tests in
  kk/tests/hardware_recommendation_tests.cpp. ApplyRenderPreset: A = aspect/(16:9); A == 1 = Beta 10 path
  exactly; A > 1: wider frame sizes, native_display_aspect "WxH", UltrawideFactor() = A.
- Built-in mod kk/mods-builtin/ultrawide (id builtin:ultrawide, kk_aspect = "auto"), 5-language description.
- Hor+: ultrawide.cpp hooks sub_8278AB00 (in projection) + sub_820E0D08 (push of 0x83090510).
- Culling (task 311): the frustum came from the view's 16:9 angle. fov.cpp's sub_82872DB8 hook now also widens
  the view angle (tan half-angle x A, like kk_fov) for every scene view except the menus, inside a
  ViewWidenedScope; ultrawide.cpp's push hook then sees y/x = 1.778 (logged once "view angle widened ...
  height put back") and multiplies column 1 by A, so the pushed matrix equals the old column-0 one. Views not
  widened that way (first-person weapon) keep column 0 / A. KK_ULTRAWIDE_OLD_CULL=1 = old 16:9 frustum (diag).
- Photo mode (task 311): fov::InMenus() was TRUE in photo mode (the pause's menu script runs; PauseMenuOpen is
  false), so the photo view was NOT widened: the picture was stretched 1.33x. MenusShow now excludes
  photo::Active(). The photo path widens SceneFieldOfView by A too. Letterbox maths (photo.hlsl, uses the real
  picture size) and saved pictures (6400x2700, CaptureGuestOutput) were already right.
- HUD box (239): executor pass 4 (sub_82796C10) tagged HUD -> renderer divides ndc x by A unless the draw
  covers the width (fades/tints). Hide HUD (243) tags the same pass "hide". Photo mode now boxes pass 4 too.
- Launcher / pause Display (task 311, launcher.cpp SettingsPages::PictureAspect): in game UltrawideFactor; in
  the main menu kk_aspect, or "auto" when kk_mods holds builtin:ultrawide. Mod on: Aspect ratio row options
  "Letterbox"/"Stretch", new description + fact "The Ultrawide mod (experimental) is on: the game's picture is
  W x H"; Upscaling rows' "Draws ..." sizes use the mod's shape. Mod off: rows exactly as before.
  3 new strings in launcher_text_table.inc (5 languages).

## Checks (task 311; 2560x1080 window = 6400x2700 at 250 % DPI; tools runs\claude-311-tools)
- run311.ps1 (claims slot claude-311) -> runP.ps1 (copy of 246's; window shots pw_<name>.png on
  "KK dev: (window )shot"). bars.py (black bars), center.py (wide picture's middle 16:9 vs a mod-off 16:9 frame,
  histogram-equalised), strips.py (left/middle/right diff), grid.py (contact sheet).
- Photo mode: needs the pause open first: -PauseActions '6:open', KK_DEV_PHOTO_STEPS=7.5:enter,11:shot:p0,...
  (set:letterbox=N: 1=1.85, 3=2.39, 5=1:1). Results: 2.39 -> bars top/bottom 0.004 (2.392), 1.85 -> side bars
  0.109/0.110 (1.850), 1:1 -> 0.289 each side; saved picture identical; Vulkan and D3D12.
- Photo hor+: center.py vs mod-off p0: before fix middle 60.7 / squeezed 33.7 (stretched); after fix middle 32.2 /
  squeezed 64.6 (hor+). Runs claude-311-uw-photo2 (before), -ph-new, -ph-off, -dx-photo.
- Mod off (ph-off): 4800x2700, no ultrawide log line. CTest 39/39.
- Culling A/B (ch 27 / ch 8 photo, -cull-new2 / -cull-old2 / -ph-oldcull): NOT CONCLUSIVE: the cutscene
  end and the start camera differ run to run, so frames don't line up. The fix is the same mechanism kk_fov
  uses; nothing missing seen at the edges in ch 27 (bright outdoors, Kong + V-Rex) with it on.
- Aiming (ch 8, LT held): no visor overlay appeared on screen (setting on in userdata-hud); HUD box fine.
- Health tint: still NOT SEEN (no damage in ch 8 / ch 27 wander; Jack's health address unknown, hardcore.cpp).
  No full-width pass-4 draw logged (KK_ULTRAWIDE_HUD_LOG) in these runs.

## Still to do
1. Culling: a deterministic A/B (same camera: e.g. photo mode at a fixed freecam position, or a dev toggle
   flipping KK_ULTRAWIDE_OLD_CULL at a script time) to prove no pop-in and no LOD change.
2. Health tint and aiming visor: find a way to take damage (health address) and what the visor is.
3. Menus' 3D pieces stay 16:9 (by design: InMenus), loading screens, freecam outside photo mode, PS2 movies.
4. Screen-space effects at non-16:9: blur/bloom taps, point sprites, 320/640 buffers using min(x,y), AO/GI/cel.
5. In-game look at the pause Display page with the mod on (row text only checked by build/tests).
