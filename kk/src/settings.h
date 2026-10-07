// King Kong port settings.
//
// Port-specific options are cvars in the "KK" categories, so they load and
// save with the rest of ReXGlue's config (king_kong.toml next to the
// exe) and can be overridden on the command line (--kk_frame_rate=120).

#pragma once

#include <array>
#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, kk_launcher);
REXCVAR_DECLARE(bool, kk_skip_launcher);
REXCVAR_DECLARE(int32_t, kk_frame_rate);
REXCVAR_DECLARE(std::string, kk_render_quality);
REXCVAR_DECLARE(std::string, kk_button_prompts);
REXCVAR_DECLARE(bool, kk_check_updates);
REXCVAR_DECLARE(bool, kk_share_poster);
REXCVAR_DECLARE(bool, kk_show_fps);
REXCVAR_DECLARE(int32_t, kk_deadzone);
REXCVAR_DECLARE(int32_t, kk_camera_sensitivity);
REXCVAR_DECLARE(bool, kk_achievement_toasts);
REXCVAR_DECLARE(bool, kk_achievement_sound);
REXCVAR_DECLARE(std::string, kk_achievement_sound_file);
REXCVAR_DECLARE(int32_t, kk_achievement_volume);
REXCVAR_DECLARE(bool, kk_vibration);
REXCVAR_DECLARE(int32_t, kk_vibration_strength);
REXCVAR_DECLARE(bool, kk_invert_rs_x);
REXCVAR_DECLARE(bool, kk_invert_rs_y);
REXCVAR_DECLARE(bool, kk_invert_ls_x);
REXCVAR_DECLARE(bool, kk_invert_ls_y);
REXCVAR_DECLARE(bool, kk_toggle_aim);
REXCVAR_DECLARE(bool, kk_cheats);

namespace kk {

// Xbox 360 gamepad buttons, in XINPUT_GAMEPAD wButtons bit order where they
// are buttons; the triggers are analog in the gamepad state and are handled as
// full press / release when remapped.
enum class Pad : uint8_t {
  kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
  kStart, kBack, kLeftThumb, kRightThumb,
  kLeftShoulder, kRightShoulder,
  kA, kB, kX, kY,
  kLeftTrigger, kRightTrigger,
  kCount,
  kNone = 0xFF,
};
constexpr size_t kPadCount = static_cast<size_t>(Pad::kCount);

struct PadInfo {
  const char* id;     // config value / cvar suffix, e.g. "a" -> kk_map_a
  const char* label;  // UI label
  uint16_t mask;      // XINPUT_GAMEPAD bit, 0 for triggers
};
const PadInfo& GetPadInfo(Pad pad);
Pad ParsePad(std::string_view id);  // kNone for "none"/unknown

// What each physical control sends to the game (kk_map_<id>).
Pad GetMapping(Pad physical);
void SetMapping(Pad physical, Pad target);

// Frame-rate choices offered by the launcher (0 = unlimited).
constexpr std::array<int32_t, 7> kFrameRateChoices = {30, 60, 120, 144, 165, 240, 0};

// Render-resolution presets (kk_render_quality).
struct RenderPreset {
  const char* id;
  const char* label;
  double ratio;  // output size / render size
};
const std::array<RenderPreset, 6>& RenderPresets();
// resolution_scale (1-8) a preset gives for an output height; 0 for "custom".
int RenderScaleFor(std::string_view preset, int output_height);
// Applies kk_render_quality for the given output height (call before the GPU starts).
void ApplyRenderPreset(int output_height);

// Writes the current settings (skipping command-line/env overrides) to `path`.
bool SaveSettings(const std::filesystem::path& path);

// Changes a cvar's default; values from the config file or command line still win.
void SetCvarDefault(std::string_view name, std::string_view value);

// Port defaults that differ from ReXGlue's (call before the config is loaded).
void ApplyPortDefaults();

// Forces the ReXGlue settings the port depends on (see settings.cpp).
void ApplyRuntimeOverrides();

}  // namespace kk
