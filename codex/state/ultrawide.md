# Ultrawide mod (task 223/231): state after phase 2 step 1 (18 Oct, Beta 11 queue)

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

## Phase 2 (task 231): HUD box built, not yet seen with a HUD on screen
- Game side (ultrawide.cpp, called from freecam.cpp's display-list hooks, the only hooks of those functions):
  sub_827FA2B0 from 0x82873AB0 = "in list" (mod on, not fov::InMenus()); each of the 19 leaf routines
  (KK_HUD_LEAF(addr, n)) sets tag HUD | leaf n << 8 for its call; sub_827C6660 (weapon pass) clears it. Draws of
  the list outside leaves (sub_827FA0B8/sub_827FA138 paths: 3D pieces, counts 1000-2700) stay untagged.
  Tags go to the renderer through nr_set_draw_tags(tags, A) (plugin/photo_api.cpp; backend/draw_tags.h).
- Renderer: draw_state.cpp copies the tags into DrawCall.tags on the game thread (travels with the render
  thread queue); a HUD draw with a perspective matrix in its VS constants is untagged (leaf 10 = sub_82874C98
  draws the 14310-index arms mesh AND the text: found in run v7on3). GameRenderer::Draw divides
  ndcScale.x / ndcOffset.x by A for tagged draws unless HudDrawCoversWidth (positions span the viewport in
  pixels, or c0-c3 clip spans -1..1, or an untextured quad given in clip space x -1..1 = a fade).
- KK_ULTRAWIDE_HUD_LOG=1 logs the first 300 distinct tagged draws (leaf, prim, count, vs, tex0, bounds, full).
  Seen: leaf 10 text (font tex 414FA7xx, pixel x 309..670), a full-screen untextured strip (vs 400FCE80, fade),
  a textured full-screen quad (vs 400F7B80) -> both kept full width. No other leaf drew in ch 7/8 starts.
- Window check (runs\claude-231-tools\run.ps1 PrintWindow, must be DPI aware: 250 % scaling makes a 2560x1080
  window 6400x2700 physical; non-aware capture shows only the top-left crop): window = full wide picture,
  same as the game frame, no letterbox, no squash (Vulkan, ch 7).
- Test-script lessons: the shot clock starts at "gameplay reached" = when the held SKIP ends (or at the
  pause); START in the script pauses and every later shot is the paused (black) game. Ch 8 intro ~29 s.

## Still to do (phase 2)
1. See the HUD in game: ammo / health tint / subtitles / pause cover / cutscene start, with shots after
   gameplay reached (SKIP short, presses after it). The perspective-matrix untag (arms) is built + tested
   (CTest) but not yet run in game.
2. Mod off vs Beta 10 and one D3D12 run were not repeated for task 231 (off path unchanged by code: no tags
   are ever sent at factor 1, the renderer only acts on tags).
3. Culling at the edges while turning (phase 1 item 2): not checked.
4. Menus (InMenus: no box, as phase 1), loading screens, photo mode letterbox, freecam, PS2 movies;
   screen-space effects at non-16:9 (blur taps, point sprites, AO/GI/cel); launcher Display page sizes.
