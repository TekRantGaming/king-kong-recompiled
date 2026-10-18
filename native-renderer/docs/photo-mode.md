# Photo mode (task 208, 18 October 2026)

Owner decision 18 Oct (DECISIONS.md): opened from the in-game pause menu; game frozen, free camera, Jack's
floating first-person arms hidden (owner, later the same day: only the arms; weapons show only while aiming
and stay; Ann and Kong are never hidden), filters and picture settings, save the picture. Code:
kk/src/photo_mode.cpp (panel, state), photo_mode_logic.h, freecam.cpp (camera), fov.cpp (field of view),
backend/game_photo.cpp, photo_config.*, photo_grade.*, scene_photo.*, shaders/photo.hlsl,
plugin/native_photo.h (the game's settings each frame through `nr_photo_set`, hooks/kk_native_photo.cpp).

## The freeze: the game's own pause

Phase 0 (trace hooks on `sub_82857E80` and `sub_828732E8`, frame logs of a paused frame): while the game is
paused the world display and its main view still run every frame (60 a second at 60 FPS) and draw the whole
scene (901 draws against 944 in play). The black screen is one extra full-screen quad at the end of the paused
frame (vertex shader template 2E3B3ECDD3BC8D62, patched B9AD69E17E826C6E; pixel shader EC85BA15F4E9DC06; 4
indices), never drawn in play. So photo mode keeps the game paused (its pause page keeps running, the port's
pause menu steps aside: `PauseMenuOpen()` is false while `photo::Active()`), and the renderer leaves that quad
out. No frame time or time scale is touched (KK_DEV_FORCE_DT faults, REPORT-152/154; the AI step is never
throttled). Leaving photo mode goes back to the pause menu; Resume is the game's own resume (it adds the paused
time itself), so timers, sounds and input resume exactly as after any pause.

Not frozen while paused: rain streaks and Jack's first-person arm animation keep moving (both run on their own
clocks); the arms are hidden, rain is a screen effect.

Photo mode closes by itself when the pause page is gone for 10 frames in a row (a level change, the game's own
page): one long frame (a stall) also looks like a missing page at the start of the next frame.

## Camera

The free camera (freecam.cpp, `PhotoStart`) starts at the game's view and stays within 15 m of it (no
streaming). Roll turns the view's right and up axes about its forward axis; field of view replaces the scene
view's angle in fov.cpp (default: the game's with the player's field of view setting); speed in m/s. F9/F10
do nothing meanwhile. The HUD (F10's hooks), the first-person weapon pass (`sub_827C6660`, as with any free
camera), the overlays and toasts (`freecam::HideHud`) and motion blur are off.

Controls: left stick move, right stick look, triggers down / up, stick clicks faster / slower, A take picture,
B back to the pause menu, Start straight back to the game, Y hide / show the panel, LB / RB tabs, D-pad rows
and values, X default (or choose Reset camera / Reset all). Keyboard and mouse: W A S D move, Space / Ctrl up /
down, Shift / Alt faster / slower, hold the right mouse button to look (always with the panel hidden), Enter
take picture, Esc or Backspace back, R hide panel, Q / E or Tab tabs, arrows rows and values, Delete default,
P back to the game; the mouse works the panel like the launcher's.

## What the renderer leaves out

Hide bit 1: Jack's arms, by `GeometryContentHash` (index and vertex bytes) and index count: 7EB7E914947CEC40
(14310 indices) and D69009DC0D6F9433 (2928), in the depth and colour passes. Never by shader (the arms' pixel
shaders are shared with leaves). Ann's and Kong's meshes cannot match (checked: one Ann and two Kong chapters
in photo mode, and the unit test). Bit 2: the pause cover above.

## The photo grade

One full-screen pass over the frame image right after the Present copy (renderer.cpp), so the presenter and
every capture (Take picture, F6, KK_DEV_SHOTS) include it and no UI is in it. Order: letterbox bars, mirror,
depth of field, exposure (linear light), white balance, contrast, saturation, look (black and white, sepia,
warm, cool, green, rose with a strength), vignette, film grain (a fixed pattern per photo session, so a still
stays still). `PhotoGradeColour` (photo_config.cpp) is the colour part on the CPU. Made the first time it is
needed; with photo mode off nothing runs.

Depth of field: `ScenePhotoDepth` finds the end of the opaque pass as the cel shading does (the AO's rule table,
frame tracker and camera vote) and runs a `Gtao::Prepare` there: view-space depth in metres at render
resolution. The grade's gather (40 taps on a golden-angle spiral, largest radius 1.6% of the picture height)
blurs by the distance from the focus beyond the sharp range. Transparent things (water, smoke) take the depth
behind them. Made when depth of field is first used, retired 8 frames after photo mode ends.

Cel shading, sharpening, brightness and gamma: photo mode's values replace the player's in the renderer's live
settings (`PhotoOverrideEffects`) only while it lasts and only for rows the player changed; no setting is ever
written.

## Pictures

`art::CaptureFrame` (the guest output, full output resolution, no UI) to `<user data>/screenshots/
kk-photo-YYYYMMDD-HHMMSS-mmm.png` (the F6 captures' folder), with a three-second confirmation in the panel.

## Tests and tools

kk tests `photo-mode` (settings, camera radius and roll, controller mapping, picture names, the hidden meshes,
the grade's colours) and `photo-mode-text` (five languages, no dashes, the pause menu entry, no settings
written, arms only). Developer builds: `KK_DEV_PHOTO_STEPS="<s>:<step>,..."` (enter, exit, resume, set:<row>=
<value>, tab:<n>, panel:<0|1>, take[:name], shot:<name>, pad:<button>, pos, done; photo_mode.cpp), with
`KK_DEV_PHOTO_DIR` for its game captures. Runs: wt/photo-mode/kk/out/task208 (run.ps1, freeze.ps1).
