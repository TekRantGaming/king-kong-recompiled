#include "settings.h"
#include "mods_logic.h"
#include <map>
#include <mutex>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

#include <rex/logging.h>
#include <rex/string.h>

#include "platform.h"
#include "hardware_recommendation.h"
#include "graphics_presets.h"
#include "ultrawide_logic.h"

// Standalone settings test targets also compile this file without the game definition.
#ifndef KK_COOP_DEFAULT_ON
#define KK_COOP_DEFAULT_ON 0
#endif

REXCVAR_DEFINE_BOOL(kk_coop_enabled, KK_COOP_DEFAULT_ON != 0, "KK", "Enable the optional co-op session")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(kk_launcher, true, "KK",
                    "Show the launcher before starting the game (hold Shift at start to force it)");
REXCVAR_DEFINE_BOOL(kk_skip_launcher, false, "KK",
                    "Internal: skip the launcher once (used when it relaunches the game)");
REXCVAR_DEFINE_STRING(kk_start, "", "KK",
                      "Internal: what the game starts with, from the launcher's Play menu (continue:<slot>, "
                      "new:<slot>[:<name>], chapter:<slot>:<entry>[:<name>]); never saved");
REXCVAR_DEFINE_BOOL(kk_check_updates, true, "KK", "Check GitHub for a newer version when the launcher opens");
REXCVAR_DEFINE_BOOL(kk_online_count, true, "KK",
                    "Show how many people are playing, with an anonymous check-in every 5 minutes (see online.h)");
REXCVAR_DEFINE_BOOL(kk_online_asked, false, "KK", "Internal: the launcher has asked about the online player count");
REXCVAR_DEFINE_BOOL(kk_discord_status, true, "KK",
                    "Show on your Discord profile that you're playing, while the Discord app runs on this PC "
                    "(discord_presence.h)");
REXCVAR_DEFINE_STRING(kk_discord_app_id, "", "KK/Debug",
                      "Discord application ID for the Discord status (empty: the port's own; testing)")
    .debug_only();
REXCVAR_DEFINE_STRING(kk_install_id, "", "KK",
                      "Internal: the random ID the online player count checks in with (deleted when it's off)");
REXCVAR_DEFINE_STRING(kk_update_channel, "stable", "KK",
                      "Which versions the launcher offers: stable, or beta (test builds of what's coming next)")
    .allowed({"stable", "beta"});
REXCVAR_DEFINE_BOOL(kk_launcher_music, true, "KK", "Play the game's main menu music in the launcher");
REXCVAR_DEFINE_INT32(kk_launcher_music_volume, 50, "KK", "The launcher's music volume, 0-100").range(0, 100);
REXCVAR_DEFINE_BOOL(kk_launcher_sounds, true, "KK", "The game's menu sounds when moving around and choosing in the launcher");
REXCVAR_DEFINE_INT32(kk_launcher_sounds_volume, 50, "KK", "The launcher's interface sound volume, 0-100").range(0, 100);
REXCVAR_DEFINE_BOOL(kk_notify_downloads, true, "KK",
                    "A system notification when a content download finishes or stops while the launcher is minimised or in the background");
REXCVAR_DEFINE_INT32(kk_settings_version, 0, "KK",
                     "Internal: which defaults the settings file was written with (see MigrateSettings)");
REXCVAR_DEFINE_STRING(kk_hardware_tier, "", "KK",
                      "Internal: first-launch hardware recommendation; empty for existing installs")
    .allowed({"", "low", "mid", "high"});
REXCVAR_DEFINE_STRING(kk_shaders_prepared, "", "KK",
                      "Internal: the port version whose one shader preparation has run; shaders are prepared again "
                      "only after an update or when the player asks (owner rule, Beta 8)");
REXCVAR_DEFINE_STRING(kk_last_version, "", "KK",
                      "Last port version the launcher has shown (it shows What's new once after an update)");
REXCVAR_DEFINE_STRING(kk_setup_version, "", "KK",
                      "Internal: the port version whose setup steps this player has had (the first-time setup, or "
                      "the one-time New in this update screen); 'waiting' while that screen is still to be shown");
REXCVAR_DEFINE_INT32(kk_frame_rate, 30, "KK/Video",
                     "Frame-rate cap: 30, 60, 90, 120, 144, 165, 240, or 0 for unlimited");
REXCVAR_DEFINE_BOOL(kk_hfr_fixes, true, "KK/Video",
                    "Above 30 FPS, fix the game systems that assume 30 steps a second (docs/high-frame-rate.md); "
                    "no effect at 30 FPS");
REXCVAR_DEFINE_STRING(kk_render_quality, "quality", "KK/Video",
                      "Render resolution relative to the output: native, quality, balanced, performance, "
                      "ultra_performance, or custom (use resolution_scale)");
REXCVAR_DEFINE_STRING(kk_button_prompts, "auto", "KK/Controls",
                      "Button pictures shown in the game: auto (matches your controller), xbox360, xbox_series, ps5, "
                      "ps2, steam_deck or keyboard")
    .allowed({"auto", "xbox360", "xbox_series", "ps5", "ps2", "steam_deck", "keyboard"});
// Beta 9: the frame counter became Performance metrics (kk_performance_metrics); an old true is moved over
// once (MigrateFrameCounter) and this one is otherwise unused.
REXCVAR_DEFINE_BOOL(kk_show_fps, false, "KK/Video", "Old: replaced by kk_performance_metrics (true = fps)");
REXCVAR_DEFINE_STRING(kk_performance_metrics, "off", "KK/Video",
                      "Performance metrics overlay while playing: off, fps (frame rate and frame time) or full "
                      "(adds a frame time graph, GPU and CPU load, temperatures, clocks, memory, API and "
                      "upscaler); F2 switches between them")
    .allowed({"off", "fps", "full"});
// Brightness and gamma (owner, 18 Oct; GitHub issue #44): the game's picture only, applied live by the native
// renderer at Present (native-renderer/backend/picture_adjust.h). The defaults are the game's own picture.
REXCVAR_DEFINE_INT32(kk_brightness, 0, "KK/Video",
                     "Picture brightness (black level), -50 to 50; 0 = the game's own picture").range(-50, 50);
