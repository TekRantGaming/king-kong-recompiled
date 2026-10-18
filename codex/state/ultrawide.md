# Ultrawide mod (task 223): state after phase 1 (18 Oct, Beta 11 queue)

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

## Phase 2 must check / fix
1. HUD and 2D overlays (orthographic, sub_8278A918): drawn over the whole width = stretched by A. Plan step 4
   (centred 16:9 box via draw tags, ComputeViewport ndc x / A); watch full-screen fades in that list.
2. Culling: confirm the frustum (sub_827E5BB8 is the VIEW matrix; frustum builder not found) covers the
   wider view: look for pop-in at the side edges while turning; if it pops, also widen the fov at
   sub_82872DB8 and compensate m11 in the push.
3. Presented window (PrintWindow of our own window): no 16:9 letterbox, no squash.
4. Menus (front end 3D pieces kept 16:9 = stretched with the 2D), pause cover (pause_hooks.cpp fits the
   game's output), loading screens, subtitles, fades, photo mode letterbox maths, freecam, PS2 movies.
5. Screen-space effects with guest-pixel steps (blur/bloom taps, point sprites, 320/640 buffers use
   min(x,y) scale) and AO/GI/cel at a non-16:9 frame.
6. Launcher Display page still shows 16:9 sizes (the mod only applies at game start); fine for a mod.
