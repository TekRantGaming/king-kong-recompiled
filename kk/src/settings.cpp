#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

#include <rex/logging.h>
#include <rex/string.h>

REXCVAR_DEFINE_BOOL(kk_launcher, true, "KK",
                    "Show the launcher before starting the game (hold Shift at start to force it)");
REXCVAR_DEFINE_BOOL(kk_skip_launcher, false, "KK",
                    "Internal: skip the launcher once (used when it relaunches the game)");
REXCVAR_DEFINE_BOOL(kk_check_updates, true, "KK", "Check GitHub for a newer version when the launcher opens");
REXCVAR_DEFINE_BOOL(kk_launcher_music, true, "KK", "Play the game's main menu music in the launcher");
REXCVAR_DEFINE_INT32(kk_launcher_music_volume, 50, "KK", "The launcher's music volume, 0-100").range(0, 100);
REXCVAR_DEFINE_BOOL(kk_launcher_sounds, true, "KK", "The game's menu sounds when moving around and choosing in the launcher");
REXCVAR_DEFINE_INT32(kk_launcher_sounds_volume, 50, "KK", "The launcher's interface sound volume, 0-100").range(0, 100);
REXCVAR_DEFINE_INT32(kk_settings_version, 0, "KK",
                     "Internal: which defaults the settings file was written with (see MigrateSettings)");
REXCVAR_DEFINE_STRING(kk_last_version, "", "KK",
                      "Last port version the launcher has shown (it shows What's new once after an update)");
REXCVAR_DEFINE_INT32(kk_frame_rate, 30, "KK/Video",
                     "Frame-rate cap: 30, 60, 90, 120, 144, 165, 240, or 0 for unlimited");
REXCVAR_DEFINE_STRING(kk_render_quality, "quality", "KK/Video",
                      "Render resolution relative to the output: native, quality, balanced, performance, "
                      "ultra_performance, supersample, or custom (use resolution_scale)");
REXCVAR_DEFINE_STRING(kk_button_prompts, "xbox360", "KK/Controls",
                      "Button pictures shown in the game: xbox360, xbox_series, ps5, ps2 or keyboard")
    .allowed({"xbox360", "xbox_series", "ps5", "ps2", "keyboard"});
REXCVAR_DEFINE_BOOL(kk_show_fps, false, "KK/Video", "Show a frame-rate counter (toggle in game with F2)");
REXCVAR_DEFINE_INT32(kk_deadzone, 5, "KK/Controls", "Extra stick deadzone in percent (0-50)");
REXCVAR_DEFINE_INT32(kk_camera_sensitivity, 150, "KK/Controls", "Camera (right stick) sensitivity in percent");
REXCVAR_DEFINE_BOOL(kk_camera_modern, true, "KK/Controls",
                    "Camera response: true = the same in every direction, false = the Xbox 360's own");
REXCVAR_DEFINE_BOOL(kk_achievement_toasts, true, "KK/Achievements", "Show achievement notifications");
REXCVAR_DEFINE_BOOL(kk_achievement_sound, true, "KK/Achievements", "Play the achievement sound");
REXCVAR_DEFINE_STRING(kk_achievement_sound_file, "Xbox_360.wav", "KK/Achievements",
                      "Achievement sound from the sounds folder (empty, or a file that isn't there = built-in chime)");
REXCVAR_DEFINE_INT32(kk_achievement_volume, 80, "KK/Achievements", "Achievement sound volume in percent");
REXCVAR_DEFINE_BOOL(kk_vibration, true, "KK/Controls", "Controller vibration");
REXCVAR_DEFINE_INT32(kk_vibration_strength, 100, "KK/Controls", "Vibration strength in percent");
REXCVAR_DEFINE_BOOL(kk_invert_rs_x, false, "KK/Controls", "Invert right stick horizontal (camera)");
REXCVAR_DEFINE_BOOL(kk_invert_rs_y, false, "KK/Controls", "Invert right stick vertical");
REXCVAR_DEFINE_BOOL(kk_invert_ls_x, false, "KK/Controls", "Invert left stick horizontal");
REXCVAR_DEFINE_BOOL(kk_invert_ls_y, false, "KK/Controls", "Invert left stick vertical");
REXCVAR_DEFINE_INT32(kk_hitch_report_ms, 0, "KK/Debug",
                     "Write logs/hitch-*.txt (where every thread was) when a frame takes longer than this (0 = off)");
