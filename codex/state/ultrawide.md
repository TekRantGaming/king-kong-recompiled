# Ultrawide mod (task 223): state, paused 18 Oct for the emergency, moves to Beta 11

Nothing implemented yet. Phase 1 research only. No game runs.

## Found (code reading)
- Projection: sub_82872DB8 copies the view (fov at view +172) into the camera (GDI = *(0x82CCAC30), camera at
  GDI +208, fov at GDI +220), calls the projection callback at GDI +32 (sub_8278AB00, via bctrl), then builds
  the view matrix (sub_827E5BB8) and sub_827E58D0(GDI+1040, GDI+432).
- sub_8278AB00 perspective path (GDI+176 & 0x400 clear, & 1 set; loc_8278AEB8): cot = 1/tan(fov*0.5);
  matrix at 0x83090510; if (GDI+176 & 4) m00 = cot*f28, m11 = -cot (vertical fov) else m00 = cot,
  m11 = -cot/f28 (horizontal fov). f28 = height/width: table 0x828D3690[i] (i=1 when camera word0 bit0 is
  clear; i=3 when *(0x82D62320+728) != 0; else GDI+204 if 1..3) or GDI+184. The matrix goes to
  sub_820E0D08(r3 = *(0x828D1CD4), r4 = matrix): a projection matrix stack push (copies 16 floats).
  Other paths call sub_8278A918 = orthographic off-centre (leave alone).
- First-person weapon (sub_827C6660) and the photo-study element set GDI+220 and call the GDI+32 callback
  directly, so a hook on sub_8278AB00 covers them too.
- HUD: the screen-space display list sub_827FA2B0 called from lr 0x82873AB0 (freecam.cpp F10 hooks, 19 leaf
  routines); the weapon pass sub_827C6660 runs inside it.
- Renderer: native_render_scale already takes per-axis scales ("2560x1080" -> 2 x 1.5, render_scale.h);
  AO/GI/cel/lighting use host_scale_x/y separately; the 320/640 effect buffers use min(x,y) (game_resources.cpp
  ScaleFor). Frame/upscaler size comes from kk::UpscalerOutputSize (settings.cpp ~300, "largest 16:9 inside"),
  written to native_upscaler_output / native_frame_size in ApplyRenderPreset, read by plugin AutoRenderScale.
  Mods are applied (kk::mods::ApplyAtGameStart) before ApplyRenderPreset and before the GPU starts.
- Draw records: DrawCall (backend/draw_sink.h) is filled on the game thread and copied by ThreadedSink, so a
  per-draw tag from a guest-side flag can be captured there.

## Plan
1. cvar kk_aspect = "16:9" (default, nothing changes) / "21:9" / "32:9" / "auto" (window aspect clamped to
   16:9..32:9). A = aspect / (16/9). Pure maths in a header with unit tests.
2. UpscalerOutputSize: largest picture of the chosen aspect instead of 16:9 (only when kk_aspect != 16:9).
   Then the renderer draws e.g. 2560x1080 with scale 2 x 1.5 (guest stays 1280x720).
3. Hor+: hook sub_8278AB00 (thread-local "in projection") + sub_820E0D08: when r4 == 0x83090510 and the
   matrix is perspective (m[11] == 1, m[15] == 0), m00 /= A. Not inside the HUD list, not in menus
   (fov.cpp g_views_since_menu). Culling follows (it uses the projection, as kk_fov showed).
4. HUD in a centred 16:9 box: export nr_set_draw_tags from the plugin; kk sets a HUD bit while inside
   sub_827FA2B0 from 0x82873AB0 (cleared inside sub_827C6660); the tracker copies it into DrawCall; for tagged
   draws ComputeViewport divides ndcScale.x / ndcOffset.x by A. Risk: full-screen fades in that list would
   cover only the centre (check in phase 2; maybe a full-screen quad test).
5. Mod kk/mods-builtin/ultrawide/mod.toml (type settings, kk_aspect = "auto"), MODDING.md row, CHANGELOG line.

## Expected problems to check in phase 2
Fades and full-screen overlays, loading screens, front-end menu 3D pieces, pause cover, subtitles, the game's
blur/bloom taps (guest-pixel steps become wider horizontally), point sprites, Kong chapters' camera, freecam
and photo mode (letterbox maths), PS2 movies (already aspect-fit with bars).