REXCVAR_DEFINE_DOUBLE(kk_gamma, 1.0, "KK/Video",
                      "Picture gamma (middle tones), 0.5 to 1.5; above 1 is brighter; 1 = the game's own picture")
    .range(0.5, 1.5);
// HDR follows Windows (owner, 17/18 Oct; hdr_logic.h): kk_hdr and kk_hdr_levels decide the presenter's
// present_hdr, present_hdr_peak and present_hdr_paper_white (hdr_display.cpp sets them at every start and while
// the settings pages are open). Those three are then not saved, except Peak and Paper white in Manual.
REXCVAR_DEFINE_STRING(kk_hdr, "auto", "KK/Video",
                      "HDR output: auto (follow Windows HDR for the game's screen), on or off")
    .allowed({"auto", "on", "off"});
REXCVAR_DEFINE_STRING(kk_hdr_levels, "auto", "KK/Video",
                      "HDR peak brightness and paper white: auto (from Windows HDR Calibration and Windows' SDR "
                      "content brightness) or manual (present_hdr_peak, present_hdr_paper_white)")
    .allowed({"auto", "manual"});
REXCVAR_DEFINE_BOOL(kk_ps2_cutscenes, false, "KK/Gameplay",
                    "Play player-owned PS2 between-mission movies during story progression");
REXCVAR_DEFINE_STRING(kk_ps2_movie_folder, "", "KK/Gameplay",
                      "Folder containing player-owned BK_xxxx.mp4 movies (empty disables playback)");
REXCVAR_DEFINE_INT32(kk_deadzone, 5, "KK/Controls", "Extra stick deadzone in percent (0-50)");
REXCVAR_DEFINE_INT32(kk_camera_sensitivity, 150, "KK/Controls", "Camera (right stick) sensitivity in percent");
REXCVAR_DEFINE_BOOL(kk_camera_modern, true, "KK/Controls",
                    "Camera response: true = the same in every direction, false = the Xbox 360's own");
REXCVAR_DEFINE_BOOL(kk_achievement_toasts, true, "KK/Achievements", "Show achievement notifications");
REXCVAR_DEFINE_BOOL(kk_achievement_sound, true, "KK/Achievements", "Play the achievement sound");
REXCVAR_DEFINE_STRING(kk_achievement_style, "xbox360", "KK/Achievements",
                      "Achievement pop-up style: xbox360 or steam")
    .allowed({"xbox360", "steam"});
REXCVAR_DEFINE_STRING(kk_achievement_sound_file, "Xbox_360.wav", "KK/Achievements",
                      "Achievement sound from the sounds folder (empty, or a file that isn't there = built-in chime)");
REXCVAR_DEFINE_INT32(kk_achievement_volume, 80, "KK/Achievements", "Achievement sound volume in percent");
REXCVAR_DEFINE_BOOL(kk_vibration, true, "KK/Controls", "Controller vibration");
REXCVAR_DEFINE_BOOL(kk_camera_shake, true, "KK/Gameplay", "Camera shake (footsteps, explosions, blows)");
REXCVAR_DEFINE_INT32(kk_camera_shake_strength, 100, "KK/Gameplay", "Camera shake strength in percent (10-200)")
    .range(10, 200);
REXCVAR_DEFINE_INT32(kk_vibration_strength, 100, "KK/Controls",
                     "Vibration strength in percent (10-200; above 100 is stronger than on the Xbox 360)");
REXCVAR_DEFINE_BOOL(kk_invert_rs_x, false, "KK/Controls", "Invert right stick horizontal (camera)");
REXCVAR_DEFINE_BOOL(kk_invert_rs_y, false, "KK/Controls", "Invert right stick vertical");
REXCVAR_DEFINE_BOOL(kk_invert_ls_x, false, "KK/Controls", "Invert left stick horizontal");
REXCVAR_DEFINE_BOOL(kk_invert_ls_y, false, "KK/Controls", "Invert left stick vertical");
REXCVAR_DEFINE_STRING(kk_original_look_keep, "", "KK/Debug",
                      "Settings (comma-separated cvar names) the Original look leaves as given, e.g. "
                      "ao_mode,ao_strength,kk_frame_rate (testing)");
// Task 295: default 1 (owner, 19 Oct; REPORT-295).
REXCVAR_DEFINE_INT32(kk_cpu_priority, 1, "KK/Performance",
                     "Favour the game's own threads when other programs load the CPU (task 295): 0 off (before), "
                     "1 game/render/vsync/audio threads above normal, 2 also Windows' multimedia scheduler (MMCSS "
                     "Games/Audio), 3 also the process above normal")
    .range(0, 3).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(kk_hitch_report_ms, 0, "KK/Debug",
                     "Write logs/hitch-*.txt (where every thread was) when a frame takes longer than this (0 = off)");
REXCVAR_DEFINE_BOOL(kk_cheats, false, "KK/Cheats", "Switch on the cheats chosen below when the game starts");
#define KK_CHEAT_CVAR(id, label) REXCVAR_DEFINE_BOOL(kk_cheat_##id, false, "KK/Cheats", label)
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
// Compatibility input only: old values are normalized before graphics setup.
REXCVAR_DEFINE_STRING(kk_renderer, "native", "KK/Graphics", "Legacy renderer selection (always native)");
REXCVAR_DEFINE_STRING(kk_fullscreen_mode, "desktop", "KK/Video",
                      "Fullscreen resolution: desktop (a borderless window at the desktop's resolution) or a display "
                      "mode WIDTHxHEIGHT@HZ, e.g. 2560x1440@144 (exclusive fullscreen at that mode)");
REXCVAR_DEFINE_STRING(kk_upscalers_offered, "", "KK/Graphics",
                      "Internal: the temporal upscalers this PC runs, as the launcher last found ('none' or "
                      "'dlss,fsr3,xess'); empty until it has asked (the default upscaler follows it)");
REXCVAR_DEFINE_STRING(kk_modern_settings, "", "KK/Graphics",
                      "Internal: the Modern settings to put back when leaving the Original look");
REXCVAR_DEFINE_STRING(kk_graphics_preset, "", "KK/Graphics",
                      "Internal: the chosen preset label when presets have identical values");
REXCVAR_DEFINE_INT32(kk_fov, 69, "KK/Gameplay",
                     "Field of view in degrees for Jack's camera (69 = original); other cameras widen to match");