REXCVAR_DEFINE_BOOL(kk_cheats, false, "KK/Cheats", "Switch on the cheats chosen below when the game starts");
#define KK_CHEAT_CVAR(id, label) REXCVAR_DEFINE_BOOL(kk_cheat_##id, false, "KK/Cheats", label)
KK_CHEAT_CVAR(chapters, "All chapters (KKst0ry)");
KK_CHEAT_CVAR(bonus, "All bonus content (KKmuseum)");
KK_CHEAT_CVAR(healing, "Fast healing for Jack (8wonder)");
KK_CHEAT_CVAR(one_hit, "One-hit kills with bullets (GrosBras)");
KK_CHEAT_CVAR(ammo, "999 bullets (KK 999 mun)");
KK_CHEAT_CVAR(spears, "Unlimited spears (lance 1nf)");
KK_CHEAT_CVAR(revolver, "Revolver (KKtigun)");
KK_CHEAT_CVAR(machine_gun, "Machine gun (KKcapone)");
KK_CHEAT_CVAR(shotgun, "Shotgun (KKsh0tgun)");
KK_CHEAT_CVAR(sniper, "Sniper rifle (KKsn1per)");
#undef KK_CHEAT_CVAR
REXCVAR_DEFINE_BOOL(kk_motion_blur, false, "KK/Graphics", "The game's motion blur effect");
REXCVAR_DEFINE_BOOL(kk_fog, true, "KK/Graphics", "The game's distance fog (the haze over far scenery)");
REXCVAR_DEFINE_BOOL(kk_big_blur, false, "KK/Graphics",
                    "The game's full-screen blur in some levels (Necropolis, Brontosaurus), sized for 720p");
REXCVAR_DEFINE_BOOL(kk_original_look, false, "KK/Graphics",
                    "Play with the Xbox 360's own settings (launcher: Graphics > Look)");
REXCVAR_DEFINE_STRING(kk_modern_settings, "", "KK/Graphics",
                      "Internal: the Modern settings to put back when leaving the Original look");
REXCVAR_DEFINE_INT32(kk_fov, 69, "KK/Gameplay",
                     "Field of view in degrees for Jack's camera (69 = original); other cameras widen to match");
REXCVAR_DEFINE_BOOL(kk_skip_intros, true, "KK/Gameplay",
                    "Skip the Ubisoft, Universal and WingNut logo movies when the game starts");
REXCVAR_DEFINE_BOOL(kk_toggle_aim, false, "KK/Controls",
                    "Aim (left trigger) toggles: press once to raise the gun, again to lower it");

