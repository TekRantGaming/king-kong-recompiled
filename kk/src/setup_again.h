// Beta 14 (owner, 21 Oct 2026, DECISIONS "Beta 14 additions" 1 and 4; task 350a): updating to Beta 14 takes every
// existing player through the first-time setup once more, starting from the NEW defaults (DirectX 12 on Windows,
// the hardware recommendation's preset, the Display, Graphics and Controls pages and the button prompts back to
// their defaults). Saves, the game files, installed downloads (texture packs, PS2 cutscenes) and every other page
// (Audio, Gameplay, Cheats, Advanced) are kept. The game files page is skipped when the game files are there.
// kk_setup_again remembers which round a player has had; a fresh install has its setup anyway and is marked done.
// Kept free of the launcher so tests check it (tests/launcher_logic_tests.cpp).
#pragma once

#include <string_view>

namespace kk::setup_again {

// The round this build asks for: 1 = Beta 14. A later version that needs everyone through the setup again raises it.
inline constexpr int kRound = 1;

// An existing install (a settings file or a remembered version from before this start) that has not had this
// round yet. Never in automated test runs (KK_AUTOPLAY), which keep their prepared settings.
inline bool Due(bool existing_install, int round_done, bool testing) {
  return existing_install && !testing && round_done < kRound;
}

// The settings the setup again puts back to their defaults: what the setup's Display, Graphics and Controls pages
// and its button prompts page show (settings_layout.h), and the F8 panel's look settings saved with them. Not the
// texture pack (an installed download), not the PS2 cutscenes, not the game's own options kept in the saves
// (Horizontal flip, Old movie, Subtitles), not what this PC was found to run (kk_upscalers_offered).
inline bool Resets(std::string_view name, std::string_view category) {
  static constexpr std::string_view kNames[] = {
      // Display
      "fullscreen", "kk_fullscreen_mode", "window_width", "window_height", "monitor", "present_letterbox",
      "kk_brightness", "kk_gamma", "d3d12_allow_variable_refresh_rate_and_tearing", "kk_frame_rate", "kk_hdr",
      "kk_hdr_levels", "kk_fov", "kk_motion_blur", "kk_camera_shake", "kk_camera_shake_strength",
      // (present_hdr and its levels follow kk_hdr and kk_hdr_levels at every start: hdr_display.cpp. A Manual peak
      // and paper white are not used once HDR brightness is back on Auto, and are then not saved.)
      // Graphics (the graphics API comes back to DirectX 12 on Windows, Vulkan on Linux)
      "kk_original_look", "kk_modern_settings", "kk_graphics_preset", "native_backend", "swap_post_effect",
      "anisotropic_override", "present_effect", "kk_render_quality", "resolution_scale", "draw_resolution_scale_x",
      "draw_resolution_scale_y", "native_upscaler", "native_upscaler_quality", "kk_big_blur", "kk_fog", "ao_mode",
      "ao_strength", "native_ao_method", "native_ao_apply", "native_ao_quality", "native_ao_samples",
      "native_ao_normal_smoothing", "native_gi", "native_gi_distance", "native_shadow_quality",
      "native_water_reflections", "native_volumetrics", "native_volumetric_strength", "native_effect_buffers",
      "native_sharpness", "native_cel_shading", "native_cel_outline", "native_cel_darkness", "native_cel_bands",
      "native_cel_strength",
      // Controls and the button prompts
      "mnk_mode", "kk_button_prompts", "kk_pad_prompts", "kk_camera_sensitivity", "kk_camera_modern",
      "kk_mouse_sensitivity", "mnk_mouse", "kk_mouse_invert_y", "kk_invert_rs_x", "kk_invert_rs_y", "kk_invert_ls_x",
      "kk_invert_ls_y", "kk_toggle_aim", "kk_deadzone", "kk_vibration", "kk_vibration_strength",
  };
  for (const std::string_view n : kNames)
    if (name == n) return true;
  // Button remapping (kk_map_*) and the keyboard bindings (keybind_*, the runtime's controller keybinds).
  return name.substr(0, 7) == "kk_map_" || category == "Input/Keybinds/Controller";
}

}  // namespace kk::setup_again
