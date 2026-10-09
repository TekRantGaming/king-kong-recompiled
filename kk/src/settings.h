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
#include <vector>
#include <utility>
#include <string_view>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, kk_launcher);
REXCVAR_DECLARE(bool, kk_skip_launcher);
REXCVAR_DECLARE(int32_t, kk_frame_rate);
REXCVAR_DECLARE(std::string, kk_render_quality);
REXCVAR_DECLARE(std::string, kk_button_prompts);
REXCVAR_DECLARE(bool, kk_check_updates);
REXCVAR_DECLARE(bool, kk_show_fps);
REXCVAR_DECLARE(int32_t, kk_deadzone);
REXCVAR_DECLARE(int32_t, kk_camera_sensitivity);
REXCVAR_DECLARE(bool, kk_camera_modern);
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
REXCVAR_DECLARE(double, kk_mouse_sensitivity);
REXCVAR_DECLARE(bool, kk_mouse_invert_y);
REXCVAR_DECLARE(std::string, kk_pad_prompts);
REXCVAR_DECLARE(bool, kk_skip_intros);
REXCVAR_DECLARE(int32_t, kk_fov);
REXCVAR_DECLARE(bool, kk_motion_blur);
REXCVAR_DECLARE(bool, kk_fog);
REXCVAR_DECLARE(bool, kk_big_blur);
REXCVAR_DECLARE(bool, kk_original_look);
REXCVAR_DECLARE(std::string, kk_modern_settings);
REXCVAR_DECLARE(bool, kk_cheats);
REXCVAR_DECLARE(int32_t, kk_hitch_report_ms);

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
// Shader preparing: Balanced's per-frame wait for pipelines being created, at
// 30 FPS (half a frame; less at higher frame rates, see ApplyRuntimeOverrides).
constexpr int32_t kBalancedShaderWaitMs = 16;

constexpr std::array<int32_t, 8> kFrameRateChoices = {30, 60, 90, 120, 144, 165, 240, 0};

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
// Once the settings file is read: brings one from an older version up to date
// (keeping the defaults it was made with) and saves it; a new install just
// gets the current defaults.
void MigrateSettings(const std::filesystem::path& config_path);
// Settings the player can't change (shader preparing is always Balanced).
void ApplyFixedSettings();

// Forces the ReXGlue settings the port depends on (see settings.cpp).
void ApplyRuntimeOverrides();

// The Original look (launcher: Graphics > Look): the Xbox 360's own settings,
// as cvar name and value.
const std::vector<std::pair<const char*, const char*>>& OriginalLookSettings();

// With kk_original_look on, sets those values (in case the settings file was
// edited). Call before the GPU starts, as it sets the render resolution.
void EnforceOriginalLook();

}  // namespace kk