REXCVAR_DEFINE_STRING(kk_aspect, "16:9", "KK/Video",
                      "Experimental (the built-in Ultrawide mod): the picture's shape, 16:9 (the game's own: nothing "
                      "changes), 21:9, 32:9 or auto (the chosen resolution's shape, 16:9 to 32:9); wider shows more "
                      "on the sides");
REXCVAR_DEFINE_BOOL(kk_skip_intros, true, "KK/Gameplay",
                    "Skip the Ubisoft, Universal and WingNut logo movies when the game starts");
REXCVAR_DEFINE_BOOL(kk_toggle_aim, false, "KK/Controls",
                    "Aim (left trigger) toggles: press once to raise the gun, again to lower it");
REXCVAR_DEFINE_DOUBLE(kk_mouse_sensitivity, 1.0, "KK/Controls",
                      "Keyboard & mouse: how far the mouse turns the camera (1 = 0.066 degrees per count)")
    .range(0.05, 10.0);
REXCVAR_DEFINE_BOOL(kk_mouse_invert_y, false, "KK/Controls", "Keyboard & mouse: mouse up looks down");
REXCVAR_DEFINE_STRING(kk_pad_prompts, "auto", "KK/Controls",
                      "Internal: the controller's button prompts, put back when Input goes back to Controller");

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
const GpuInfo& Gpu() {
  static const GpuInfo gpu = DetectGpu();
  return gpu;
}
}  // namespace
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

