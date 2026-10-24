# Ultrawide: state after task 363 (23 Oct, Beta 17): built in, always on, wide sizes in the resolution lists

Owner decision 23 Oct (DECISIONS top row): native ultrawide, ON by default, no setting, no mod, no experimental label.
Branch b17-uw (from beta4-next ca87286), merged into beta4-next 082070c (task 361, 23 Oct); smoke tested Vulkan and D3D12 at 16:9 and 21:9. Not pushed.

## What is true now
- No cvar: kk_aspect is gone. settings.cpp PictureAspectFor(w, h) = the chosen resolution's own shape clamped
  16:9..32:9 (ultrawide_logic.h AspectFor("auto")). 16:9 or narrower (16:10, 4:3): factor exactly 1, nothing differs.
  Dev off switch only: env KK_ULTRAWIDE=0 (UltrawideSwitchedOff, logs "KK: ultrawide: off (KK_ULTRAWIDE=0)").
- ApplyRenderPreset: A = aspect/(16:9) > 1: wider frame sizes, native_display_aspect "WxH", UltrawideFactor() = A,
  log "KK: ultrawide: WxH -> a WxH picture, views A times wider".
- Hooks unchanged from task 311 (ultrawide.cpp, fov.cpp: hor+, culling angle widened, HUD box on pass 4, photo mode),
  plus task 360: the MENUS now get hor+ too (their own angle, no kk_fov, angle widened by A for the frustum) and their
  HUD pass is boxed, so menu 3D and 2D still line up (hor+ and the box map the middle 16:9 the same way). This also
  makes the launcher's live menu scene and the game's own pause pages wide instead of stretched.
- Built in mod kk/mods-builtin/ultrawide removed (copy in moved-aside\task360-ultrawide-mod). mods_logic.h
  RetiredBuiltin("ultrawide"): a folder left behind by an update or an old build dir is never listed or loaded.
  Workshop facts entry, its 3 translated facts strings and the mod description string removed.
- Launcher / pause Display: PictureAspect = PictureAspectFor in the main menu, UltrawideFactor in game. Wide: Aspect
  ratio row "Letterbox"/"Stretch", description "What the game's wider picture does on a screen wider still.", fact
  "Ultrawide: the game fills your wide screen with a {0} picture and shows more at the sides." (5 languages, no dashes).
- MODDING.md (kk_aspect row gone, built in sentence), CHANGELOG beta.17 section, plugin cvar text updated.
- Tests: hardware-recommendation checks 16:9 and 16:10 give factor 1 and 2560x1080 sets the shape with no setting;
  texture-packs checks builtin:ultrawide has no facts. CTest 52/52.

## Task 363 (23 Oct): ultrawide sizes in the resolution lists
- cvar kk_picture_size ("" default, KK/Video, saved; in kRestartCvars, Display reset, setup again, beta only reset,
  diagnostics, benchmark). ultrawide_logic.h: kWideSizes (2560x1080, 3440x1440, 3840x1080, 3840x1600, 5120x1440,
  5120x2160, 7680x2160), ParsePictureSize (only wider than 16:9, at most 7680x2160), WideSizeOffered (size height <=
  screen height), FitWindow (picture size if it fits 90% of the screen, else largest of its shape inside, even).
- settings ChosenPictureSize(); king_kong_app.h OutputSize() returns it first, so ApplyRenderPreset, upscaler output,
  frame size and native_display_aspect follow; the presenter fits the shape into the real window/screen (bars, or
  stretched with Aspect ratio = Stretch). Log "... (chosen picture size, fitted into the window or screen)".
- Launcher/setup/pause: Window size lists wide sizes (chosen: window = FitWindow); Borderless gets a Resolution row
  (Desktop + wide sizes; setup's info row replaced); Fullscreen Resolution appends wide sizes the display has no mode
  for (screen stays at the desktop mode). Window mode Windowed refits the window. Facts/descriptions: 4 new strings.
  Setup Upscaler mode sizes now use the picture's shape (was 16:9).
- Checks (runs\claude-363-tools run363.ps1, settings file kk_picture_size, window 1280x720 = 3200x1800): 2560x1080 VK
  bars 0.124/0.124 (2.367), 5120x1440 D3D12 and 7680x2160 D3D12 bars 0.249/0.251 (3.556), frames at the chosen size,
  hor+ logged, no memory errors at 7680x2160. Not checked: launcher rows by eye, real borderless/exclusive (rules:
  windowed only).

## Checks (task 360; tools runs\claude-360-tools: run360.ps1 -> runP.ps1, stats.py, bars.py, diff.py)
- Window 1680x720 = 4200x1800 (21:9, A 1.313), 2560x720 = 6400x1800 (32:9, A 2.0) at 250 % DPI. Ch 8, userdata-hud.
- Ch 8 opening cutscene: autoskip presses Start, which opens the pause; "gameplay reached" about 47 s after load.
  Working timing: KK_DEV_SHOTS_FROM=menu, -PauseActions '6:open,52:close', KK_DEV_PHOTO_STEPS=7.5:enter,11:shot:p0,...
- 21:9 Vulkan DLSS, 21:9 D3D12 XeSS, 32:9 D3D12 FSR 3, 32:9 Vulkan DLSS: gameplay/cutscene hor+, ammo counter and
  subtitles in the middle box, pause menu over a wide backdrop, photo mode hor+, letterbox 1.85 / 2.39 side bars
  (2.39 at 32:9: 0.16 each side, expected 0.164), saved pictures full size (4200x1800). Far left at 32:9 is real
  geometry (rock wall), nothing culled. Vulkan KK_DEV_SHOTS frames come out black while paused (window shots fine).
- 16:9 (1280x720 = 3200x1800) vs beta4-next exe: same sizes and log lines, no ultrawide line; frame differences
  (35 to 89 %) are within two runs of the same build (36 to 87 %): cutscene frames are not deterministic.
- Launcher live menu scene at 21:9 (needs --kk_setup_again=1 and shader packs in the run cache): wide, not stretched.
  Setup Display page at 21:9 shows "Letterbox" (the wide row).

## Still to do
1. Health tint and aiming visor in the box (no damage spot found yet; KK_DEV_HEALTH_SET in hardcore.cpp may help).
2. Loading screens and PS2 movies at 21:9/32:9 (allowed to stay pillarboxed; not looked at: a full width loading
   picture in pass 4 would be stretched, HudDrawCoversWidth leaves it unboxed).
3. Front end without the launcher (--kk_launcher=false menus) not looked at; game's own pause pages (Score, Options).
4. Screen space effects at non 16:9 by eye only (AO/GI/bloom looked normal in captures); no numeric check.