// Button remapping: kk_map_<physical> = <game button> (or "none").
#define KK_MAP_CVAR(id, def, label) \
  REXCVAR_DEFINE_STRING(kk_map_##id, def, "KK/Controls/Remap", label " sends")
KK_MAP_CVAR(dpad_up, "dpad_up", "D-pad up");
KK_MAP_CVAR(dpad_down, "dpad_down", "D-pad down");
KK_MAP_CVAR(dpad_left, "dpad_left", "D-pad left");
KK_MAP_CVAR(dpad_right, "dpad_right", "D-pad right");
KK_MAP_CVAR(start, "start", "Start");
KK_MAP_CVAR(back, "back", "Back");
KK_MAP_CVAR(ls, "ls", "Left stick click");
KK_MAP_CVAR(rs, "rs", "Right stick click");
KK_MAP_CVAR(lb, "lb", "Left bumper");
KK_MAP_CVAR(rb, "rb", "Right bumper");
KK_MAP_CVAR(a, "a", "A");
KK_MAP_CVAR(b, "b", "B");
KK_MAP_CVAR(x, "x", "X");
KK_MAP_CVAR(y, "y", "Y");
KK_MAP_CVAR(lt, "lt", "Left trigger");
KK_MAP_CVAR(rt, "rt", "Right trigger");
#undef KK_MAP_CVAR

namespace kk {
namespace {

constexpr std::array<PadInfo, kPadCount> kPads = {{
    {"dpad_up", "D-pad Up", 0x0001},
    {"dpad_down", "D-pad Down", 0x0002},
    {"dpad_left", "D-pad Left", 0x0004},
    {"dpad_right", "D-pad Right", 0x0008},
    {"start", "Start", 0x0010},
    {"back", "Back", 0x0020},
    {"ls", "Left Stick Click", 0x0040},
    {"rs", "Right Stick Click", 0x0080},
    {"lb", "Left Bumper", 0x0100},
    {"rb", "Right Bumper", 0x0200},
    {"a", "A", 0x1000},
    {"b", "B", 0x2000},
    {"x", "X", 0x4000},
    {"y", "Y", 0x8000},
    {"lt", "Left Trigger", 0},
    {"rt", "Right Trigger", 0},
}};

std::string& MapStorage(Pad p) {
  switch (p) {
    case Pad::kDpadUp: return REXCVAR_GET(kk_map_dpad_up);
    case Pad::kDpadDown: return REXCVAR_GET(kk_map_dpad_down);
    case Pad::kDpadLeft: return REXCVAR_GET(kk_map_dpad_left);
    case Pad::kDpadRight: return REXCVAR_GET(kk_map_dpad_right);
    case Pad::kStart: return REXCVAR_GET(kk_map_start);
    case Pad::kBack: return REXCVAR_GET(kk_map_back);
    case Pad::kLeftThumb: return REXCVAR_GET(kk_map_ls);
    case Pad::kRightThumb: return REXCVAR_GET(kk_map_rs);
    case Pad::kLeftShoulder: return REXCVAR_GET(kk_map_lb);
    case Pad::kRightShoulder: return REXCVAR_GET(kk_map_rb);
    case Pad::kA: return REXCVAR_GET(kk_map_a);
    case Pad::kB: return REXCVAR_GET(kk_map_b);
    case Pad::kX: return REXCVAR_GET(kk_map_x);
    case Pad::kY: return REXCVAR_GET(kk_map_y);
    case Pad::kLeftTrigger: return REXCVAR_GET(kk_map_lt);
    default: return REXCVAR_GET(kk_map_rt);
  }
}

}  // namespace

const PadInfo& GetPadInfo(Pad pad) { return kPads[static_cast<size_t>(pad)]; }

Pad ParsePad(std::string_view id) {
  std::string lower(id);
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (size_t i = 0; i < kPadCount; ++i)
    if (lower == kPads[i].id) return static_cast<Pad>(i);
  return Pad::kNone;
}

Pad GetMapping(Pad physical) { return ParsePad(MapStorage(physical)); }

void SetMapping(Pad physical, Pad target) {
  rex::cvar::SetFlagByName(std::string("kk_map_") + GetPadInfo(physical).id,
                           target == Pad::kNone ? "none" : GetPadInfo(target).id);
}

const std::array<RenderPreset, 6>& RenderPresets() {
  // Ratios follow the usual upscaler naming (output / render); the game renders
  // at integer multiples of its native 720p, so the nearest multiple is used.
  static const std::array<RenderPreset, 6> kPresets = {{
      {"supersample", "Supersample", 0.5},
      {"native", "Native", 1.0},
      {"quality", "Quality", 1.5},
      {"balanced", "Balanced", 1.7},
      {"performance", "Performance", 2.0},
      {"ultra_performance", "Ultra Performance", 3.0},
  }};
  return kPresets;
}

int RenderScaleFor(std::string_view preset, int output_height) {
  for (const auto& p : RenderPresets()) {
    if (preset != p.id) continue;
    const double target = std::max(1, output_height) / p.ratio;
    const int scale = std::clamp(static_cast<int>(std::lround(target / 720.0)), 1, 8);
    // In 720p steps Performance often rounds to the same step as Quality (both
    // 1440p at 4K), so it goes one step below Quality where there is one.
    if (preset == "performance") return std::max(1, std::min(scale, RenderScaleFor("quality", output_height) - 1));
    return scale;
  }
  return 0;  // custom
}

void ApplyRenderPreset(int output_height) {
  const int scale = RenderScaleFor(REXCVAR_GET(kk_render_quality), output_height);
  if (scale <= 0) return;  // custom: the player's resolution_scale is used as is
  // The runtime only honours resolution_scale when it differs from its default,
  // so a preset drives the per-axis scales instead (read directly). They are set
  // as defaults so the derived value is not written to the config, and any
  // resolution_scale left over from an earlier custom choice is cleared.
  rex::cvar::ResetToDefault("resolution_scale");
  for (const char* axis : {"draw_resolution_scale_x", "draw_resolution_scale_y"}) {
    SetCvarDefault(axis, std::to_string(scale));
    rex::cvar::ResetToDefault(axis);
  }
  REXLOG_INFO("KK: render preset {} at {}p output -> {}x ({}p)", REXCVAR_GET(kk_render_quality),
              output_height, scale, scale * 720);
}

bool SaveSettings(const std::filesystem::path& path) {
  // Like rex::cvar::SaveConfig, but values that came from the command line or
  // environment (e.g. --game_data_root, --log_file) are one-off overrides and
  // are not written back.
  std::string out = "# King Kong settings (edited by the launcher)\n";
  for (const auto& e : rex::cvar::GetRegistry()) {
    if (e.type == rex::cvar::FlagType::Command || e.is_debug_only) continue;
    if (e.source == rex::cvar::Source::kCommandLine || e.source == rex::cvar::Source::kEnvironment) continue;
    // Set by the port at every start (ApplyRuntimeOverrides).
    if (e.name == "video_mode_width" || e.name == "video_mode_height") continue;
    const std::string value = e.getter();
    if (value == e.default_value) continue;
    out += e.name + " = ";
    if (e.type == rex::cvar::FlagType::String) {
      out += '"';
      for (char c : value) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
      }
      out += '"';
    } else {
      out += value;
    }
    out += '\n';
  }
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    REXLOG_ERROR("KK: cannot write settings to {}", path.string());
    return false;
  }
  file << out;
  return static_cast<bool>(file);
}