const std::array<RenderPreset, 5>& RenderPresets() {
  // Ratios follow the usual upscaler naming (output / render); the game renders
  // at integer multiples of its native 720p, so the nearest multiple is used.
  // (Supersample, 0.5, went in Beta 2: Display > Resolution picks the size.)
  static const std::array<RenderPreset, 5> kPresets = {{
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

namespace {
// One axis of the draw resolution scale the renderer uses (the runtime's rule: resolution_scale when the
// player set it and left the axis alone, else the axis), 1 to 8.
int DrawScaleAxis(const char* name) {
  const std::string v = rex::cvar::HasNonDefaultValue("resolution_scale") && !rex::cvar::HasNonDefaultValue(name)
                            ? rex::cvar::GetFlagByName("resolution_scale")
                            : rex::cvar::GetFlagByName(name);
  const int n = std::atoi(v.c_str());
  return n >= 1 && n <= 8 ? n : 1;
}

bool TemporalUpscalerOn() {
  if (rex::cvar::GetFlagByName("gpu_plugin") != "native") return false;
  const std::string temporal = rex::cvar::GetFlagByName("native_upscaler");
  return !temporal.empty() && temporal != "off";
}
}  // namespace

std::pair<int, int> UpscalerOutputSize(int screen_w, int screen_h, int cap_w, int cap_h, double aspect) {
  int w = std::max(screen_w, 1), h = std::max(screen_h, 1);
  if (cap_w > 0 && cap_h > 0) {
    w = std::min(w, cap_w);
    h = std::min(h, cap_h);
  }
  if (ultrawide::WidthFactor(aspect) != 1.0) {  // the Ultrawide mod: the largest picture of its shape
    const auto [uw, uh] = ultrawide::LargestInside(w, h, aspect);
    return {std::clamp(uw, 320, 10240), std::clamp(uh, 180, 5760)};
  }
  // The largest 16:9 inside w x h; rounding never goes past w or h (h * 16 / 9 <= w there, and the
  // other way round).
  int ow = w, oh = h;
  if (int64_t(w) * 9 >= int64_t(h) * 16) ow = int(std::lround(double(h) * 16.0 / 9.0));
  else oh = int(std::lround(double(w) * 9.0 / 16.0));
  // The native renderer's render scale limits (render_scale.h): 0.25 to 8 times the game's 1280x720.
  return {std::clamp(ow, 320, 10240), std::clamp(oh, 180, 5760)};
}

std::pair<int, int> UpscalerOutputFor(int screen_w, int screen_h, double aspect) {
  if (REXCVAR_GET(kk_render_quality) == "custom")
    return UpscalerOutputSize(
        screen_w, screen_h,
        int(std::lround(1280 * DrawScaleAxis("draw_resolution_scale_x") * ultrawide::WidthFactor(aspect))),
        720 * DrawScaleAxis("draw_resolution_scale_y"), aspect);
  return UpscalerOutputSize(screen_w, screen_h, 0, 0, aspect);
}

std::pair<int, int> NoUpscalerFrameSize(int screen_w, int screen_h, int draw_w, int draw_h, double aspect) {
  const auto [pw, ph] = UpscalerOutputSize(screen_w, screen_h, 0, 0, aspect);
  // The Ultrawide mod: the draw resolution scale's height, as wide as the picture's shape.
  if (const double a = ultrawide::WidthFactor(aspect); a != 1.0) draw_w = int(std::lround(draw_w * a));
  return {std::clamp(std::min(draw_w, pw), 320, 10240), std::clamp(std::min(draw_h, ph), 180, 5760)};
}

namespace {
std::atomic<float> ultrawide_factor{1.0f};
}
float UltrawideFactor() { return ultrawide_factor.load(std::memory_order_relaxed); }

namespace {
// Without a temporal upscaler: the frame the native renderer draws (native_frame_size, read by its
// native_render_scale=auto), the draw resolution scale's size but never above the chosen resolution.
void SetNoUpscalerFrame(int output_width, int output_height, int scale_x, int scale_y, double aspect) {
  const auto [fw, fh] = NoUpscalerFrameSize(output_width, output_height, 1280 * scale_x, 720 * scale_y, aspect);
  SetCvarDefault("native_frame_size", std::to_string(fw) + "x" + std::to_string(fh));
  REXLOG_INFO("KK: no upscaler: frame {}x{} (draw resolution scale {}x{}: {}x{}; the chosen resolution {}x{}{})", fw,
              fh, scale_x, scale_y, 1280 * scale_x, 720 * scale_y, output_width, output_height,
              fw < 1280 * scale_x || fh < 720 * scale_y ? ", never above it" : "");
}
}  // namespace

void ApplyRenderPreset(int output_width, int output_height) {
  // A temporal upscaler (the native renderer's native_upscaler, Display > Upscaling) rebuilds the picture
  // at the resolution chosen in the launcher (never above it: DECISIONS 16 Oct) and draws fewer pixels
  // itself; the renderer takes that size from native_upscaler_output (set as a default: not saved).
  std::string quality = REXCVAR_GET(kk_render_quality);
  const bool temporal = TemporalUpscalerOn();
  // The Ultrawide mod (kk_aspect, ultrawide_logic.h): a wider picture, the presenter shown its shape
  // (native_display_aspect, not saved) and the game's 3D views widened (ultrawide.cpp). 16:9: nothing changes.
  const double aspect = ultrawide::AspectFor(REXCVAR_GET(kk_aspect), output_width, output_height);
  const double factor = ultrawide::WidthFactor(aspect);
  ultrawide_factor.store(float(factor), std::memory_order_relaxed);
  if (factor != 1.0) {
    const auto [pw, ph] = UpscalerOutputSize(output_width, output_height, 0, 0, aspect);
    SetCvarDefault("native_display_aspect", std::to_string(pw) + "x" + std::to_string(ph));
    REXLOG_INFO("KK: ultrawide (experimental): {} at {}x{} -> a {}x{} picture, views {:.3f} times wider",
                REXCVAR_GET(kk_aspect), output_width, output_height, pw, ph, factor);
  } else {
    SetCvarDefault("native_display_aspect", "");
  }
  if (temporal) {
    const auto [ow, oh] = UpscalerOutputFor(output_width, output_height, aspect);
    SetCvarDefault("native_upscaler_output", std::to_string(ow) + "x" + std::to_string(oh));
    REXLOG_INFO("KK: upscaler output {}x{} (the chosen resolution {}x{}{})", ow, oh, output_width, output_height,
                quality == "custom" ? ", at most the custom render scale" : "");
    SetCvarDefault("native_frame_size", "");
  } else {
    SetCvarDefault("native_upscaler_output", "");
  }
  if (quality == "custom") {  // retain explicitly supplied render scales (without an upscaler: at most the screen)
    if (!temporal)
      SetNoUpscalerFrame(output_width, output_height, DrawScaleAxis("draw_resolution_scale_x"),
                         DrawScaleAxis("draw_resolution_scale_y"), aspect);
    return;
  }
  // With an upscaler the draw resolution scale no longer sets the native renderer's output (above); it is
  // kept at the screen's size in 720p steps for the logs and for an upscaler turned off live.
  if (temporal) quality = "native";
  const int scale = RenderScaleFor(quality, output_height);
  if (scale <= 0) {  // unknown preset: the player's resolution_scale is used as is (at most the screen)
    if (!temporal)
      SetNoUpscalerFrame(output_width, output_height, DrawScaleAxis("draw_resolution_scale_x"),
                         DrawScaleAxis("draw_resolution_scale_y"), aspect);
    return;
  }
  // The runtime only honours resolution_scale when it differs from its default,
  // so a preset drives the per-axis scales instead (read directly). They are set
  // as defaults so the derived value is not written to the config, and any
  // resolution_scale left over from an earlier custom choice is cleared.
  rex::cvar::ResetToDefault("resolution_scale");
  for (const char* axis : {"draw_resolution_scale_x", "draw_resolution_scale_y"}) {
    SetCvarDefault(axis, std::to_string(scale));
    rex::cvar::ResetToDefault(axis);
  }
  REXLOG_INFO("KK: render preset {} at {}p output -> {}x ({}p)", quality,
              output_height, scale, scale * 720);
  if (!temporal) SetNoUpscalerFrame(output_width, output_height, scale, scale, aspect);
}

namespace {
std::mutex temporary_mutex;
struct TemporarySetting {
  std::string saved, trial;
  rex::cvar::Source source;
};
std::map<std::string, TemporarySetting, std::less<>> temporary_baselines;
}
void SetTemporarySettingBaseline(const std::string& name, const std::string& value) {
  std::lock_guard lock(temporary_mutex);
  auto it = temporary_baselines.find(name);
  if (it == temporary_baselines.end() || it->second.saved != value)
    temporary_baselines[name] = TemporarySetting{value, value, rex::cvar::GetFlagSource(name)};
}
void SetTemporarySettingTrialValue(const std::string& name, const std::string& value) {
  std::lock_guard lock(temporary_mutex);
  if (auto it = temporary_baselines.find(name); it != temporary_baselines.end()) it->second.trial = value;
}
void ClearTemporarySettingBaselines() {
  std::lock_guard lock(temporary_mutex);
  temporary_baselines.clear();
}
namespace {
std::map<std::string, mods::OverlayEntry, std::less<>> mod_overlay;  // under temporary_mutex
}
bool SetModSetting(const std::string& name, const std::string& value) {
  std::lock_guard lock(temporary_mutex);
  auto it = mod_overlay.find(name);
  const std::string saved = it != mod_overlay.end() ? it->second.saved : rex::cvar::GetFlagByName(name);
  const int source = it != mod_overlay.end() ? it->second.saved_source : int(rex::cvar::GetFlagSource(name));
  if (!rex::cvar::SetFlagByName(name, value)) return false;
  mod_overlay[name] = mods::OverlayEntry{saved, source, rex::cvar::GetFlagByName(name)};
  return true;
}
void ClearModSettings() {
  std::lock_guard lock(temporary_mutex);
  mod_overlay.clear();
}
namespace {
std::atomic<bool> g_audio_mute_held{false};  // task 293 (SetAudioMuteHeld)
}  // namespace
void SetAudioMuteHeld(bool held) { g_audio_mute_held = held; }

bool SaveSettings(const std::filesystem::path& path, bool include_temporary) {
  std::lock_guard lock(temporary_mutex);
  // Like rex::cvar::SaveConfig, but values that came from the command line or
  // environment (e.g. --game_data_root, --log_file) are one-off overrides and
  // are not written back.
  std::string out = "# King Kong settings (edited by the launcher)\n";
  for (const auto& e : rex::cvar::GetRegistry()) {
    if (e.type == rex::cvar::FlagType::Command || e.is_debug_only) continue;
    const auto baseline = temporary_baselines.find(e.name);
    const std::string live = e.getter();
    const bool protect = !include_temporary && baseline != temporary_baselines.end() && live == baseline->second.trial;
    auto source = protect ? baseline->second.source : e.source;
    if (source == rex::cvar::Source::kCommandLine || source == rex::cvar::Source::kEnvironment) continue;
    // Set by the port at every start (ApplyRuntimeOverrides).
    if (e.name == "video_mode_width" || e.name == "video_mode_height") continue;
    // Task 293: held on by a PS2 cutscene the level loads under, never the player's choice.
    if (e.name == "audio_mute" && g_audio_mute_held.load()) continue;
    // Set from kk_hdr and kk_hdr_levels at every start (hdr_display.cpp, HDR follows Windows).
    if (e.name == "present_hdr" || ((e.name == "present_hdr_peak" || e.name == "present_hdr_paper_white") &&
                                    REXCVAR_GET(kk_hdr_levels) != "manual"))
      continue;
    std::string value = protect ? baseline->second.saved : live;
    // A settings mod's value still in place: the player's own is saved (Kong mods, mods_logic.h).
    if (const auto mod = mod_overlay.find(e.name); mod != mod_overlay.end() && value == mod->second.applied) {
      value = mod->second.saved;
      source = rex::cvar::Source(mod->second.saved_source);
    }
    const bool hardware_setting = e.name == "kk_render_quality" || e.name == "native_upscaler_quality" ||
        e.name == "native_ao_quality" || e.name == "native_ao_samples" || e.name == "ao_mode" || e.name == "native_upscaler" ||
        e.name == "present_effect" || e.name == "resolution_scale" || e.name == "draw_resolution_scale_x" ||
        e.name == "draw_resolution_scale_y" || e.name == "native_gi" || e.name == "native_gi_distance";
    if (value == e.default_value && !(hardware_setting && source != rex::cvar::Source::kDefault)) continue;
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

void MigrateFrameCounter() {
  // Beta 9: Show FPS (kk_show_fps) became Performance metrics; a frame counter that was on is "FPS only",
  // the same frame rate and frame time it showed. The old value then goes back to its default, so the
  // settings file drops it at the next save.
  if (!REXCVAR_GET(kk_show_fps)) return;
  if (rex::cvar::GetFlagSource("kk_performance_metrics") == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagByName("kk_performance_metrics", "fps");
  rex::cvar::SetFlagByName("kk_show_fps", "false");
  REXLOG_INFO("KK: the frame counter setting moved to Performance metrics ({})",
              rex::cvar::GetFlagByName("kk_performance_metrics"));
}

void MigrateHdrSettings() {
  // Beta 10: HDR follows Windows. A settings file from before has present_hdr only when the player turned HDR on
  // (On stays On), and Peak brightness or Paper white only when the player moved them (Manual keeps them). Newer
  // files never save present_hdr, and save the two levels only in Manual, so this changes nothing after that.
  using rex::cvar::GetFlagSource, rex::cvar::Source;
  const bool hdr_saved = GetFlagSource("present_hdr") == Source::kConfig;
  if (hdr_saved && GetFlagSource("kk_hdr") == Source::kDefault && rex::cvar::GetFlagByName("present_hdr") == "true")
    rex::cvar::SetFlagByName("kk_hdr", "on");
  bool levels_saved = false;
  for (const char* name : {"present_hdr_peak", "present_hdr_paper_white"})
    levels_saved |= GetFlagSource(name) == Source::kConfig;
  if (levels_saved && GetFlagSource("kk_hdr_levels") == Source::kDefault)
    rex::cvar::SetFlagByName("kk_hdr_levels", "manual");
  if (hdr_saved || levels_saved)
    REXLOG_INFO("KK: HDR settings from an older version: kk_hdr={} kk_hdr_levels={}",
                rex::cvar::GetFlagByName("kk_hdr"), rex::cvar::GetFlagByName("kk_hdr_levels"));
}

void MigrateSettings(const std::filesystem::path& config_path) {
  // 2: FSR 1 at Quality became the default (it was no upscaler at Native).
  // A settings file only keeps values that differ from the defaults, so an
  // older one that has no upscaler setting meant the old default: keep it, so
  // an update doesn't change a player's picture (or turn their High, Ultra or
  // Steam Deck preset into Custom). New installs get the new defaults.
  // 3: GI follows the preset. Saved GI values, including an explicit Off, win.
  constexpr int32_t kVersion = 3;
  if (REXCVAR_GET(kk_settings_version) >= kVersion) return;
  std::error_code ec;
  if (std::filesystem::exists(config_path, ec)) {
    // A file this version wrote already has the version in it: if it still
    // reads as older, the file couldn't be read, and saving now would put the
    // defaults over every setting in it (issue #37). Keep a copy and leave it.
    std::ifstream file(config_path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file || (text.find("kk_settings_version") != std::string::npos &&
                  rex::cvar::GetFlagSource("kk_settings_version") == rex::cvar::Source::kDefault)) {
      auto backup = config_path;
      backup += ".bak";
      std::filesystem::copy_file(config_path, backup, std::filesystem::copy_options::overwrite_existing, ec);
      REXLOG_ERROR("KK: the settings in {} couldn't be read; left as they are (copy in {})",
                   config_path.filename().string(), backup.filename().string());
      return;
    }
    static constexpr std::pair<const char*, const char*> kOldDefaults[] = {{"present_effect", "bilinear"},
                                                                           {"kk_render_quality", "native"}};
    if (REXCVAR_GET(kk_settings_version) < 2) {
      for (const auto& [name, value] : kOldDefaults)
        for (const auto& e : rex::cvar::GetRegistry())
          if (e.name == name && e.source == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
      REXLOG_INFO("KK: settings from an older version: kept its upscaler settings ({}, {})",
                  rex::cvar::GetFlagByName("present_effect"), rex::cvar::GetFlagByName("kk_render_quality"));
    }
    auto preset_gi = [](auto get) {
      for (const auto& p : kGraphicsPresets)
        if (MatchesGraphicsPresetSettings(p, get, false)) return p.gi;
      // Custom settings retain their hardware recommendation, or Medium when
      // they predate hardware detection. Do not change any other graphics value.
      for (const auto& t : kHardwareTiers)
        if (REXCVAR_GET(kk_hardware_tier) == t.id) return t.gi;
      return "medium";
    };
    if (REXCVAR_GET(kk_original_look)) {
      // The live Off belongs to Original. Migrate the saved Modern settings
      // instead, so switching back restores GI with the rest of that look.
      std::string modern = REXCVAR_GET(kk_modern_settings);
      if (!modern.empty()) {
        std::map<std::string, std::string> values;
        for (size_t pos = 0, end; pos < modern.size(); pos = end + 1) {
          end = modern.find(';', pos);
          if (end == std::string::npos) end = modern.size();
          const auto eq = modern.find('=', pos);
          if (eq < end) values[modern.substr(pos, eq - pos)] = modern.substr(eq + 1, end - eq - 1);
        }
        if (!values.count("native_gi")) {
          values["native_gi"] = preset_gi([&](const char* name) {
            const auto it = values.find(name);
            return it != values.end() ? it->second : rex::cvar::GetFlagByName(name);
          });
          modern.clear();
          for (const auto& [name, value] : values) modern += name + "=" + value + ";";
          rex::cvar::SetFlagByName("kk_modern_settings", modern);
        }
      }
    } else if (rex::cvar::GetFlagSource("native_gi") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("native_gi", preset_gi(rex::cvar::GetFlagByName));
    }
    REXLOG_INFO("KK: GI settings migrated to version 3: {} (Original look {})",
                rex::cvar::GetFlagByName("native_gi"), REXCVAR_GET(kk_original_look));
    rex::cvar::SetFlagByName("kk_settings_version", std::to_string(kVersion));
    SaveSettings(config_path);
    return;
  }
  // A new install: saved with the rest later (writing the file now would look
  // like an earlier run to the launcher's What's new).
  rex::cvar::SetFlagByName("kk_settings_version", std::to_string(kVersion));
}

namespace {

// FSR 1 where the presenter has it (NIS is a choice only, since Beta 3).
std::string SpatialDefault() {
  for (const auto& e : rex::cvar::GetRegistry()) {
    if (e.name != "present_effect") continue;
    const auto& allowed = e.constraints.allowed_values;
    return std::find(allowed.begin(), allowed.end(), "fsr") != allowed.end() ? "fsr" : "bilinear";
  }
  return "bilinear";
}

const rex::cvar::FlagEntry* Flag(std::string_view name) {
  for (const auto& e : rex::cvar::GetRegistry())
    if (e.name == name) return &e;
  return nullptr;
}

// The best temporal upscaler for this card among those offered ("off": none).
std::string BestTemporalUpscaler() {
  const GpuInfo& gpu = Gpu();
  const std::string offered = REXCVAR_GET(kk_upscalers_offered);
  if (offered.empty()) {
#if defined(_WIN32)
    // Not asked yet: by the vendor. FSR 4 waits for the answer (it needs Direct3D 12 and a recent card).
    return RecommendedUpscaler(gpu.vendor, {}, false);
#else
    return "off";  // the temporal upscalers are Windows only for now
#endif
  }
  auto has = [&](const char* v) { return ("," + offered + ",").find("," + std::string(v) + ",") != std::string::npos; };
  return RecommendedUpscaler(gpu.vendor, {true, has("dlss"), has("fsr3"), has("fsr4"), has("xess")},
                            rex::cvar::GetFlagByName("native_backend") == "d3d12");
}

bool NativeRendererInUse() { return rex::cvar::GetFlagByName("gpu_plugin") == "native"; }

}  // namespace

bool NormalizeRendererSettings() {
  const std::string renderer = rex::cvar::GetFlagByName("kk_renderer");
  const std::string plugin = rex::cvar::GetFlagByName("gpu_plugin");
  const std::string api = rex::cvar::GetFlagByName("native_backend");
  const bool legacy = renderer != "native" || plugin == "xenos" || plugin == "classic" ||
                      (api != "auto" && api != "vulkan"
#if !defined(__linux__)
                       && api != "d3d12"
#endif
                      );
  rex::cvar::SetFlagByName("kk_renderer", "native");
  rex::cvar::SetFlagByName("gpu_plugin", "native");
  if (legacy) rex::cvar::SetFlagByName("native_backend", "auto");
  return legacy;
}

std::string DefaultTemporalUpscaler() {
  if (!Flag("native_upscaler") || !NativeRendererInUse()) return "off";
  return BestTemporalUpscaler();
}

std::string DefaultUpscaler() { return DefaultTemporalUpscaler() != "off" ? "bilinear" : SpatialDefault(); }

void ApplyUpscalerDefaults() {
  const std::string temporal = DefaultTemporalUpscaler();
  SetCvarDefault("native_upscaler", temporal);
  SetCvarDefault("present_effect", temporal != "off" ? "bilinear" : SpatialDefault());
}

void ApplyHardwareRecommendation(bool detect, int width, int height) {
#if !defined(_WIN32)
  // Only GI follows detection on Linux in this update; the existing resolution
  // and upscaler recommendation behaviour is unchanged.
  if (detect) {
    const auto& tier = kHardwareTiers[HardwareTierIndex(DetectHardware())];
    rex::cvar::SetFlagByName("kk_hardware_tier", tier.id);
  }
  if (!REXCVAR_GET(kk_original_look)) {
    for (const auto& tier : kHardwareTiers)
      if (REXCVAR_GET(kk_hardware_tier) == tier.id) SetCvarDefault("native_gi", tier.gi);
  }
  return;
#endif
  if (detect) {
    const HardwareProfile h = DetectHardware();
    const auto& tier = kHardwareTiers[HardwareTierIndex(h)];
    rex::cvar::SetFlagByName("kk_hardware_tier", tier.id);
    REXLOG_INFO("KK: hardware recommendation {}: GPU {} ({} MiB), {} physical cores, {} MiB RAM, display {}x{}@{}",
                tier.id, h.gpu.name, h.gpu.dedicated_memory >> 20, h.physical_cores,
                h.system_memory >> 20, h.width, h.height, h.refresh_hz);
  }
  const std::string selected = REXCVAR_GET(kk_hardware_tier);
  const auto tier = std::find_if(kHardwareTiers.begin(), kHardwareTiers.end(),
                               [&](const auto& t) { return selected == t.id; });
  if (tier == kHardwareTiers.end()) return;
  auto explicit_setting = [](const char* name) {
    const auto* f = Flag(name);
    return f && f->source != rex::cvar::Source::kDefault;
  };
  const bool original = REXCVAR_GET(kk_original_look);
  const bool custom = explicit_setting("resolution_scale") || explicit_setting("draw_resolution_scale_x") ||
                      explicit_setting("draw_resolution_scale_y");
  if (custom && !original && !explicit_setting("kk_render_quality")) SetCvarDefault("kk_render_quality", "custom");
  const char* quality = RecommendedRenderQuality(*tier, width, height);
  for (const char* name : {"kk_render_quality", "native_upscaler_quality"}) {
    if (!PreserveHardwareSetting(explicit_setting(name), custom, original)) SetCvarDefault(name, quality);
  }
  if (!original) {
    SetCvarDefault("native_gi", tier->gi);
    SetCvarDefault("native_gi_distance", std::string_view(tier->gi) == "low" ? "near" :
                   std::string_view(tier->gi) == "high" ? "far" : "medium");
    SetCvarDefault("native_ao_quality", tier->ao_quality);
    SetCvarDefault("native_ao_samples", std::to_string(tier->ao_samples));
    SetCvarDefault("ao_mode", tier->ao_enabled ? "1" : "0");
    ApplyUpscalerDefaults(); // asynchronous support query refines this using the existing fallback policy
  }
  REXLOG_INFO("KK: recommendation {} at {}x{}: render {}, AO {} / {} samples; explicit settings retained",
              selected, width, height, quality, tier->ao_quality, tier->ao_samples);
}

void InitializeHardwareRecommendation(const std::filesystem::path& config_path, int width, int height) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(config_path, ec);
  // A pre-existing config, including one predating this feature, never gets a new tier automatically.
  ApplyHardwareRecommendation(!exists && !ec && REXCVAR_GET(kk_hardware_tier).empty(), width, height);
}

void SetOfferedUpscalers(const std::string& offered) {
  if (offered != REXCVAR_GET(kk_upscalers_offered)) {
    REXLOG_INFO("KK: temporal upscalers this PC runs: {}", offered);
    rex::cvar::SetFlagByName("kk_upscalers_offered", offered);
  }
  ApplyUpscalerDefaults();
}

void ApplyVSyncSettings() {
  // The launcher's VSync setting is shared by both presenters. Derive Vulkan's
  // allowed modes as defaults so they are never saved as independent settings.
  // With VSync on, FIFO is the only allowed mode; off keeps the SDK's choices.
  const std::string tearing = rex::cvar::GetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing");
  for (const char* name : {"vulkan_allow_present_mode_immediate", "vulkan_allow_present_mode_mailbox",
                         "vulkan_allow_present_mode_fifo_relaxed"}) {
    const auto source = rex::cvar::GetFlagSource(name);
    SetCvarDefault(name, tearing);
    // Allow an explicit off-mode choice (e.g. mailbox) for diagnostics. VSync
    // on still requires FIFO, even when an old CLI override enabled tearing.
    if (tearing != "true" || (source != rex::cvar::Source::kCommandLine && source != rex::cvar::Source::kEnvironment))
      rex::cvar::ResetToDefault(name);
  }
  REXLOG_INFO("KK: VSync {} (D3D12 tearing {}; Vulkan immediate {} mailbox {} fifo_relaxed {})",
              tearing == "true" ? "off" : "on", tearing,
              rex::cvar::GetFlagByName("vulkan_allow_present_mode_immediate"),
              rex::cvar::GetFlagByName("vulkan_allow_present_mode_mailbox"),
              rex::cvar::GetFlagByName("vulkan_allow_present_mode_fifo_relaxed"));
}

void ApplyFixedSettings() {
  // Beta 3's default is a temporal upscaler where one runs (ApplyUpscalerDefaults). A settings file from
  // before that chose an upscaler on the Upscaler row (present_effect is in it only when the player
  // picked one: FSR 1, NIS or Off) keeps that choice: no temporal upscaler on top.
  if (const auto* present = Flag("present_effect"), *temporal = Flag("native_upscaler");
      present && temporal && present->source == rex::cvar::Source::kConfig &&
      temporal->source == rex::cvar::Source::kDefault) {
    REXLOG_INFO("KK: present_effect {} chosen in the settings file: no temporal upscaler by default",
                rex::cvar::GetFlagByName("present_effect"));
    rex::cvar::SetFlagByName("native_upscaler", "off");
  }
  {
    const GpuInfo& gpu = Gpu();
    REXLOG_INFO("KK: graphics card: {} ({}); default upscaler {} / {} (offered: {})",
                gpu.name.empty() ? "not found" : gpu.name,
                gpu.vendor == GpuVendor::kNvidia ? "NVIDIA"
                : gpu.vendor == GpuVendor::kAmd  ? "AMD"
                : gpu.vendor == GpuVendor::kIntel ? "Intel"
                                                  : "unknown",
                BestTemporalUpscaler(), SpatialDefault(),
                REXCVAR_GET(kk_upscalers_offered).empty() ? "not asked yet" : REXCVAR_GET(kk_upscalers_offered));
  }
  // FXAA Extreme is no longer offered: a settings file that chose it gets FXAA.
  if (rex::cvar::GetFlagByName("swap_post_effect") == "fxaa_extreme")
    rex::cvar::SetFlagByName("swap_post_effect", "fxaa");
  // Shader preparing is always Balanced (no longer a setting): Wait paused the
  // game for up to 2 seconds the first time an effect appeared, and
  // Background made objects vanish or flash. A settings file from before keeps
  // no say in it (the values equal the defaults, so they aren't saved again).
  rex::cvar::SetFlagByName("async_shader_compilation", "true");
  rex::cvar::ResetToDefault("async_shader_wait_ms");
  // The mouse turns the camera itself (mouse_look.cpp), so the runtime's
  // mouse-to-right-stick part is kept at its lowest, where it never gets past
  // the camera's deadzone (kk_mouse_sensitivity is the setting now).
  rex::cvar::SetFlagByName("mnk_sensitivity", "0.010000");
  // A beta follows the beta channel unless a channel was chosen: one unzipped by hand, rather than
  // switched to with Change version, would otherwise look for stable versions only and never be
  // offered the next beta (Beta 1 copies installed by hand). Set, not made the default, so the
  // settings file keeps it.
  if (std::string_view(KK_VERSION).find("-beta") != std::string_view::npos)
    for (const auto& e : rex::cvar::GetRegistry())
      if (e.name == "kk_update_channel" && e.source == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName("kk_update_channel", "beta");
        REXLOG_INFO("KK: a beta with no update channel chosen: on the beta channel");
        break;
      }
  // Supersampling is no longer offered (Beta 2: Display > Resolution picks the size the game is
  // shown at): a settings file that chose it gets Native.
  if (rex::cvar::GetFlagByName("kk_render_quality") == "supersample")
    rex::cvar::SetFlagByName("kk_render_quality", "native");
  // The ambient occlusion method is no longer a setting (Beta 2: always GTAO, ApplyPortDefaults):
  // Beta 1's AO method choice in a settings file goes.
  for (const char* name : {"native_ao_method", "native_ao_apply"})
    for (const auto& e : rex::cvar::GetRegistry())
      if (e.name == name && e.source == rex::cvar::Source::kConfig) {
        REXLOG_INFO("KK: {} {} from the settings file: back to the default", name, rex::cvar::GetFlagByName(name));
        rex::cvar::ResetToDefault(name);
        break;
      }
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
  SetCvarDefault("mnk_sensitivity", "0.010000");  // as the setting writes it, so it is not saved
  // Keyboard & mouse: laid out like other first-person games. The mouse turns
  // the camera (mouse_look.cpp; the runtime's own mouse-to-stick part is off,
  // see ApplyFixedSettings), the buttons fire and aim, and the keys follow
  // what each button does in the game (Jack / Kong, from the Xbox 360 manual).
  SetCvarDefault("mnk_mouse", "true");
  static constexpr std::pair<const char*, const char*> kKeys[] = {
      {"keybind_right_trigger", "LMB,E"},         // shoot, take, use, repel
      {"keybind_left_trigger", "RMB"},            // take aim
      {"keybind_right_shoulder", "R"},            // reload
      {"keybind_left_shoulder", "1"},             // (unused)
      {"keybind_lstick_press", "C,Control"},      // crouch
      {"keybind_rstick_press", "MMB,Z"},          // zoom
      {"keybind_a", "Return,F"},                  // select; call companion / Kong: hit
      {"keybind_b", "Space,Backspace"},           // back; check reserve bullets / Kong: jump, dodge, climb
      {"keybind_x", "Q"},                         // Kong: grab, throw, take Ann
      {"keybind_y", "G"},                         // drop spear / Kong: push back, bite, fury
      {"keybind_start", "Escape,P"},              // menu
      {"keybind_back", "Tab"},
      {"keybind_lstick_up", "W"},  {"keybind_lstick_down", "S"},
      {"keybind_lstick_left", "A"}, {"keybind_lstick_right", "D"},
      {"keybind_rstick_up", "I"},  {"keybind_rstick_down", "K"},  // the camera without the mouse
      {"keybind_rstick_left", "J"}, {"keybind_rstick_right", "L"},
      {"keybind_dpad_up", "Up"},   {"keybind_dpad_down", "Down"},  // menus
      {"keybind_dpad_left", "Left"}, {"keybind_dpad_right", "Right"},
  };
  for (const auto& [name, keys] : kKeys) SetCvarDefault(name, keys);
  // VSync off (the frame rate cap paces the game), FXAA, 4x texture filtering.
  SetCvarDefault("d3d12_allow_variable_refresh_rate_and_tearing", "true");
  SetCvarDefault("swap_post_effect", "fxaa");
  SetCvarDefault("anisotropic_override", "3");
  SetCvarDefault("native_gi", "medium");  // hardware recommendation refines this; explicit choices win
  // An upscaler at Quality (kk_render_quality): FSR 1 until the settings are read and the renderer is
  // known (ApplyUpscalerDefaults then picks the temporal one where it runs). The player's own choice in
  // the settings file still wins.
  SetCvarDefault("present_effect", SpatialDefault());
  // Ambient occlusion on the native renderer: GTAO at the end of the opaque pass, so water, smoke,
  // fire, fur and the sky are never darkened (native-renderer/docs/ao.md). The plugin's own defaults
  // stay the legacy method for standalone renderer tests.
  SetCvarDefault("native_ao_method", "gtao");
  SetCvarDefault("native_ao_apply", "opaque");
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
      {"native_gi", "off"}, {"native_ao_normal_smoothing", "0"},
      {"native_cel_shading", "false"},  // a screen effect the console never had (docs/cel.md)
      {"native_effect_buffers", "false"},
      {"native_sharpness", "off"},
      {"native_upscaler", "off"},  // the native renderer's temporal upscalers (Display > Upscaling)
      // The native renderer's lighting options (native-renderer/docs/lighting.md): the game's own.
      {"native_shadow_quality", "original"}, {"native_water_reflections", "original"}, {"native_volumetrics", "off"},
  };
  return settings;
}

void EnforceOriginalLook() {
  if (!REXCVAR_GET(kk_original_look)) return;
  const std::string keep = REXCVAR_GET(kk_original_look_keep);
  for (const auto& [name, value] : OriginalLookSettings()) {
    // kk_original_look_keep: comma-separated settings the Original look leaves as they are (testing:
    // the golden-frame harness turns AO or the frame-rate cap on over the Original look).
    if (!keep.empty() && ("," + keep + ",").find("," + std::string(name) + ",") != std::string::npos) continue;
    rex::cvar::SetFlagByName(name, value);
  }
}

void ApplyVideoMode() {
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
  // It also runs before the GPU starts (king_kong_app.h, with ApplyRenderPreset): the native renderer
  // sizes its frame from the same mode, and a window size there (1920x1080, or 768x432 at 250%) gave it a
  // frame the game never draws (the picture in a corner, no scene found: task 167).
}

void ApplyRuntimeOverrides() {
  rex::cvar::SetFlagByName("vsync", "false");
  REXLOG_INFO("KK: frame-rate cap {}", REXCVAR_GET(kk_frame_rate));
  REXLOG_INFO("KK: global illumination {}", rex::cvar::GetFlagByName("native_gi"));

  ApplyVideoMode();

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
