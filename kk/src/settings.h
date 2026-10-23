// King Kong port settings.
//
// Port-specific options are cvars in the "KK" categories, so they load and
// save with the rest of ReXGlue's config (king_kong.toml next to the
// exe) and can be overridden on the command line (--kk_frame_rate=120).

#pragma once

#include <array>
#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <utility>
#include <string_view>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, kk_launcher);
REXCVAR_DECLARE(bool, kk_skip_launcher);
REXCVAR_DECLARE(std::string, kk_start);
REXCVAR_DECLARE(int32_t, kk_frame_rate);
REXCVAR_DECLARE(int32_t, kk_cpu_priority);  // task 295
REXCVAR_DECLARE(std::string, kk_render_quality);
REXCVAR_DECLARE(std::string, kk_button_prompts);
REXCVAR_DECLARE(bool, kk_check_updates);
REXCVAR_DECLARE(std::string, kk_update_channel);
REXCVAR_DECLARE(bool, kk_online_count);
REXCVAR_DECLARE(bool, kk_discord_status);
REXCVAR_DECLARE(std::string, kk_discord_app_id);
REXCVAR_DECLARE(bool, kk_online_asked);
REXCVAR_DECLARE(std::string, kk_install_id);
REXCVAR_DECLARE(bool, kk_show_fps);
REXCVAR_DECLARE(std::string, kk_performance_metrics);
REXCVAR_DECLARE(int32_t, kk_brightness);
REXCVAR_DECLARE(double, kk_gamma);
REXCVAR_DECLARE(std::string, kk_hdr);
REXCVAR_DECLARE(std::string, kk_hdr_levels);
REXCVAR_DECLARE(int32_t, kk_deadzone);
REXCVAR_DECLARE(int32_t, kk_camera_sensitivity);
REXCVAR_DECLARE(bool, kk_camera_modern);
REXCVAR_DECLARE(bool, kk_achievement_toasts);
REXCVAR_DECLARE(bool, kk_achievement_sound);
REXCVAR_DECLARE(std::string, kk_achievement_style);
REXCVAR_DECLARE(std::string, kk_achievement_sound_file);
REXCVAR_DECLARE(int32_t, kk_achievement_volume);
REXCVAR_DECLARE(bool, kk_vibration);
REXCVAR_DECLARE(bool, kk_camera_shake);
REXCVAR_DECLARE(int32_t, kk_camera_shake_strength);
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
REXCVAR_DECLARE(bool, kk_ps2_cutscenes);
REXCVAR_DECLARE(std::string, kk_ps2_movie_folder);
REXCVAR_DECLARE(int32_t, kk_fov);
REXCVAR_DECLARE(bool, kk_motion_blur);
REXCVAR_DECLARE(bool, kk_fog);
REXCVAR_DECLARE(bool, kk_big_blur);
REXCVAR_DECLARE(bool, kk_original_look);
REXCVAR_DECLARE(std::string, kk_modern_settings);
REXCVAR_DECLARE(std::string, kk_renderer);
REXCVAR_DECLARE(std::string, kk_fullscreen_mode);
REXCVAR_DECLARE(std::string, kk_picture_size);
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
const std::array<RenderPreset, 5>& RenderPresets();
// resolution_scale (1-8) a preset gives for an output height; 0 for "custom".
int RenderScaleFor(std::string_view preset, int output_height);
// The temporal upscaler's output size (DECISIONS 16 Oct): the resolution chosen in the launcher
// (screen_w x screen_h: the fullscreen mode, the desktop's, or the window's size) as the largest 16:9
// picture inside it (the game's shape: the presenter letterboxes or stretches it as before). Never above
// the chosen resolution. cap_w/cap_h above 0 (a Custom render scale): at most that size too. aspect: the
// picture's shape (PictureAspectFor, ultrawide_logic.h) instead of 16:9.
std::pair<int, int> UpscalerOutputSize(int screen_w, int screen_h, int cap_w = 0, int cap_h = 0,
                                       double aspect = 16.0 / 9.0);
// The same with the current settings: a Custom render quality's draw resolution scale is the cap.
std::pair<int, int> UpscalerOutputFor(int screen_w, int screen_h, double aspect = 16.0 / 9.0);
// The frame the game draws without a temporal upscaler (DECISIONS 16 Oct, late): the draw resolution
// scale's size (draw_w x draw_h, 1280x720 times the scale), never above the chosen resolution's 16:9
// picture (UpscalerOutputSize). Each axis is the smaller of the two, so a 1080p screen draws 1920x1080
// where its Native scale (2) would draw 2560x1440. Below the chosen size it stays as it is (900p: 720p).
// Ultrawide (aspect wider than 16:9): the draw size is widened to that shape first.
std::pair<int, int> NoUpscalerFrameSize(int screen_w, int screen_h, int draw_w, int draw_h,
                                        double aspect = 16.0 / 9.0);
// Applies kk_render_quality for the given output size (call before the GPU starts). With a temporal
// upscaler it also sets native_upscaler_output (not saved) to UpscalerOutputFor(the output size); without
// one, native_frame_size (not saved) to NoUpscalerFrameSize(the output size, the draw resolution scale's).
// A screen or window wider than 16:9 (PictureAspectFor) also sets native_display_aspect and UltrawideFactor.
void ApplyRenderPreset(int output_width, int output_height);
// How many times wider the game's 3D views are (ultrawide; 1 = unchanged), set by ApplyRenderPreset.
float UltrawideFactor();
// Task 360 (Beta 17): ultrawide is built in and always on. KK_ULTRAWIDE=0 (developers only) switches it off.
bool UltrawideSwitchedOff();
// The picture's shape for the chosen resolution: its own shape clamped to 16:9..32:9 (16:9 when switched off).
double PictureAspectFor(int output_width, int output_height);
// Task 363: the ultrawide picture size chosen in a resolution list (kk_picture_size, ultrawide_logic.h
// ParsePictureSize), drawn whatever the window or screen and fitted into it (presenter: native_display_aspect).
// nullopt: none chosen (the window's or screen's own size, as before) or ultrawide switched off.
std::optional<std::pair<int, int>> ChosenPictureSize();