void MigrateSettings(const std::filesystem::path& config_path) {
  // 2: FSR 1 at Quality became the default (it was no upscaler at Native).
  // A settings file only keeps values that differ from the defaults, so an
  // older one that has no upscaler setting meant the old default: keep it, so
  // an update doesn't change a player's picture (or turn their High, Ultra or
  // Steam Deck preset into Custom). New installs get the new defaults.
  constexpr int32_t kVersion = 2;
  if (REXCVAR_GET(kk_settings_version) >= kVersion) return;
  std::error_code ec;
  if (std::filesystem::exists(config_path, ec)) {
    static constexpr std::pair<const char*, const char*> kOldDefaults[] = {{"present_effect", "bilinear"},
                                                                           {"kk_render_quality", "native"}};
    for (const auto& [name, value] : kOldDefaults)
      for (const auto& e : rex::cvar::GetRegistry())
        if (e.name == name && e.source == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
    REXLOG_INFO("KK: settings from an older version: kept its upscaler settings ({}, {})",
                rex::cvar::GetFlagByName("present_effect"), rex::cvar::GetFlagByName("kk_render_quality"));
    rex::cvar::SetFlagByName("kk_settings_version", std::to_string(kVersion));
    SaveSettings(config_path);
    return;
  }
  // A new install: saved with the rest later (writing the file now would look
  // like an earlier run to the launcher's What's new).
  rex::cvar::SetFlagByName("kk_settings_version", std::to_string(kVersion));
}

void ApplyFixedSettings() {
  // Shader preparing is always Balanced (no longer a setting): Wait paused the
  // game for up to 2 seconds the first time an effect appeared, and
  // Background made objects vanish or flash. A settings file from before keeps
  // no say in it (the values equal the defaults, so they aren't saved again).
  rex::cvar::SetFlagByName("async_shader_compilation", "true");
  rex::cvar::ResetToDefault("async_shader_wait_ms");
}

void SetCvarDefault(std::string_view name, std::string_view value) {
  for (auto& e : rex::cvar::GetRegistry()) {
    if (e.name != name) continue;
    e.default_value = value;
    if (e.source == rex::cvar::Source::kDefault) e.setter(value);  // config/CLI still win
    return;
  }
}

void ApplyPortDefaults() {
  // Windowed by default so the launcher isn't a giant fullscreen dialog.
  SetCvarDefault("fullscreen", "false");
  // VSync off (the frame rate cap paces the game), FXAA, 4x texture filtering.
  SetCvarDefault("d3d12_allow_variable_refresh_rate_and_tearing", "true");
  SetCvarDefault("swap_post_effect", "fxaa");
  SetCvarDefault("anisotropic_override", "3");
  // AMD FSR 1 at Quality (kk_render_quality), where this build has it.
  for (auto& e : rex::cvar::GetRegistry()) {
    const auto& allowed = e.constraints.allowed_values;
    if (e.name == "present_effect" && std::find(allowed.begin(), allowed.end(), "fsr") != allowed.end())
      SetCvarDefault("present_effect", "fsr");
  }
  // Shader preparing: Balanced. New pipelines are created on background
  // threads, many at once, and a frame may wait up to async_shader_wait_ms in
  // total for them (a setting added to this port's build of the GPU plugin),
  // drawing without one only if it still isn't ready. Creating them one at a
  // time while the game waits (Wait) froze V-Rex for up to 2 seconds the first
  // time; never waiting (Background) skips the draws, so objects vanish or
  // flash for a moment instead.
  SetCvarDefault("async_shader_compilation", "true");
  SetCvarDefault("async_shader_wait_ms", std::to_string(kBalancedShaderWaitMs));
}

const std::vector<std::pair<const char*, const char*>>& OriginalLookSettings() {
  // 720p at 30 FPS, the console's anti-aliasing (its own 2x MSAA, no FXAA) and
  // texture filtering, Jack's 69 degree field of view, motion blur, fog and
  // the screen blur on, no ambient occlusion, no upscaler.
  static const std::vector<std::pair<const char*, const char*>> settings = {
      {"present_effect", "bilinear"},
      {"kk_render_quality", "custom"}, {"resolution_scale", "1"}, {"swap_post_effect", "none"},
      {"anisotropic_override", "-1"},  {"ao_mode", "0"},          {"ao_strength", "1"},
      {"kk_motion_blur", "true"},      {"kk_fog", "true"},        {"kk_big_blur", "true"},
      {"kk_frame_rate", "30"},         {"kk_fov", "69"},
  };
  return settings;
}

void EnforceOriginalLook() {
  if (!REXCVAR_GET(kk_original_look)) return;
  for (const auto& [name, value] : OriginalLookSettings()) rex::cvar::SetFlagByName(name, value);
}

void ApplyRuntimeOverrides() {
  // ReXGlue submits GPU work at every primary ring-buffer end by default, which
  // stalls this title to ~27 ms per frame (the 30 FPS "lock"). Batching lets
  // it run at any rate; the title's delta-time keeps game speed correct.
  // Guest vsync only paces the emulated console (60 Hz vblank + coarse sleeps
  // in GPU waits). Frame pacing is done by kk_frame_rate instead.
  for (const char* name : {"d3d12_submit_on_primary_buffer_end", "vsync"}) {
    if (!rex::cvar::SetFlagByName(name, "false"))
      REXLOG_WARN("KK: could not set {} (cvar not registered)", name);
  }
  REXLOG_INFO("KK: frame-rate cap {}", REXCVAR_GET(kk_frame_rate));

  // Shader preparing: Balanced waits up to half a frame at the frame-rate cap
  // for pipelines being prepared (16 ms at 30 FPS, 8 at 60, 4 from 120 or
  // unlimited), which fits in the frame's spare time. A fixed 16 ms caused a
  // hitch at 60 FPS.
  {
    const int32_t fps = REXCVAR_GET(kk_frame_rate);
    const int32_t wait = fps > 0 ? std::clamp(500 / fps, 4, kBalancedShaderWaitMs) : 4;
    SetCvarDefault("async_shader_wait_ms", std::to_string(wait));
    REXLOG_INFO("KK: shader preparing {}, waiting up to {} ms a frame",
                rex::cvar::GetFlagByName("async_shader_compilation") == "true" ? "in the background" : "on demand",
                rex::cvar::GetFlagByName("async_shader_wait_ms"));
  }

  // The game sizes its frame from the console's video mode: 1280x720 on an HD
  // mode, but the whole game at 640x480 (stretched to 16:9) on a mode under 720
  // lines. The runtime reports the window size as that mode whenever a window
  // size is set, even in fullscreen, and window sizes are kept in DPI-scaled
  // units (1280x720 at 125% is saved as 1024x576), so some players got
  // 640x480 everywhere (#32). Always report 720p, like a console on a 720p TV;
  // the render resolution scales it up. A non-default value is needed for it
  // to win over the window size, so the default moves to 0 (and SaveSettings
  // leaves these out). This runs after the window is created, so its size
  // doesn't change.
  for (const auto& [name, value] :
       {std::pair{"video_mode_width", "1280"}, std::pair{"video_mode_height", "720"}}) {
    SetCvarDefault(name, "0");
    rex::cvar::SetFlagByName(name, value);
  }
  REXLOG_INFO("KK: video mode {}x{} (window {}x{})", rex::cvar::GetFlagByName("video_mode_width"),
              rex::cvar::GetFlagByName("video_mode_height"), rex::cvar::GetFlagByName("window_width"),
              rex::cvar::GetFlagByName("window_height"));

  // The draw resolution scale the GPU uses (same rule as the runtime's
  // TextureCache::GetConfigDrawResolutionScale), for bug reports.
  auto axis = [](const char* name) {
    if (rex::cvar::HasNonDefaultValue("resolution_scale") && !rex::cvar::HasNonDefaultValue(name))
      return rex::cvar::GetFlagByName("resolution_scale");
    return rex::cvar::GetFlagByName(name);
  };
  REXLOG_INFO("KK: draw resolution scale {}x{}", axis("draw_resolution_scale_x"), axis("draw_resolution_scale_y"));

  // The GPU logs every occlusion ("viz") query at info level: thousands of
  // lines a second for this title, written from the GPU thread, plus a log
  // rotation every few seconds. Keep its warnings, drop the chatter, unless
  // the player asked for more detailed logs.
  if (REXCVAR_GET(log_level) == "info") {
    if (auto gpu = rex::FindCategory("gpu")) rex::SetCategoryLevel(*gpu, spdlog::level::warn);
  }
}

}  // namespace kk