// Writes the current settings (skipping command-line/env overrides) to `path`.
bool SaveSettings(const std::filesystem::path& path, bool include_temporary = false);
// Temporary overlay trials are excluded from every ordinary settings save.
void SetTemporarySettingBaseline(const std::string& name, const std::string& value);
void SetTemporarySettingTrialValue(const std::string& name, const std::string& value);
void ClearTemporarySettingBaselines();
// Kong mods (mods.cpp): a settings mod's value, put over the player's own for this run only. Saves keep the
// player's value while the mod's is in place (mods_logic.h ValueToSave). False if the setting refused it.
bool SetModSetting(const std::string& name, const std::string& value);
void ClearModSettings();

// Changes a cvar's default; values from the config file or command line still win.
void SetCvarDefault(std::string_view name, std::string_view value);

// Port defaults that differ from ReXGlue's (call before the config is loaded).
void ApplyPortDefaults();
// First launch selects and persists a hardware tier. Existing installs are left alone.
void InitializeHardwareRecommendation(const std::filesystem::path& config_path, int width, int height);
// Recommended refreshes defaults only; explicit player settings always win.
// first_setup selects the task 338 policy only while assigning a new install's tier.
void ApplyHardwareRecommendation(bool detect, int width, int height, bool first_setup = false);
// Task 350a: the recommendation a new install gets (its tier detected with the first-setup policy), for an existing
// install going through the first-time setup again (setup_again.h) after its settings went back to the defaults.
void ApplyFirstSetupHardwareRecommendation(int width, int height);
// Task 350a (owner, 21 Oct): DirectX 12 is the default graphics API on Windows, for existing installs too. Once, for
// a settings file from before Beta 14 (kk_settings_version under 4), native_backend becomes d3d12; the player's
// later choices stay. Call after the settings file is read and before the renderer starts (it reads native_backend
// only then), before startup recovery applies a fallback. Nothing on Linux (Vulkan). True if it changed the API.
bool MigrateGraphicsApi();

// The default upscaler (Beta 3, docs/upscalers.md "The default"): with the native renderer, the best
// temporal upscaler this PC runs (native_upscaler): DLSS on an NVIDIA card that supports it, FSR 4 on
// an AMD card that runs it (Direct3D 12 only), else FSR 3.1, XeSS on an Intel card, FSR 3.1 on any
// other card that runs it; FSR 1 (present_effect) where none runs. NIS
// is a choice only. What runs is the launcher's question to the plugin (kk_upscalers_offered, kept in
// the settings file); before it has answered once, the card's vendor decides (the game falls back to
// FSR 3.1 itself if the guess cannot run).
// DefaultUpscaler: present_effect's default ("bilinear" under a temporal upscaler, else "fsr").
std::string DefaultUpscaler();
// native_upscaler's default ("off" or the upscaler).
std::string DefaultTemporalUpscaler();
// Sets both defaults (SetCvarDefault: values the player chose stay) for the renderer in use. Called
// once the settings are read, whenever the renderer is decided and when the launcher's question is
// answered. (A settings file from before Beta 3 that chose FSR 1, NIS or Off keeps it: ApplyFixedSettings.)
void ApplyUpscalerDefaults();
// The launcher's answer: "none" or the upscalers offered ("dlss,fsr3,xess"). Applies the defaults.
void SetOfferedUpscalers(const std::string& offered);
// Once the settings file is read: brings one from an older version up to date
// (preserving explicit choices; version 3 enables GI for old presets; version 4 is
// DirectX 12 on Windows, MigrateGraphicsApi, done before the renderer starts; version 5
// turns HDR off once, as it is experimental, F8 panel only) and saves it; a new install
// just gets the current defaults.
void MigrateSettings(const std::filesystem::path& config_path);
// Beta 9: an old Show FPS (kk_show_fps = true) becomes Performance metrics "fps" (run after the settings load).
void MigrateFrameCounter();
// Beta 10: HDR follows Windows; an older file's moved Peak/Paper white sliders stay Manual.
void MigrateHdrSettings();
// Settings the player can't change (shader preparing is always Balanced).
void ApplyFixedSettings();

// Derive Vulkan's allowed present modes from VSync before the presenter is created.
void ApplyVSyncSettings();

// Forces the ReXGlue settings the port depends on (see settings.cpp).
void ApplyRuntimeOverrides();
// Reports the console video mode 1280x720 whatever the window size (part of ApplyRuntimeOverrides; also
// called before the GPU starts, which sizes its frame from it).
void ApplyVideoMode();

// The Original look (launcher: Graphics > Look): the Xbox 360's own settings,
// as cvar name and value.
const std::vector<std::pair<const char*, const char*>>& OriginalLookSettings();

// With kk_original_look on, sets those values (in case the settings file was
// edited). Call before the GPU starts, as it sets the render resolution.
void EnforceOriginalLook();

// Normalize old renderer/API config values before graphics setup; true if migrated.
bool NormalizeRendererSettings();

}  // namespace kk
