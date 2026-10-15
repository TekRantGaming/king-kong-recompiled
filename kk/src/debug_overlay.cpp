// The graphics panel (F8): a translucent panel over the running game for trying graphics settings while
// playing. Changes apply at once and are temporary: "Save as my settings" writes them to the settings file
// the launcher uses, "Reset to my settings" puts the saved ones back, and every other save of the port
// (the launcher, the pause menu) keeps writing the saved values while a trial is on (settings.h,
// SetTemporarySettingBaseline).
//
// Keys, in play only (the launcher, the pause menu and the black start screen keep theirs):
//   F8          the panel on and off (the mouse goes to the panel while it is open; other keys reach the game)
//   F7 (held)   the saved settings while held, the trial ones again when let go: an A/B comparison
//   F6          a capture of the game's picture (PNG, without the panel) and a text file of the settings, in
//               <user data>/screenshots
// Controller: both stick clicks toggle the panel; D-pad/left stick navigate,
// left/right change, A activates, B closes, LB compares while held, RB captures.
// All controller input is consumed while open, then until neutral on closing.
// The SDK binds F7 to its achievements list: in play the panel takes F7; elsewhere the SDK still has it.
//
// Every setting is a cvar the launcher also writes (the native renderer reads its live ones once a frame,
// native-renderer/backend/effect_settings.h). Those that only apply at the next start are shown with a
// "needs restart" mark and cannot be changed here.

#include "debug_overlay.h"
#include "graphics_panel_style.h"
#include "debug_overlay_logic.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#if defined(KK_DEV_TOOLS)
#include <rex/ui/keybinds.h>
#endif
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "art.h"
#include "diagnostics.h"
#include "frame_stats.h"
#include "launcher_ui.h"
#include "launcher_text.h"
#include "ui_paint.h"
#include "play_internal.h"
#include "settings.h"
#if defined(KK_DEV_TOOLS)
namespace kk {
double DevSecondsSinceMenu();
}
#endif
#include "hooks/kk_native_ui.h"

REXCVAR_DEFINE_DOUBLE(kk_graphics_panel_opacity, 0.82, "KK/Graphics panel",
                      "The graphics panel's (F8) background opacity, 0.3 to 1");
REXCVAR_DEFINE_BOOL(kk_graphics_panel_compact, false, "KK/Graphics panel",
                    "The graphics panel (F8) shows only the frame rate and the A/B hint");

namespace kk {
namespace {
using text::T;
using text::F;

std::mutex g_pad_mutex;
panel::PadCapture g_pad[4];
std::chrono::steady_clock::time_point g_pad_at{};
#if defined(KK_DEV_TOOLS)
// The controller harness supplies physical samples at the real game pad read,
// so its assertions cover the wrapper's input suppression as well as the UI.
bool g_controller_test_override = false;
panel::PadState g_controller_test_pad;
int g_controller_test_samples = 0;
bool g_controller_test_consumed = false;
#endif

// ------------------------------------------------------------------ settings ---

enum class Kind { kChoice, kChoiceSlider, kToggle, kSlider, kSliderInt, kUpscaler, kSharpness, kMipBias, kRestart };
// When a row is shown (the upscaler's own rows only with one that uses them).
enum class Show { kAlways, kTemporal, kDlss, kFsr, kAo, kGi, kShadows, kWater, kShafts };

struct Option {
  const char* label;
  const char* value;
};

struct Setting {
  const char* section;
  const char* label;
  const char* cvar;
  Kind kind;
  std::vector<Option> options;  // choices; a toggle's {off, on}
  float min = 0, max = 1;
  const char* format = "%.2f";
  bool native = true;  // the native renderer's effects
  Show show = Show::kAlways;
  const char* help = "";
};

const std::vector<Setting>& Settings() {
  static const std::vector<Setting> settings = {
      // Upscaling.
      {"Upscaling", "Upscaler", "native_upscaler", Kind::kUpscaler, {}, 0, 1, "", true, Show::kAlways,
       "Temporal upscalers rebuild a sharp picture from several frames. Only those this PC runs are listed."},
      {"Upscaling", "Mode", "native_upscaler_quality", Kind::kChoice,
       {{"Native AA", "native"}, {"Quality", "quality"}, {"Balanced", "balanced"}, {"Performance", "performance"},
        {"Ultra Performance", "ultra_performance"}},
       0, 1, "", true, Show::kTemporal, "How many pixels the game draws before the upscaler rebuilds the picture."},
      {"Upscaling", "DLSS model", "native_upscaler_dlss_preset", Kind::kChoice,
       {{"Latest (M, L for Ultra Performance)", "latest"}, {"Driver default", "default"}, {"J", "j"}, {"K", "k"},
        {"L", "l"}, {"M", "m"}},
       0, 1, "", true, Show::kDlss, "The DLSS preset: J and K are DLSS 4's transformer model, L and M DLSS 4.5's."},
      {"Upscaling", "Sharpening", "native_upscaler_sharpness", Kind::kSharpness, {}, 0, 1, "%.2f", true, Show::kFsr,
       "AMD FSR's sharpening after the upscale (DLSS and XeSS have none)."},
      {"Upscaling", "Mip bias", "native_upscaler_mip_bias", Kind::kMipBias, {}, -2, 1, "%.2f", true, Show::kTemporal,
       "Texture sharpness of the scene. Auto is what the upscalers recommend for the mode."},
      {"Upscaling", "Reactive mask", "native_upscaler_reactive", Kind::kSlider, {}, 0, 1, "%.2f", true,
       Show::kTemporal, "How strongly water, smoke, fur and particles favour the current frame (less ghosting)."},
      {"Upscaling", "Render quality", "kk_render_quality", Kind::kRestart, {}, 0, 1, "", false},
      {"Upscaling", "Spatial upscaler", "present_effect", Kind::kRestart, {}, 0, 1, "", false},
      // Image.
      {"Image", "Sharpness", "native_sharpness", Kind::kChoiceSlider,
       {{"Off", "off"}, {"Low", "low"}, {"Medium", "medium"}, {"High", "high"}}, 0, 1, "", true, Show::kAlways,
       "Adaptive sharpening. Skipped when the upscaler's own sharpening is active."},
      {"Image", "Effect resolution", "native_effect_buffers", Kind::kChoice,
       {{"Original", "false"}, {"Output", "true"}}, 0, 1, "", true, Show::kAlways,
       "Resolution of bloom, blur and glow buffers."},
      {"Image", "Anti-aliasing (FXAA)", "swap_post_effect", Kind::kChoice, {{"Off", "none"}, {"FXAA", "fxaa"}}, 0, 1,
       "", true, Show::kAlways, "Off while a temporal upscaler runs (it anti-aliases itself)."},
      {"Image", "Texture filtering", "anisotropic_override", Kind::kChoice,
       {{"Game", "-1"}, {"Off", "0"}, {"2x", "2"}, {"4x", "3"}, {"8x", "4"}, {"16x", "5"}}},
      // Ambient occlusion.
      {"Ambient occlusion", "Ambient occlusion", "ao_mode", Kind::kChoice,
       {{"Off", "0"}, {"On", "1"}, {"AO only (test view)", "2"}}, 0, 1, "", false},
      {"Ambient occlusion", "Strength", "ao_strength", Kind::kChoice,
       {{"Light", "1"}, {"Medium", "2"}, {"Strong", "3"}}, 0, 1, "", false, Show::kAo},
      {"Ambient occlusion", "Quality", "native_ao_samples", Kind::kChoice,
       {{"Low", "1"}, {"Medium", "2"}, {"High", "3"}}, 0, 1, "", true, Show::kAo},
      {"Ambient occlusion", "Resolution", "native_ao_quality", Kind::kChoice,
       {{"Auto", "auto"}, {"Half", "half"}, {"Full", "full"}}, 0, 1, "", true, Show::kAo},
      {"Ambient occlusion", "Noise smoothing", "native_ao_temporal", Kind::kToggle, {{"Off", "false"}, {"On", "true"}},
       0, 1, "", true, Show::kAo, "Blends the shading over frames (grain on hands and weapons)."},
      // Global illumination.
      {"Global illumination", "Global illumination", "native_gi", Kind::kChoice,
       {{"Off", "off"}, {"Low", "low"}, {"Medium", "medium"}, {"High", "high"}}},
      {"Global illumination", "Intensity", "native_gi_intensity", Kind::kSlider, {}, 0, 4, "%.2f", true, Show::kGi},
      {"Global illumination", "GI distance", "native_gi_distance", Kind::kChoice,
       {{"Near", "near"}, {"Medium", "medium"}, {"Far", "far"}, {"Full", "full"}}, 0, 1, "", true, Show::kGi,
       "Limits bounced light to nearby surfaces to save GPU time. Near: 16 m, Medium: 32 m, Far: 64 m. Full has no limit. Light fades smoothly over the final quarter of the distance."},
      {"Global illumination", "Resolution", "native_gi_quality", Kind::kChoice,
       {{"Auto", "auto"}, {"Half", "half"}, {"Full", "full"}}, 0, 1, "", true, Show::kGi},
      // Lighting.
      {"Tessellation", "Tessellation", "native_tessellation", Kind::kToggle, {{"Off", "false"}, {"On", "true"}},
       0, 1, "", true, Show::kAlways, "Ann only. Parts whose depth shaders have no normals retain their original geometry."},
      {"Tessellation", "Smoothness", "native_tessellation_smoothness", Kind::kSlider, {}, 0, 1, "%.2f"},
      {"Tessellation", "Detail", "native_tessellation_detail", Kind::kSliderInt, {}, 1, 16, "%d"},
      {"Lighting", "Shadows", "native_shadow_quality", Kind::kChoice,
       {{"Original", "original"}, {"High", "high"}, {"Ultra", "ultra"}}},
      {"Lighting", "Shadow softness", "native_shadow_softness", Kind::kSlider, {}, 0.25f, 4, "%.2f", true, Show::kShadows},
      {"Lighting", "Water reflections", "native_water_reflections", Kind::kChoice,
       {{"Original", "original"}, {"Screen space", "ssr"}}},
      {"Lighting", "Reflection strength", "native_ssr_strength", Kind::kSlider, {}, 0, 1, "%.2f", true, Show::kWater},
      {"Lighting", "Light shafts", "native_volumetrics", Kind::kChoice,
       {{"Original", "off"}, {"Volumetric low", "low"}, {"Volumetric high", "high"}}},
      {"Lighting", "Light shaft strength", "native_volumetric_strength", Kind::kSlider, {}, 0, 4, "%.2f", true,
       Show::kShafts},
      // The game's own effects.
      {"Effects", "Motion blur", "kk_motion_blur", Kind::kToggle, {{"Off", "false"}, {"On", "true"}}, 0, 1, "", false},
      {"Effects", "Screen blur", "kk_big_blur", Kind::kToggle, {{"Off", "false"}, {"On", "true"}}, 0, 1, "", false},
      {"Effects", "Distance fog", "kk_fog", Kind::kToggle, {{"Off", "false"}, {"On", "true"}}, 0, 1, "", false},
      {"Effects", "Field of view", "kk_fov", Kind::kSliderInt, {}, 69, 110, "%d", false},
      // Only at the next start.
      {"Needs restart", "Look", "kk_original_look", Kind::kRestart, {}, 0, 1, "", false},
      {"Needs restart", "Graphics API", "native_backend", Kind::kRestart, {}, 0, 1, "", true},
  };
  return settings;
}

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
bool Exists(const char* name) { return rex::cvar::GetFlagInfo(name) != nullptr; }
bool NativeRenderer() { return Get("gpu_plugin") == "native"; }

using panel::UpscalerChoice;
using panel::kUpscalers;

NrEffectStats Stats() { return nr::kk::EffectTimes(); }

// The temporal upscalers this PC runs: the launcher's answer (it asked the SDKs), else by the GPU's vendor.
std::vector<UpscalerChoice> OfferedUpscalers() {
  std::vector<UpscalerChoice> out;
  const NrEffectStats s = Stats();
  const nr::kk::UpscalerChoices c = nr::kk::PeekUpscalers();
#if !defined(__linux__)
  constexpr bool windows = true;
#else
  constexpr bool windows = false;
#endif
  for (int i : panel::OfferedUpscalers(s.size ? s.api : 0, s.size ? s.vendor : 0,
                                     {c.state == 2, c.dlss, c.fsr4, c.fsr3, c.xess}, windows))
    out.push_back(kUpscalers[i]);
  return out;
}

const char* UpscalerName(const std::string& value) {
  if (value == "auto") return "Auto";
  for (const auto& u : kUpscalers)
    if (value == u.value) return u.label;
  return nullptr;
}

std::string Number(double v, const char* format = "%.2f") {
  char buf[32];
  std::snprintf(buf, sizeof(buf), format, v);
  return buf;
}

// The value's label for the text dumps and the restart rows.
std::string ValueText(const Setting& s) {
  const std::string v = Get(s.cvar);
  if (s.kind == Kind::kUpscaler) {
    const char* n = UpscalerName(v);
    return n ? T(n) : v;
  }
  if (s.kind == Kind::kSharpness) return std::atof(v.c_str()) < 0 ? T("Off") : Number(std::atof(v.c_str()));
  if (s.kind == Kind::kSlider) return Number(std::atof(v.c_str()), s.format);
  for (const Option& o : s.options)
    if (v == o.value) return T(o.label);
  if (s.kind == Kind::kRestart) {
    if (std::string_view(s.cvar) == "kk_original_look") return T(v == "true" ? "Original Xbox 360" : "Remastered");
    if (std::string_view(s.cvar) == "present_effect") {
      if (v == "bilinear") return T("Off");
      if (v == "fsr") return "AMD FSR 1";
      if (v == "nis") return "NVIDIA NIS";
    }
    if (std::string_view(s.cvar) == "native_backend") {
      if (v == "auto") return T("Auto");
      if (v == "vulkan") return "Vulkan";
      if (v == "d3d12") return "DirectX 12";
    }
    if (std::string_view(s.cvar) == "kk_render_quality") {
      for (const auto& p : RenderPresets()) if (v == p.id) return T(p.label);
      if (v == "custom") return T("Custom");
    }
  }
  return v;
}

// ------------------------------------------------------------- trial state ---

std::atomic<bool> g_open{false};
std::mutex g_mutex;  // the UI thread changes; the save thread (none today) may read
std::map<std::string, std::string, std::less<>> g_saved;  // a changed setting's saved value
std::map<std::string, std::string, std::less<>> g_ab_trial;  // while F7 is held: the trial values
bool g_ab = false;
std::filesystem::path g_settings_path;

void Change(const char* name, const std::string& value) {
  if (g_ab) return;  // the saved settings are showing
  {
    std::lock_guard lock(g_mutex);
    const auto [entry, inserted] = g_saved.try_emplace(name, Get(name));
    if (inserted) SetTemporarySettingBaseline(name, entry->second);
  }
  if (rex::cvar::SetFlagByName(name, value)) SetTemporarySettingTrialValue(name, Get(name));
}

// The settings that differ from the saved ones now.
std::vector<std::string> Changed() {
  std::lock_guard lock(g_mutex);
  std::vector<std::string> out;
  for (const auto& [name, saved] : g_saved) {
    const auto it = g_ab_trial.find(name);
    const std::string trial = g_ab && it != g_ab_trial.end() ? it->second : Get(name.c_str());
    if (trial != saved) out.push_back(name);
  }
  return out;
}

void EndAb();

void Reset() {
  EndAb();
  std::lock_guard lock(g_mutex);
  for (const auto& [name, value] : g_saved) rex::cvar::SetFlagByName(name, value);
  g_saved.clear();
  ClearTemporarySettingBaselines();
}

bool SaveAsMine() {
  EndAb();
  if (!SaveSettings(g_settings_path, true)) return false;
  std::lock_guard lock(g_mutex);
  g_saved.clear();
  ClearTemporarySettingBaselines();
  return true;
}

// F7 held: the saved values, the trial ones put aside.
void StartAb() {
  std::lock_guard lock(g_mutex);
  if (g_ab) return;
  g_ab = true;
  g_ab_trial.clear();
  for (const auto& [name, saved] : g_saved) {
    const std::string trial = Get(name.c_str());
    if (trial == saved) continue;
    g_ab_trial[name] = trial;
    rex::cvar::SetFlagByName(name, saved);
  }
  REXLOG_INFO("KK graphics panel: A/B: saved settings ({} differ)", g_ab_trial.size());
}

void EndAb() {
  std::lock_guard lock(g_mutex);
  if (!g_ab) return;
  g_ab = false;
  for (const auto& [name, trial] : g_ab_trial) {
    rex::cvar::SetFlagByName(name, trial);
    SetTemporarySettingTrialValue(name, trial);
  }
  g_ab_trial.clear();
  REXLOG_INFO("KK graphics panel: A/B: trial settings again");
}

bool Visible(const Setting& s) {
  // Task 062: the three experimental lighting effects stay hidden for Beta 4.
  if (std::string_view(s.section) == "Lighting") return false;
  if (!Exists(s.cvar)) return false;
  if (s.native && !NativeRenderer()) return false;
  const std::string up = Get("native_upscaler");
  const bool temporal = NativeRenderer() && up != "off" && !up.empty();
  switch (s.show) {
    case Show::kAlways: return true;
    case Show::kTemporal: return temporal;
    case Show::kDlss: return temporal && (up == "dlss" || up == "auto");
    case Show::kFsr: return temporal && (up == "fsr3" || up == "fsr4" || up == "auto");
    case Show::kAo: return Get("ao_mode") != "0";
    case Show::kGi: return Get("native_gi") != "off";
    case Show::kShadows: return Get("native_shadow_quality") != "original";
    case Show::kWater: return Get("native_water_reflections") == "ssr";
    case Show::kShafts: return Get("native_volumetrics") != "off";
  }
  return true;
}

// ---------------------------------------------------------------- the text ---

// What the renderer applies this frame, and why a setting that is on does not show (the panel's check that
// a change reached the picture).
std::string AppliedText(const NrEffectStats& s, std::vector<std::string>* notes) {
  static const char* const kGi[] = {"Off", "Low", "Medium", "High"};
  std::string out;
  auto add = [&](const std::string& t) { out += (out.empty() ? "" : ", ") + t; };
  if (s.applied & 64) add(T("Upscaler"));
  if (s.applied & 1) add("AO");
  if (s.applied & 2) add(std::string("GI ") + T(kGi[std::min<uint32_t>(s.gi_level, 3)]));
  if (s.applied & 4)
    add(F("Soft shadows ({0} draws, maps x{1})", {std::to_string(s.shadow_draws), Number(s.shadow_scale, "%.3g")}));
  if (s.applied & 8) add(F("Water reflections ({0} water draws)", {std::to_string(s.water_draws)}));
  if (s.applied & 16) add(T("Light shafts"));
  if (s.applied & 32) add("FXAA");
  if (s.anisotropy >= 2) add(F("{0}x filtering", {std::to_string(1 << (s.anisotropy - 1))}));
  if (notes) {
    if (Get("native_shadow_quality") != "original" && !(s.applied & 4))
      notes->push_back(T(s.shadow_draws_waiting ? "Shadows: their shader is being prepared."
                                              : "Shadows: nothing in view draws the sun's shadows here."));
    if (Get("native_water_reflections") == "ssr" && !s.water_draws)
      notes->push_back(T(s.water_draws_waiting ? "Water reflections: their shader is being prepared."
                                             : "Water reflections: no water in view."));
    if (Get("swap_post_effect") != "none" && (s.applied & 64))
      notes->push_back(T("FXAA: off while a temporal upscaler runs (it anti-aliases itself)."));
    if (Get("native_volumetrics") != "off" && !(s.applied & 16)) notes->push_back(T("Light shafts: being prepared."));
    if (Get("native_gi") != "off" && !(s.applied & 2)) notes->push_back(T("GI: being prepared."));
  }
  return out.empty() ? T("Nothing beyond the game's own picture") : out;
}

std::string Readout() {
  std::string out;
  const auto frame = GetGuestFrameStats();
  out += F("Frame rate: {0} FPS, {1} ms a frame, longest {2} ms in the last second",
           {Number(frame.fps, "%.1f"), Number(frame.frame_time_ms), Number(GuestWorstFrameMs(), "%.0f")}) + "\n";
  const NrEffectStats s = Stats();
  if (NativeRenderer() && s.size && s.frame) {
    out += F("Renderer: Native, {0}", {s.api == 1 ? "Vulkan" : "DirectX 12"}) + "\n";
    out += std::string(T("Upscaler")) + ": " + T(UpscalerName(s.upscaler) ? UpscalerName(s.upscaler) : s.upscaler);
    if (std::string_view(s.upscaler) != "off")
      out += F(", draws {0}x{1} for {2}x{3}, mip bias {4}", {std::to_string(s.input_width), std::to_string(s.input_height),
                 std::to_string(s.output_width), std::to_string(s.output_height), Number(s.mip_bias)});
    else
      out += F(", draws {0}x{1}", {std::to_string(s.output_width), std::to_string(s.output_height)});
    out += "\n";
    std::string gpu;
    auto add = [&](const char* name, double ms) {
      if (ms >= 0) gpu += std::string(gpu.empty() ? "" : ", ") + T(name) + " " + Number(ms) + " ms";
    };
    add("Ambient occlusion", s.ao_ms);
    add("GI", s.gi_ms);
    add("Shadows", s.shadows_ms);
    add("Water", s.water_ms);
    add("Light shafts", s.volumetric_ms);
    add("Upscaler", s.upscaler_ms);
    add("Tessellation", s.tessellation_ms);
    out += F("Tessellated Ann draws: {0}", {std::to_string(s.tessellation_draws)}) + "\n";
    out += F("GPU time a frame: {0}", {gpu.empty() ? T("No effect timed") : gpu}) + "\n";
    std::vector<std::string> notes;
    out += F("Applied: {0}", {AppliedText(s, &notes)}) + "\n";
    for (const auto& n : notes) out += "  " + n + "\n";
  } else {
    out += std::string(T("Renderer: Native")) + "\n";
  }
  return out;
}

std::string SettingsText() {
  std::string out = std::string(T("King Kong PC port graphics settings (F8 panel)")) + "\n" + T("Version") + ": " KK_VERSION "\n";
  out += DiagnosticsHardware();
  out += Readout();
  const auto changed = Changed();
  out += std::string(T("Settings (* changed in the panel, not saved):")) + "\n";
  for (const Setting& s : Settings()) {
    if (!Exists(s.cvar) || (s.native && !NativeRenderer())) continue;
    const bool star = std::find(changed.begin(), changed.end(), s.cvar) != changed.end();
    std::string value = ValueText(s);
    if (s.kind == Kind::kMipBias && value == "auto") value = T("Auto");
    out += std::string("  ") + T(s.section) + " / " + T(s.label) + ": " + value + (star ? " *" : "") +
           (s.kind == Kind::kRestart ? std::string(" (") + T("Needs restart") + ")" : "") + "\n";
  }
  return RedactDiagnostics(out);
}

// ---------------------------------------------------------------- the panel ---

class GraphicsPanel final : public rex::ui::ImGuiDialog, public rex::ui::WindowInputListener {
 public:
  GraphicsPanel(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, std::filesystem::path user_root)
      : ImGuiDialog(drawer), window_(window), user_root_(std::move(user_root)) {
    // Above the SDK's input (the ImGui drawer, the keyboard and mouse driver, the mouse look): the
    // panel's keys and, while it is open, the mouse.
    window_->AddInputListener(this, 1200);
    pad_input_.set_keyboard_enabled(false);
  }
  ~GraphicsPanel() override { window_->RemoveInputListener(this); }

  bool NeedsContinuousPaint() override { return g_open.load(); }

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!InPlay()) return;
    switch (e.virtual_key()) {
      case rex::ui::VirtualKey::kF8:
        if (!e.prev_state()) Toggle();
        e.set_handled(true);
        break;
      case rex::ui::VirtualKey::kF7:
        if (!e.prev_state()) { key_compare_ = true; Comparison(); }
        e.set_handled(true);
        break;
      case rex::ui::VirtualKey::kF6:
        if (!e.prev_state()) Capture();
        e.set_handled(true);
        break;
      default:
        break;
    }
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (e.virtual_key() == rex::ui::VirtualKey::kF7 && (g_ab || InPlay())) {
      key_compare_ = false;
      Comparison();
      e.set_handled(true);
    } else if ((e.virtual_key() == rex::ui::VirtualKey::kF8 || e.virtual_key() == rex::ui::VirtualKey::kF6) && InPlay()) {
      e.set_handled(true);
    }
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (!Interactive()) return;
    held_[int(e.button())] = true;
    static_cast<rex::ui::WindowInputListener*>(imgui_drawer())->OnMouseDown(e);
    e.set_handled(true);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (!held_[int(e.button())]) return;  // a button pressed in the game is let go in the game
    held_[int(e.button())] = false;
    static_cast<rex::ui::WindowInputListener*>(imgui_drawer())->OnMouseUp(e);
    e.set_handled(true);
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!Interactive()) return;
    static_cast<rex::ui::WindowInputListener*>(imgui_drawer())->OnMouseMove(e);
    e.set_handled(true);
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (!Interactive()) return;
    static_cast<rex::ui::WindowInputListener*>(imgui_drawer())->OnMouseWheel(e);
    e.set_handled(true);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
#if defined(KK_DEV_TOOLS)
    UiPaintTick();
    TestTick();
#endif
    ControllerTick();
#if defined(KK_DEV_TOOLS)
    ControllerTestTick();
#endif
    // A held F7 whose release went to another window (focus lost) ends the comparison.
    const bool focus = window_->HasFocus();
    if (had_focus_ && !focus) {
      key_compare_ = false; pad_compare_ = false; pad_ready_ = false; EndAb();
    }
    had_focus_ = focus;
    if (!InPlay()) {
      EndAb();
      if (g_open.load()) SetOpen(false);
    }
    UpdateCursor(io);
    if (!g_open.load()) return;
    if (REXCVAR_GET(kk_graphics_panel_compact)) {
      DrawCompact(io);
    } else {
      DrawPanel(io);
    }
  }

 private:
  static bool InPlay() { return play::SessionActive() && !play::CoverScreen() && !play::PauseMenuOpen(); }
  static bool Interactive() { return g_open.load() && !REXCVAR_GET(kk_graphics_panel_compact); }

  void Toggle() {
    // Compact readout: F8 brings the full panel; the full panel: F8 closes it.
    if (g_open.load() && REXCVAR_GET(kk_graphics_panel_compact)) {
      rex::cvar::SetFlagByName("kk_graphics_panel_compact", "false");
      pad_ready_ = false;
      return;
    }
    SetOpen(!g_open.load());
  }
  void SetOpen(bool open) {
    g_open.store(open);
    if (open) { focus_row_ = 0; focus_cvar_.clear(); pad_ready_ = false; }
    else { pad_compare_ = false; key_compare_ = false; EndAb(); }
    window_->RequestPresenterUIPaintFromUIThread();
    REXLOG_INFO("KK graphics panel: {}", open ? "open" : "closed");
  }

  void Comparison() {
    if (key_compare_ || pad_compare_) StartAb();
    else EndAb();
  }

  std::vector<const Setting*> ControllerRows() {
    std::vector<const Setting*> rows;
    for (const auto& st : Settings()) if (Visible(st)) rows.push_back(&st);
    return rows;
  }

  void Adjust(const Setting& st, int direction) {
    if (g_ab || st.kind == Kind::kRestart) return;
    std::vector<Option> options = st.options;
    if (st.kind == Kind::kUpscaler)
      for (const auto& u : OfferedUpscalers()) options.push_back({u.label, u.value});
    const std::string cur = Get(st.cvar);
    if (!options.empty()) {
      int index = -1;
      for (int i = 0; i < int(options.size()); ++i) if (cur == options[i].value) index = i;
      Change(st.cvar, options[panel::NextChoice(index, int(options.size()), direction)].value);
      return;
    }
    if (st.kind == Kind::kMipBias && (cur == "auto" || cur.empty())) {
      Change(st.cvar, Number(direction > 0 ? st.min : st.max));
      return;
    }
    float value = float(std::atof(cur.c_str()));
    if (st.kind == Kind::kMipBias && ((value <= st.min && direction < 0) || (value >= st.max && direction > 0))) {
      Change(st.cvar, "auto");
    } else if (st.kind == Kind::kSharpness && value <= 0 && direction < 0) {
      Change(st.cvar, "-1");
    } else {
      const float step = st.kind == Kind::kSliderInt ? 1.0f : 0.05f;
      value = value < st.min ? st.min : ui::StepSlider(value, st.min, st.max, step, direction);
      Change(st.cvar, Number(value, st.kind == Kind::kSliderInt ? "%.0f" : "%.2f"));
    }
  }

  void FooterAction(int row) {
    if (row == 0 && !g_ab) { Reset(); Status(T("Your saved settings are back.")); }
    if (row == 1 && !g_ab)
      Status(T(SaveAsMine() ? "Saved: these are now your settings, in the launcher too." : "Could not save the settings."));
    if (row == 2) {
      ImGui::SetClipboardText(SettingsText().c_str());
      Status(T("Copied the settings, frame rate and GPU times (names and paths left out)."));
    }
    if (row == 3) Capture();
    if (row == 4) { rex::cvar::SetFlagByName("kk_graphics_panel_compact", "true"); pad_compare_ = false; Comparison(); }
  }

  void ControllerTick() {
    panel::PadState pad;
    bool toggle = false;
    {
      std::lock_guard lock(g_pad_mutex);
      toggle = g_pad[0].TakeToggle();
      if (std::chrono::steady_clock::now() - g_pad_at < std::chrono::milliseconds(250)) pad = g_pad[0].latest;
    }
    if (toggle && InPlay()) { Toggle(); pad_navigation_ = true; scroll_to_focus_ = true; }
    if (!Interactive() || !InPlay()) {
      if (pad_compare_) { pad_compare_ = false; Comparison(); }
      pad_input_.FeedPad(0, 0, 0, 0, 0);
      pad_input_.Update();
      return;
    }
    // An input already held as the panel opens must first be released.
    if (!pad_ready_) {
      pad_ready_ = pad.Neutral();
      pad = {};
    }
    const bool compare = (pad.buttons & 0x0100) != 0;
    if (compare != pad_compare_) { pad_compare_ = compare; Comparison(); }
    pad_input_.FeedPad(pad.buttons, pad.lx, pad.ly, 0, 0);
    pad_input_.Update();
    using ui::Action;
    if (pad_input_.Pressed(Action::kBack)) { SetOpen(false); return; }
    if (pad_input_.Pressed(Action::kNextTab)) Capture();  // RB
    const auto rows = ControllerRows();
    if (!focus_cvar_.empty()) {
      for (int i = 0; i < int(rows.size()); ++i) if (focus_cvar_ == rows[i]->cvar) focus_row_ = i;
    }
    focus_row_ = std::clamp(focus_row_, 0, int(rows.size()) + 5);
    const int move = int(pad_input_.Pressed(Action::kDown)) - int(pad_input_.Pressed(Action::kUp));
    if (move) {
      focus_row_ = (focus_row_ + int(rows.size()) + 6 + move) % (int(rows.size()) + 6);
      pad_navigation_ = true;
      scroll_to_focus_ = true;
    }
    focus_cvar_ = focus_row_ < int(rows.size()) ? rows[focus_row_]->cvar : "";
    int direction = int(pad_input_.Pressed(Action::kRight)) - int(pad_input_.Pressed(Action::kLeft));
    if (direction || pad_input_.Pressed(Action::kAccept)) pad_navigation_ = true;
    if (focus_row_ < int(rows.size())) {
      const auto& st = *rows[focus_row_];
      if (!direction && pad_input_.Pressed(Action::kAccept) && st.kind == Kind::kToggle) direction = 1;
      if (direction) Adjust(st, direction);
    } else {
      const int footer = focus_row_ - int(rows.size());
      if (footer == 5 && direction)
        rex::cvar::SetFlagByName("kk_graphics_panel_opacity",
                               Number(ui::StepSlider(float(REXCVAR_GET(kk_graphics_panel_opacity)), 0.3f, 1.0f, 0.05f, direction)));
      if (pad_input_.Pressed(Action::kAccept)) FooterAction(footer);
    }
  }

  void MarkFocus(bool focused) {
    if (!pad_navigation_ || !focused) return;
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(ImVec2(lo.x - 2, lo.y - 2), ImVec2(hi.x + 2, hi.y + 2), ui::color::kFocus);
    if (scroll_to_focus_) { ImGui::SetScrollHereY(); scroll_to_focus_ = false; }
  }

  void UpdateCursor(ImGuiIO& io) {
    const bool want = Interactive();
    if (want == cursor_shown_) return;
    cursor_shown_ = want;
    if (want) {
      nav_before_ = (io.ConfigFlags & ImGuiConfigFlags_NavNoCaptureKeyboard) != 0;
      io.ConfigFlags |= ImGuiConfigFlags_NavNoCaptureKeyboard;
    } else {
      if (!nav_before_) io.ConfigFlags &= ~ImGuiConfigFlags_NavNoCaptureKeyboard;
      for (auto& [button, down] : held_) down = false;
    }
  }

  void Capture() {
    std::error_code ec;
    const auto dir = user_root_ / "screenshots";
    std::filesystem::create_directories(dir, ec);
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char name[64];
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::snprintf(name, sizeof(name), "kk-%04d%02d%02d-%02d%02d%02d-%03d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    const auto png = dir / (std::string(name) + art::kDevCaptureExtension);
    art::CaptureFrame(png);
    std::string text = SettingsText();
    if (g_ab) text += std::string(T("Captured while showing your saved settings.")) + "\n";
    std::ofstream(dir / (std::string(name) + ".txt"), std::ios::binary | std::ios::trunc) << text;
    last_capture_ = png;
    Status(F("Saved {0} and its settings in screenshots.", {std::string(name) + art::kDevCaptureExtension}));
    REXLOG_INFO("KK graphics panel: capture {}", name);
  }

  void Status(std::string text) {
    status_ = std::move(text);
    status_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  }

  void PushStyle(float alpha) { ui::PushGraphicsPanelStyle(alpha); }
  static void PopStyle() { ui::PopGraphicsPanelStyle(); }

  void DrawCompact(ImGuiIO& io) {
    const float s = ImGui::GetFontSize() / 18.0f;
    // Under the frame counter (F2) when it shows.
    const float top = (rex::cvar::GetFlagByName("kk_show_fps") == "true" ? 64 : 16) * s;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * s, top), ImGuiCond_Always, ImVec2(1, 0));
    PushStyle(float(std::clamp(REXCVAR_GET(kk_graphics_panel_opacity), 0.3, 1.0)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##kk_graphics_compact", nullptr, flags)) {
      const auto frame = GetGuestFrameStats();
      ImGui::Text(T("%.0f FPS  %.1f ms  (longest %.0f ms)"), frame.fps, frame.frame_time_ms, GuestWorstFrameMs());
      if (g_ab) {
        ImGui::TextColored(ImColor(ui::color::kAccent), "%s", T("Your saved settings (F7 / LB)"));
      } else {
        const size_t n = Changed().size();
        if (n) ImGui::TextColored(ImColor(ui::color::kWarn), "%s", F("Unsaved changes: {0}", {std::to_string(n)}).c_str());
        ImGui::TextDisabled("%s", T("Hold F7: saved settings   F8 / L3+R3: panel"));
      }
    }
    ImGui::End();
    PopStyle();
  }

  void DrawPanel(ImGuiIO& io) {
    const float s = ImGui::GetFontSize() / 18.0f;
    const float margin = 24 * s;
    ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(std::min(560 * s, io.DisplaySize.x - 2 * margin), io.DisplaySize.y - 2 * margin),
                             ImGuiCond_Appearing);
    PushStyle(float(std::clamp(REXCVAR_GET(kk_graphics_panel_opacity), 0.3, 1.0)));
    bool visible = true;
    if (ImGui::Begin((std::string(T("Graphics (F8)")) + "###kk_graphics_panel").c_str(), &visible,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
      DrawHeader();
      const float footer = ImGui::GetFrameHeightWithSpacing() * 5.2f + ImGui::GetTextLineHeightWithSpacing() * 2.2f;
      if (ImGui::BeginChild("##settings", ImVec2(0, -footer))) DrawSettings();
      ImGui::EndChild();
      DrawFooter();
    }
    ImGui::End();
    PopStyle();
    if (!visible) SetOpen(false);
  }

  void DrawHeader() {
    const auto frame = GetGuestFrameStats();
    ImGui::Text(T("%.1f FPS   %.2f ms   longest %.0f ms"), frame.fps, frame.frame_time_ms, GuestWorstFrameMs());
    const NrEffectStats st = Stats();
    if (NativeRenderer() && st.size && st.frame) {
      if (std::string_view(st.upscaler) != "off") {
        ImGui::TextWrapped(T("%s: draws %ux%u for %ux%u"), st.upscaler, st.input_width, st.input_height, st.output_width,
                           st.output_height);
      } else {
        ImGui::Text(T("No temporal upscaler: draws %ux%u"), st.output_width, st.output_height);
      }
      std::string gpu;
      auto add = [&](const char* name, double ms) {
        if (ms >= 0) gpu += std::string(gpu.empty() ? "" : "   ") + T(name) + " " + Number(ms) + " ms";
      };
      add("AO", st.ao_ms);
      add("GI", st.gi_ms);
      add("Shadows", st.shadows_ms);
      add("Water", st.water_ms);
      add("Shafts", st.volumetric_ms);
      add("Upscaler", st.upscaler_ms);
      add("Tessellation", st.tessellation_ms);
      if(Get("native_tessellation")=="true" && !st.tessellation_draws)
        ImGui::TextDisabled("%s", T("Tessellation: no eligible prepared Ann draw"));
      if (!gpu.empty()) ImGui::TextColored(ImColor(ui::color::kTextDim), "GPU: %s", gpu.c_str());
      std::vector<std::string> notes;
      const std::string applied = AppliedText(st, &notes);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(ImColor(ui::color::kTextDim), "%s", F("Applied: {0}", {applied}).c_str());
      ImGui::PopTextWrapPos();
      for (const auto& n : notes) ImGui::TextColored(ImColor(ui::color::kWarn), "%s", n.c_str());
      if (st.preparing) {
        std::string what;
        if (st.preparing & 4) what += std::string(" ") + T("Upscaler");
        if (st.preparing & 1) what += " GI";
        if (st.preparing & 2) what += std::string(" ") + T("Lighting");
        if (st.preparing & 8) what += std::string(" ") + T("Shaders");
        ImGui::TextColored(ImColor(ui::color::kWarn), "%s", F("Preparing:{0} (the game keeps drawing meanwhile)", {what}).c_str());
      }

    }
    if (g_ab) {
      ImGui::TextWrapped("%s", T("Showing your saved settings while F7 or LB is held."));
    } else {
      const size_t n = Changed().size();
      if (n) {
        ImGui::TextWrapped("%s", F("Unsaved changes: {0} (temporary until you save).", {std::to_string(n)}).c_str());
      } else {
        ImGui::TextWrapped("%s", T("Changes apply at once and stay until the game closes."));
      }
      ImGui::TextWrapped("%s", T("Hold F7 / LB to compare. F6 / RB captures."));
    }
    ImGui::TextWrapped("%s", T("L3+R3: panel. D-pad / left stick: move. Left/right: change. A: select. B: close."));
    ImGui::Separator();
  }

  void DrawSettings() {
    const char* section = nullptr;
    bool section_open = false;
    column_ = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.42f;
    ImGui::BeginDisabled(g_ab);
    for (const Setting& st : Settings()) {
      if (!Visible(st)) continue;
      if (!section || std::string_view(section) != st.section) {
        section = st.section;
        ImGui::Spacing();
        if (pad_navigation_ && focus_cvar_ == st.cvar) ImGui::SetNextItemOpen(true);
        // A focused row can be below the section's first setting.
        if (pad_navigation_)
          for (const auto& row : Settings())
            if (focus_cvar_ == row.cvar && std::string_view(row.section) == st.section) ImGui::SetNextItemOpen(true);
        section_open = ImGui::CollapsingHeader((std::string(T(st.section)) + "###" + st.section).c_str(), std::string_view(st.section) == "Needs restart"
                                                               ? ImGuiTreeNodeFlags_None
                                                               : ImGuiTreeNodeFlags_DefaultOpen);
      }
      if (section_open) DrawSetting(st);
    }
    ImGui::EndDisabled();
  }

  void Label(const Setting& st) {
    const bool changed = [&] {
      std::lock_guard lock(g_mutex);
      const auto it = g_saved.find(st.cvar);
      return it != g_saved.end() && it->second != Get(st.cvar);
    }();
    ImGui::AlignTextToFramePadding();
    if (changed) ImGui::TextColored(ImColor(ui::color::kAccent), "%s *", T(st.label));
    else ImGui::TextUnformatted(T(st.label));
    MarkFocus(focus_cvar_ == st.cvar);
    if (st.help[0] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(st.help));
    ImGui::SameLine(column_);
    ImGui::SetNextItemWidth(-1);
  }

  void Combo(const Setting& st, const std::vector<Option>& options) {
    const std::string cur = Get(st.cvar);
    std::string preview = cur == "auto" ? T("Auto") : cur;
    if (const char* name = UpscalerName(cur); st.kind == Kind::kUpscaler && name) preview = T(name);
    for (const Option& o : options)
      if (cur == o.value) preview = T(o.label);
    if (ImGui::BeginCombo((std::string("##") + st.cvar).c_str(), preview.c_str())) {
      for (const Option& o : options) {
        if (ImGui::Selectable(T(o.label), cur == o.value) && cur != o.value) Change(st.cvar, o.value);
      }
      ImGui::EndCombo();
    }
  }

  void DrawSetting(const Setting& st) {
    ImGui::PushID(st.cvar);
    Label(st);
    const std::string id = std::string("##") + st.cvar;
    const ImGuiSliderFlags slider = ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput;
    switch (st.kind) {
      case Kind::kChoice:
        Combo(st, st.options);
        break;
      case Kind::kChoiceSlider: {
        int value = 0;
        for (int i = 0; i < int(st.options.size()); ++i)
          if (Get(st.cvar) == st.options[i].value) value = i;
        const auto label = T(st.options[value].label);
        if (ImGui::SliderInt(id.c_str(), &value, 0, int(st.options.size()) - 1, label, slider))
          Change(st.cvar, st.options[value].value);
        break;
      }
      case Kind::kToggle: {
        bool on = Get(st.cvar) == st.options[1].value;
        if (ImGui::Checkbox(id.c_str(), &on)) Change(st.cvar, on ? st.options[1].value : st.options[0].value);
        break;
      }
      case Kind::kSlider: {
        float v = float(std::atof(Get(st.cvar).c_str()));
        if (ImGui::SliderFloat(id.c_str(), &v, st.min, st.max, st.format, slider)) Change(st.cvar, Number(v, "%.3f"));
        break;
      }
      case Kind::kSliderInt: {
        int v = std::atoi(Get(st.cvar).c_str());
        if (ImGui::SliderInt(id.c_str(), &v, int(st.min), int(st.max), st.format, slider))
          Change(st.cvar, std::to_string(v));
        break;
      }
      case Kind::kUpscaler: {
        std::vector<Option> options;
        for (const auto& u : OfferedUpscalers()) options.push_back({u.label, u.value});
        const std::string cur = Get(st.cvar);
        if (cur == "auto") options.push_back({"Auto", "auto"});
        // Unavailable saved choices remain a preview, never a selectable entry.
        Combo(st, options);
        break;
      }
      case Kind::kSharpness: {
        float v = float(std::atof(Get(st.cvar).c_str()));
        bool on = v >= 0;
        if (ImGui::Checkbox("##on", &on)) Change(st.cvar, on ? "0.5" : "-1");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(!on);
        float shown = std::max(v, 0.0f);
        if (ImGui::SliderFloat(id.c_str(), &shown, 0, 1, "%.2f", slider)) Change(st.cvar, Number(shown, "%.3f"));
        ImGui::EndDisabled();
        break;
      }
      case Kind::kMipBias: {
        const std::string cur = Get(st.cvar);
        bool automatic = cur == "auto" || cur.empty();
        if (ImGui::Checkbox((std::string(T("Auto")) + "##auto").c_str(), &automatic)) Change(st.cvar, automatic ? "auto" : "0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(automatic);
        // A step of 0.05, applied when the slider is let go (each value makes the scene's samplers again).
        if (!automatic && !dragging_bias_) bias_ = float(std::atof(cur.c_str()));
        ImGui::SliderFloat(id.c_str(), &bias_, st.min, st.max, "%.2f", slider);
        dragging_bias_ = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) Change(st.cvar, Number(std::round(bias_ * 20.0f) / 20.0f, "%.2f"));
        ImGui::EndDisabled();
        break;
      }
      case Kind::kRestart:
        ImGui::TextWrapped("%s", F("{0} (needs restart: change it in the launcher)", {ValueText(st)}).c_str());
        break;
    }
    if (pad_navigation_ && focus_cvar_ == st.cvar && st.help[0]) ImGui::TextWrapped("%s", T(st.help));
    ImGui::PopID();
  }

  void DrawFooter() {
    ImGui::Separator();
    ImGui::BeginDisabled(g_ab);
    const int footer = focus_row_ - int(ControllerRows().size());
    if (ImGui::Button(T("Reset to my settings"))) FooterAction(0);
    MarkFocus(footer == 0);
    if (ImGui::Button(T("Save as my settings"))) FooterAction(1);
    MarkFocus(footer == 1);
    ImGui::EndDisabled();
    if (ImGui::Button(T("Copy settings"))) FooterAction(2);
    MarkFocus(footer == 2);
    ImGui::SameLine();
    if (ImGui::Button(T("Capture (F6 / RB)"))) Capture();
    MarkFocus(footer == 3);
    ImGui::SameLine();
    if (ImGui::Button(T("Compact"))) FooterAction(4);
    MarkFocus(footer == 4);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Opacity"));
    MarkFocus(footer == 5);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    float opacity = float(REXCVAR_GET(kk_graphics_panel_opacity));
    if (ImGui::SliderFloat("##opacity", &opacity, 0.3f, 1.0f, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
      rex::cvar::SetFlagByName("kk_graphics_panel_opacity", Number(opacity, "%.2f"));
    if (!status_.empty() && std::chrono::steady_clock::now() < status_until_)
      ImGui::TextColored(ImColor(ui::color::kGood), "%s", status_.c_str());
  }

#if defined(KK_DEV_TOOLS)
  // KK_DEV_PANEL_CONTROLLER=1, with KK_DEV_AUTOSKIP=1 and a direct chapter
  // start: a short controller-only pass through the actual pad wrapper. No OS
  // input or focus is needed. The old full settings stress harness stays opt-in.
  void ControllerTestTick() {
    const double start = std::getenv("KK_DEV_PANEL_CONTROLLER_START") ? std::atof(std::getenv("KK_DEV_PANEL_CONTROLLER_START")) : 8;
    if (!std::getenv("KK_DEV_PANEL_CONTROLLER") || controller_test_stage_ >= 1000 || !InPlay() ||
        DevSecondsSinceMenu() < start) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < controller_test_next_) return;
    int samples;
    bool consumed;
    {
      std::lock_guard lock(g_pad_mutex);
      samples = g_controller_test_samples;
      consumed = g_controller_test_consumed;
    }
    auto check = [&](bool ok, const char* message) {
      ++(ok ? test_pass_ : test_fail_);
      REXLOG_INFO("KK panel controller test: {} {}", ok ? "PASS" : "FAIL", message);
    };
    auto drive = [&](panel::PadState state) {
      std::lock_guard lock(g_pad_mutex);
      g_controller_test_override = true;
      g_controller_test_pad = state;
      g_controller_test_samples = 0;
      controller_test_next_ = now + std::chrono::milliseconds(200);
    };
    if (controller_test_stage_ && !samples) {
      // A missing/disconnected scripted pad is a failure, not a false pass.
      if (++controller_test_waits_ < 30) return;
      check(false, "no game pad reads reached the controller harness");
      controller_test_stage_ = 24;
    }
    controller_test_waits_ = 0;
    switch (controller_test_stage_++) {
      case 0:
        controller_test_upscaler_ = Get("native_upscaler");
        controller_test_fxaa_ = Get("swap_post_effect");
        controller_test_language_ = Get("user_language");
        SetOpen(false);
        rex::cvar::SetFlagByName("native_upscaler", "off");
        rex::cvar::SetFlagByName("swap_post_effect", "none");
        drive({});
        break;
      case 1: drive({0x00C0}); break;
      case 2:
        check(Interactive() && consumed, "both stick clicks open the panel and never reach the game");
        drive({0x00C0});
        break;
      case 3:
        check(Interactive(), "holding the opening chord does not close the panel again");
        drive({});
        break;
      case 4: drive({0x0002}); break;
      case 5:
        check(consumed && focus_row_ > 0, "D-pad moves focus while the game's pad remains neutral");
        drive({});
        if (focus_cvar_ != "swap_post_effect" && ++controller_test_moves_ < 30) controller_test_stage_ = 4;
        break;
      case 6: drive({0x0008}); break;
      case 7:
        check(consumed && Get("swap_post_effect") == "fxaa", "right changes the focused setting without a game input");
        drive({});
        break;
      case 8: drive({0x0100}); break;
      case 9:
        check(consumed && g_ab && Get("swap_post_effect") == "none", "LB held shows the saved setting");
        Key(rex::ui::VirtualKey::kF7, true);
        drive({});
        break;
      case 10:
        check(g_ab, "releasing LB keeps comparison while F7 is held");
        Key(rex::ui::VirtualKey::kF7, false);
        check(!g_ab && Get("swap_post_effect") == "fxaa", "releasing comparison restores the trial");
        drive({0x0200});
        break;
      case 11:
        // Frame encoding happens on a worker. Give it up to five seconds,
        // without holding a direction long enough to repeat it.
        if (!last_capture_.empty() && !std::filesystem::exists(last_capture_) && ++controller_capture_waits_ < 25) {
          controller_test_stage_ = 11;
          controller_test_next_ = now + std::chrono::milliseconds(200);
          return;
        }
        check(consumed && !last_capture_.empty() && std::filesystem::exists(last_capture_), "RB captures the game frame");
        drive({});
        break;
      case 12: drive({0, 0, 0, 0, 32767}); break;
      case 13:
        check(consumed && focus_cvar_ != "swap_post_effect", "left stick up moves focus without moving the player");
        drive({});
        break;
      case 14: drive({0, 0, 0, 0, -32768}); break;
      case 15:
        check(consumed && focus_cvar_ == "swap_post_effect", "left stick down returns to the setting");
        drive({});
        break;
      case 16: {
        const char* languages[] = {"1", "3", "5", "4", "6"};
        if (controller_language_step_ < 5) {
          rex::cvar::SetFlagByName("user_language", languages[controller_language_step_++]);
          REXLOG_INFO("KK panel controller test: show language {}", Get("user_language"));
          drive({});
          controller_test_stage_ = 16;
          controller_test_next_ = now + std::chrono::milliseconds(800);
        } else drive({0xFFFF, 255, 255, 32767, -32768, -32768, 32767});
        break;
      }
      case 17:
        check(!Interactive() && consumed && !g_ab, "closing chord consumes all controls and ends comparison");
        drive({0x2000, 255, 255, 32767, 0, -32768, 0});
        break;
      case 18:
        check(consumed, "held buttons, triggers and sticks remain hidden after closing");
        drive({});
        break;
      case 19:
        check(!consumed, "neutral restores game input after closing");
        drive({0x00C0});
        break;
      case 20:
        check(Interactive(), "controller reopens the panel");
        drive({});
        break;
      case 21: drive({0x2000}); break;
      case 22:
        check(!Interactive() && consumed, "B closes the panel without reaching the game");
        drive({});
        break;
      case 23:
        check(!consumed, "game input resumes after B is released");
        drive({});
        break;
      case 24:
        SetOpen(false);
        Reset();
        rex::cvar::SetFlagByName("native_upscaler", controller_test_upscaler_);
        rex::cvar::SetFlagByName("swap_post_effect", controller_test_fxaa_);
        rex::cvar::SetFlagByName("user_language", controller_test_language_);
        {
          std::lock_guard lock(g_pad_mutex);
          g_controller_test_override = false;
        }
        REXLOG_INFO("KK panel controller test: done: {} passed, {} failed", test_pass_, test_fail_);
        controller_test_stage_ = 1000;
        break;
    }
  }
  int controller_test_stage_ = 0, controller_test_moves_ = 0, controller_test_waits_ = 0;
  int controller_capture_waits_ = 0, controller_language_step_ = 0;
  std::chrono::steady_clock::time_point controller_test_next_{};
  std::string controller_test_upscaler_, controller_test_fxaa_, controller_test_language_;

  // ------------------------------------------------------- the robustness test ---
  // KK_DEV_PANEL_TEST=<folder>: in play, every live setting is toggled KK_DEV_PANEL_TOGGLES times (100)
  // every KK_DEV_PANEL_INTERVAL seconds (0.1), with a capture before and after each; then the upscalers
  // round and round, the A/B key, the capture key, Reset and Save. Results in the log ("KK panel test").
  struct TestStep {
    std::string cvar;
    std::vector<std::string> values;  // cycled; the last is the "after" picture's
    std::string upscaler;             // the upscaler to run under (its own rows), empty: as it is
  };
  std::vector<TestStep> TestPlan() {
    std::vector<TestStep> plan;
    const char* only = std::getenv("KK_DEV_PANEL_ONLY");
    std::vector<std::string> upscalers;
    for (const auto& u : OfferedUpscalers()) upscalers.push_back(u.value);
    if (const char* list = std::getenv("KK_DEV_PANEL_UPSCALERS"); list && *list) {
      upscalers.clear();
      std::string l = list;
      for (size_t a = 0; a <= l.size();) {
        const size_t b = std::min(l.find(',', a), l.size());
        if (b > a) upscalers.push_back(l.substr(a, b - a));
        a = b + 1;
      }
    }
    const std::string main_up = std::getenv("KK_DEV_PANEL_UPSCALER") ? std::getenv("KK_DEV_PANEL_UPSCALER") : "";
    for (const Setting& st : Settings()) {
      if (st.kind == Kind::kRestart || !Exists(st.cvar) || (st.native && !NativeRenderer())) continue;
      if (only && *only && std::string(only).find(st.cvar) == std::string::npos) continue;
      TestStep step;
      step.cvar = st.cvar;
      switch (st.kind) {
        case Kind::kChoice:
        case Kind::kChoiceSlider:
        case Kind::kToggle:
          for (const Option& o : st.options) step.values.push_back(o.value);
          break;
        case Kind::kSlider:
          step.values = {Number(st.min, "%.3f"), Number(st.max, "%.3f"), Number((st.min + st.max) / 2, "%.3f")};
          break;
        case Kind::kSliderInt:
          step.values = {std::to_string(int(st.min)), std::to_string(int(st.max)), std::to_string(int(std::lround((st.min+st.max)*0.5f)))};
          break;
        case Kind::kUpscaler:
          step.values = upscalers;
          if (!main_up.empty()) step.values.push_back(main_up);
          break;
        case Kind::kSharpness:
          step.values = {"-1", "0", "1", "0.5"};
          break;
        case Kind::kMipBias:
          step.values = {"auto", "-1", "0.5", "-0.5"};
          break;
        case Kind::kRestart:
          break;
      }
      if (st.show == Show::kTemporal || st.show == Show::kFsr || st.show == Show::kDlss) {
        step.upscaler = st.show == Show::kFsr ? "fsr3" : st.show == Show::kDlss ? "dlss" : main_up;
        if (step.upscaler.empty()) step.upscaler = "fsr3";
      }
      // An effect's own sliders are tried with the effect on.
      if (st.show == Show::kAo || st.show == Show::kGi || st.show == Show::kShadows || st.show == Show::kWater ||
          st.show == Show::kShafts) {
        TestStep on;
        on.cvar = st.show == Show::kAo ? "ao_mode" : st.show == Show::kGi ? "native_gi" :
                  st.show == Show::kShadows ? "native_shadow_quality" : st.show == Show::kWater ?
                  "native_water_reflections" : "native_volumetrics";
        on.values = {st.show == Show::kAo ? "1" : st.show == Show::kGi ? "high" : st.show == Show::kShadows ? "ultra" :
                     st.show == Show::kWater ? "ssr" : "high"};
        step.upscaler = "";
        plan.push_back({"#enable " + on.cvar, on.values, ""});
      }
      plan.push_back(step);
    }
    return plan;
  }

  void TestCapture(const std::string& name) {
    art::CaptureFrame(test_dir_ / (name + art::kDevCaptureExtension));
  }

  void TestTick() {
    // Task 121: bounded same-camera comparisons; no desktop capture or focus changes.
    if (const char* dir = std::getenv("KK_DEV_GFX_QUICK"); dir && *dir) {
      if (!InPlay() || DevSecondsSinceMenu() < 32) return;
      struct Phase { const char* name; const char* up; const char* bias; const char* effects;
                     const char* sharp; const char* aniso; const char* aa; const char* rcas; };
      static const Phase phases[] = {
        {"baseline", "dlss", "-0.585", "false", "off", "4", "none", "-1"},
        {"mip-dlss", "dlss", "auto", "false", "off", "4", "none", "-1"},
        {"effects", "dlss", "-0.585", "true", "off", "4", "none", "-1"},
        {"cas-low", "dlss", "-0.585", "false", "low", "4", "none", "-1"},
        {"cas-medium", "dlss", "-0.585", "false", "medium", "4", "none", "-1"},
        {"cas-high", "dlss", "-0.585", "false", "high", "4", "none", "-1"},
        {"aniso16", "dlss", "-0.585", "false", "off", "5", "none", "-1"},
        {"fsr-before", "fsr3", "-0.585", "false", "off", "4", "none", "-1"},
        {"mip-fsr", "fsr3", "auto", "false", "off", "4", "none", "-1"},
        {"fsr-rcas", "fsr3", "auto", "false", "off", "4", "none", "0.5"},
        {"fsr-rcas-cas", "fsr3", "auto", "false", "high", "4", "none", "0.5"},
        {"xess-before", "xess", "0", "false", "off", "4", "none", "-1"},
        {"mip-xess", "xess", "auto", "false", "off", "4", "none", "-1"},
        {"fxaa-before", "off", "auto", "false", "off", "4", "fxaa", "-1"},
        {"fxaa-cas", "off", "auto", "false", "medium", "4", "fxaa", "-1"},
      };
      static auto start = std::chrono::steady_clock::now();
      static auto last_tick = start;
      static int phase = -1;
      static int shots = 0;
      const auto tick = std::chrono::steady_clock::now();
      // A cache rebuild or a capture may stall a frame. Give the new value its full warmup
      // and keep the three shots apart instead of catching them all up after the stall.
      if (tick - last_tick > std::chrono::milliseconds(250))
        start += tick - last_tick - std::chrono::milliseconds(17);
      last_tick = tick;
      const double elapsed = std::chrono::duration<double>(tick - start).count();
      const int next = int(elapsed / 6);
      if (next >= int(std::size(phases))) {
        if (phase != next) REXLOG_INFO("KK quick wins: done");
        phase = next;
        return;
      }
      const Phase& p = phases[next];
      if (next != phase) {
        phase = next; shots = 0;
        Change("native_upscaler", p.up);
        Change("native_upscaler_quality", "quality");
        Change("native_upscaler_mip_bias", p.bias);
        Change("native_effect_buffers", p.effects);
        Change("native_sharpness", p.sharp);
        Change("anisotropic_override", p.aniso);
        Change("swap_post_effect", p.aa);
        Change("native_upscaler_sharpness", p.rcas);
        REXLOG_INFO("KK quick wins: phase {} {} begin frame {}", next, p.name, Stats().frame);
      }
      if (elapsed - next * 6 >= 4.0 + shots * 0.5 && shots < 3) {
        const std::filesystem::path root(dir);
        std::filesystem::create_directories(root);
        const auto stem = std::string(p.name) + "-" + std::to_string(shots++);
        art::CaptureFrame(root / (stem + art::kDevCaptureExtension));
        const auto s = Stats();
        std::ofstream(root / (stem + ".txt")) << SettingsText() << "\nframe=" << s.frame
            << "\ninput=" << s.input_width << 'x' << s.input_height << "\noutput="
            << s.output_width << 'x' << s.output_height << "\nbias=" << s.mip_bias
            << "\nupscaler=" << s.upscaler << "\npreparing=" << s.preparing << '\n';
        REXLOG_INFO("KK quick wins: capture {} frame {} bias {} upscaler {}", stem, s.frame, s.mip_bias, s.upscaler);
      }
      return;
    }
    // Task 123: fixed-view GI distance/cost comparisons. Guest frames only;
    // GPU times are the production timer's recent-frame average, not UI CPU time.
    if (const char* dir = std::getenv("KK_DEV_GI_DISTANCE_TEST"); dir && *dir) {
      if (!InPlay() || DevSecondsSinceMenu() < 8) return;
      static const auto start = std::chrono::steady_clock::now();
      static int previous = -1;
      static bool captured = false;
      static double next_sample = 0;
      const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
      const char* levels[] = {"off","low","low","medium","medium","high","high","high","high"};
      const char* distances[] = {"full","full","near","full","medium","full","far","near","full"};
      const int phase = int(elapsed / 12);
      const std::filesystem::path root(dir);
      if (phase >= 9) {
        if (previous != 9) REXLOG_INFO("KK GI distance test: done");
        previous = 9; return;
      }
      if (phase != previous) {
        std::filesystem::create_directories(root);
        Change("native_gi",levels[phase]); Change("native_gi_distance",distances[phase]);
        REXLOG_INFO("KK GI distance test: phase {} {} {} at {:.3f}",phase,levels[phase],distances[phase],elapsed);
        if (previous < 0) std::ofstream(root/"gpu.csv") << "seconds,phase,level,distance,gi_ms,input_width,input_height,output_width,output_height,api,applied\n";
        captured = false; previous = phase;
      }
      if (elapsed >= next_sample) {
        const auto s = nr::kk::EffectTimes();
        std::ofstream(root/"gpu.csv",std::ios::app) << elapsed << ',' << phase << ',' << levels[phase] << ',' << distances[phase]
            << ',' << s.gi_ms << ',' << s.input_width << ',' << s.input_height << ',' << s.output_width << ','
            << s.output_height << ',' << s.api << ',' << s.applied << '\n';
        next_sample = elapsed + 0.2;
      }
      if (!captured && elapsed-phase*12 >= 8) {
        const std::string name = std::to_string(phase)+"-"+levels[phase]+"-"+distances[phase];
        art::CaptureFrame(root/(name+art::kDevCaptureExtension));
        std::ofstream(root/(name+".txt")) << SettingsText();
        captured = true;
      }
      return;
    }
    // Task 072's one-run Off/4/16 comparison, through the same live changes as
    // the F8 controls. Captures the game's frame only, in a private test folder.
    if (const char* dir=std::getenv("KK_DEV_TESS_BENCH"); dir && *dir) {
      if (!InPlay()) return;
      const double seconds=DevSecondsSinceMenu();
      if (seconds<8) return;
      static int phase=-1;
      static bool captured=false;
      const int next=std::min(3,int((seconds-8)/16));
      if(next!=phase) {
        phase=next; captured=false;
        Change("kk_frame_rate","0");
        Change("native_tessellation",phase==1||phase==2?"true":"false");
        Change("native_tessellation_smoothness","0.75");
        Change("native_tessellation_detail",phase==2?"16":"4");
        REXLOG_INFO("KK tess test: phase {} at {:.3f} seconds, frame cap {}",phase,seconds,Get("kk_frame_rate"));
      }
      if(phase<3 && !captured && seconds>=8+phase*16+12) {
        std::filesystem::path root(dir); std::filesystem::create_directories(root);
        const char* names[]={"off","4","16"};
        art::CaptureFrame(root/(std::string("ann-")+names[phase]+art::kDevCaptureExtension));
        std::ofstream(root/(std::string("ann-")+names[phase]+".txt"))<<SettingsText();
        REXLOG_INFO("KK tess test: capture {}",names[phase]); captured=true;
      }
      return;
    }
    if (test_state_ < 0) {
      const char* dir = std::getenv("KK_DEV_PANEL_TEST");
      if (!dir || !*dir) {
        test_state_ = 1000;
        return;
      }
      test_dir_ = dir;
      test_state_ = 0;
    }
    if (test_state_ >= 1000 || !InPlay()) return;
    // The game's seconds until the test starts, then the wall clock's (KK_DEV_FORCE_DT can stop the game's).
    const double start = std::getenv("KK_DEV_PANEL_START") ? std::atof(std::getenv("KK_DEV_PANEL_START")) : 8.0;
    if (test_wall0_ == std::chrono::steady_clock::time_point{}) {
      if (DevSecondsSinceMenu() < start) return;
      test_wall0_ = std::chrono::steady_clock::now();
    }
    const double now = start + std::chrono::duration<double>(std::chrono::steady_clock::now() - test_wall0_).count();
    if (now < test_next_) return;
    const bool verify = std::getenv("KK_DEV_PANEL_VERIFY") != nullptr;
    const int toggles = std::getenv("KK_DEV_PANEL_TOGGLES") ? std::atoi(std::getenv("KK_DEV_PANEL_TOGGLES")) : 100;
    const double interval = std::getenv("KK_DEV_PANEL_INTERVAL") ? std::atof(std::getenv("KK_DEV_PANEL_INTERVAL")) : 0.1;
    auto log = [&](const std::string& text) { REXLOG_INFO("KK panel test: {} at {:.3f}", text, now); };
    auto check = [&](bool ok, const std::string& what) {
      ++(ok ? test_pass_ : test_fail_);
      log(std::string(ok ? "PASS " : "FAIL ") + what);
    };
    switch (test_state_) {
      case 0: {
        std::ifstream f(g_settings_path, std::ios::binary);
        test_config_ = std::string((std::istreambuf_iterator<char>(f)), {});
        SetOpen(true);
        plan_ = TestPlan();
        log("start: " + std::to_string(plan_.size()) + " steps, " + std::to_string(toggles) + " toggles each, every " +
            Number(interval, "%.3f") + " s" + (verify ? " (verify: a capture of each value)" : ""));
        test_state_ = verify ? 100 : 1;
        test_next_ = now + 3;
        return;
      }
      case 1: {  // a step: its upscaler, then the "before" capture
        if (step_ >= plan_.size()) {
          test_state_ = 10;
          return;
        }
        const TestStep& st = plan_[step_];
        if (st.cvar.rfind("#enable ", 0) == 0) {
          Change(st.cvar.substr(8).c_str(), st.values[0]);
          log("enable " + st.cvar.substr(8) + "=" + st.values[0]);
          ++step_;
          test_next_ = now + 2;
          return;
        }
        if (!st.upscaler.empty() && Get("native_upscaler") != st.upscaler) Change("native_upscaler", st.upscaler);
        test_state_ = 2;
        test_next_ = now + 1.5;
        return;
      }
      case 2:
        TestCapture(std::to_string(step_) + "-" + plan_[step_].cvar + "-before");
        toggle_ = 0;
        test_state_ = 3;
        test_next_ = now + 0.5;
        log("step " + std::to_string(step_) + " " + plan_[step_].cvar);
        return;
      case 3: {  // the toggles
        const TestStep& st = plan_[step_];
        if (toggle_ < toggles) {
          const std::string& v = st.values[size_t(toggle_) % st.values.size()];
          Change(st.cvar.c_str(), v);
          ++toggle_;
          test_next_ = now + interval;
          if (toggle_ % 25 == 0) log(st.cvar + " toggle " + std::to_string(toggle_) + " = " + v);
          return;
        }
        Change(st.cvar.c_str(), st.values.back());
        test_state_ = 4;
        test_next_ = now + 2;
        return;
      }
      case 4: {
        const TestStep& st = plan_[step_];
        const std::string got = Get(st.cvar.c_str());
        char* end = nullptr;
        const double a = std::strtod(got.c_str(), &end);
        const bool numeric = end && *end == 0 && !got.empty();
        const bool same = got == st.values.back() ||
                          (numeric && std::fabs(a - std::atof(st.values.back().c_str())) < 1e-3);
        check(same, st.cvar + " holds its last value " + st.values.back());
        TestCapture(std::to_string(step_) + "-" + st.cvar + "-after");
        test_state_ = 5;
        test_next_ = now + 0.5;
        return;
      }
      case 5:
        Reset();
        ++step_;
        test_state_ = 1;
        test_next_ = now + 1;
        return;
      case 10: {  // A/B: trial settings, hold, let go
        test_orig_ = {{"native_gi", Get("native_gi")}, {"native_shadow_quality", Get("native_shadow_quality")},
                      {"ao_mode", Get("ao_mode")}};
        Change("native_gi", "high");
        Change("native_shadow_quality", "ultra");
        Change("ao_mode", Get("ao_mode") == "1" ? "0" : "1");
        test_ab_ = {{"native_gi", Get("native_gi")}, {"native_shadow_quality", Get("native_shadow_quality")},
                    {"ao_mode", Get("ao_mode")}};
        test_state_ = 11;
        test_next_ = now + 3;
        return;
      }
      case 11:
        TestCapture("ab-trial");
        StartAb();
        test_state_ = 12;
        test_next_ = now + 3;
        return;
      case 12: {
        bool saved = true;
        {
          std::lock_guard lock(g_mutex);
          for (const auto& [name, value] : g_saved) saved = saved && Get(name.c_str()) == value;
        }
        check(saved, "A/B: F7 held shows every saved value");
        TestCapture("ab-saved");
        EndAb();
        test_state_ = 13;
        test_next_ = now + 3;
        return;
      }
      case 13: {
        bool trial = true;
        for (const auto& [name, value] : test_ab_) trial = trial && Get(name.c_str()) == value;
        check(trial, "A/B: letting go of F7 brings the trial values back");
        TestCapture("ab-trial-again");
        // An ordinary save during a trial keeps the saved values in the file.
        SaveSettings(g_settings_path);
        std::ifstream f(g_settings_path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), {});
        check(text.find("native_shadow_quality = \"ultra\"") == std::string::npos,
              "an ordinary save during a trial does not write it");
        Capture();
        test_state_ = 14;
        test_next_ = now + 3;
        return;
      }
      case 14: {
        std::error_code ec;
        const bool png = std::filesystem::exists(last_capture_, ec);
        auto txt = last_capture_;
        txt.replace_extension(".txt");
        std::ifstream f(txt, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), {});
        check(png, "the capture key wrote " + last_capture_.filename().string());
        check(text.find(std::string(T("Global illumination")) + " / " + T("Global illumination") + ": " + T("High") + " *") != std::string::npos,
              "the capture's text lists the trial settings");
        Reset();
        bool back = true;
        for (const auto& [name, value] : test_orig_) back = back && Get(name.c_str()) == value;
        check(back && Changed().empty(), "Reset puts the saved settings back");
        Change("native_gi", "medium");
        check(SaveAsMine(), "Save as my settings");
        std::ifstream g(g_settings_path, std::ios::binary);
        const std::string saved((std::istreambuf_iterator<char>(g)), {});
        check(saved.find("native_gi = \"medium\"") != std::string::npos, "Save as my settings writes the panel's values");
        check(Changed().empty(), "after saving nothing is temporary");
        // The run's own settings file again.
        std::ofstream(g_settings_path, std::ios::binary | std::ios::trunc) << test_config_;
        test_state_ = 15;
        test_next_ = now + 1;
        return;
      }
      // The keys, through this listener as the window hands them over (the test window never has the
      // keyboard focus, so the OS sends it no keys): a trial on, then F8, F7 held, F6, compact.
      case 15:
        SetOpen(false);
        Change("native_gi", "high");
        Change("native_shadow_quality", "ultra");
        Key(rex::ui::VirtualKey::kF8, true);
        Key(rex::ui::VirtualKey::kF8, false);
        check(g_open.load() && test_key_handled_, "F8 opens the panel (the key is the panel's)");
        log("show panel");  // the harness takes a picture of the window
        test_state_ = 16;
        test_next_ = now + 3;
        return;
      case 16: {
        std::string saved_gi;
        {
          std::lock_guard lock(g_mutex);
          saved_gi = g_saved.count("native_gi") ? g_saved["native_gi"] : "";
        }
        Key(rex::ui::VirtualKey::kF7, true);
        check(g_ab && test_key_handled_ && !saved_gi.empty() && Get("native_gi") == saved_gi,
              "F7 held shows the saved settings");
        log("show f7");
        test_state_ = 17;
        test_next_ = now + 3;
        return;
      }
      case 17: {
        Key(rex::ui::VirtualKey::kF7, false);
        check(!g_ab && Get("native_gi") == "high", "letting go of F7 brings the trial back");
        const auto before = last_capture_;
        Key(rex::ui::VirtualKey::kF6, true);
        Key(rex::ui::VirtualKey::kF6, false);
        check(last_capture_ != before && test_key_handled_, "F6 captures");
        Key(rex::ui::VirtualKey::kA, true);
        check(!test_key_handled_, "other keys reach the game");
        Key(rex::ui::VirtualKey::kA, false);
        rex::cvar::SetFlagByName("kk_graphics_panel_compact", "true");
        log("show compact");
        test_state_ = 18;
        test_next_ = now + 3;
        return;
      }
      case 18:
        Key(rex::ui::VirtualKey::kF8, true);
        Key(rex::ui::VirtualKey::kF8, false);
        check(g_open.load() && !REXCVAR_GET(kk_graphics_panel_compact), "F8 from the compact readout opens the panel");
        Key(rex::ui::VirtualKey::kF8, true);
        Key(rex::ui::VirtualKey::kF8, false);
        check(!g_open.load(), "F8 again closes it");
        Reset();
        log("show closed");
        test_state_ = 19;
        test_next_ = now + 3;
        return;
      case 19:
        log("done: " + std::to_string(test_pass_) + " passed, " + std::to_string(test_fail_) + " failed");
        test_state_ = 1000;
        return;
      // ---- verify: a capture of each value against two of the step's own value (the noise), in a scene the
      // game's frozen clock keeps still (KK_DEV_FORCE_DT). tools/graphics-panel-verify.py compares them.
      case 100: {
        if (step_ >= plan_.size()) {
          test_state_ = 19;
          return;
        }
        const TestStep& st = plan_[step_];
        if (st.cvar.rfind("#enable ", 0) == 0) {
          Change(st.cvar.substr(8).c_str(), st.values[0]);
          ++step_;
          test_next_ = now + 3;
          return;
        }
        if (!st.upscaler.empty() && Get("native_upscaler") != st.upscaler) Change("native_upscaler", st.upscaler);
        test_state_ = 101;
        test_next_ = now + 3;
        return;
      }
      // Each value: once on and back (anything it makes is ready and kept), then three trials of the step's own
      // value, the value and its own value again, 0.8 s apart: the scene (rain, lightning, the idle camera)
      // changes little in that time, and the median of the three trials drops a lightning flash.
      case 101:
        step_base_ = Get(plan_[step_].cvar.c_str());
        value_ = 0;
        test_state_ = 103;
        return;
      case 103: {
        const TestStep& st = plan_[step_];
        std::vector<std::string> values;
        for (const auto& v : st.values)
          if (v != step_base_ && std::find(values.begin(), values.end(), v) == values.end()) values.push_back(v);
        if (value_ >= values.size()) {
          Reset();
          ++step_;
          test_state_ = 100;
          test_next_ = now + 1.5;
          return;
        }
        verify_value_ = values[value_];
        Change(st.cvar.c_str(), verify_value_);
        trial_ = 0;
        test_state_ = 120;
        test_next_ = now + 2.5;
        return;
      }
      case 120:
        Change(plan_[step_].cvar.c_str(), step_base_);
        test_state_ = 121;
        test_next_ = now + 1.5;
        return;
      case 121:
        TestCapture(TrialName("A"));
        Change(plan_[step_].cvar.c_str(), verify_value_);
        test_state_ = 122;
        test_next_ = now + 0.8;
        return;
      case 122: {
        TestCapture(TrialName("V"));
        if (trial_ == 0) {
          const NrEffectStats es = Stats();
          log("verify " + std::to_string(step_) + "-" + plan_[step_].cvar + "-V" + std::to_string(value_) + "-" +
              verify_value_ + " base=" + step_base_ + " upscaler=" + es.upscaler + " applied=" + AppliedText(es, nullptr) +
              " gpu ao=" + Number(es.ao_ms) + " gi=" + Number(es.gi_ms) + " shadows=" + Number(es.shadows_ms) +
              " water=" + Number(es.water_ms) + " shafts=" + Number(es.volumetric_ms) + " up=" + Number(es.upscaler_ms) +
              " shadow_draws=" + std::to_string(es.shadow_draws) + " water_draws=" + std::to_string(es.water_draws));
        }
        Change(plan_[step_].cvar.c_str(), step_base_);
        test_state_ = 123;
        test_next_ = now + 0.8;
        return;
      }
      case 123:
        TestCapture(TrialName("B"));
        if (++trial_ < 3) {
          test_state_ = 121;
          test_next_ = now + 0.3;
        } else {
          ++value_;
          test_state_ = 103;
          test_next_ = now + 0.5;
        }
        return;
      case 110:  // a burst of three captures (rain and particles differ in each; their median is the scene)
        TestCapture(burst_name_ + "-" + std::to_string(burst_count_));
        if (++burst_count_ >= 3) {
          test_state_ = burst_next_;
          test_next_ = now + 0.1;
        } else {
          test_next_ = now + 0.35;
        }
        return;
      default:
        return;
    }
  }
  // Background-safe input through the same listeners/bind registry as real keys.
  // No OS focus, raw window messages or changes to the player's input state.
  void UiPaintTick() {
    if (!std::getenv("KK_DEV_UIPAINT_TEST")) return;
    const double t = DevSecondsSinceMenu();
    struct Press { double at; rex::ui::VirtualKey key; bool down; };
    static const Press plan[] = {
        {74, rex::ui::VirtualKey::kF2, true}, {74.2, rex::ui::VirtualKey::kF2, false},
        {78, rex::ui::VirtualKey::kF2, true}, {78.2, rex::ui::VirtualKey::kF2, false},
        {94, rex::ui::VirtualKey::kF8, true}, {94.2, rex::ui::VirtualKey::kF8, false},
        {99, rex::ui::VirtualKey::kF7, true}, {102, rex::ui::VirtualKey::kF7, false},
        {104, rex::ui::VirtualKey::kF6, true}, {104.2, rex::ui::VirtualKey::kF6, false},
        {108, rex::ui::VirtualKey::kF8, true}, {108.2, rex::ui::VirtualKey::kF8, false}};
    static size_t next = 0;
    if (next < std::size(plan) && t >= plan[next].at) {
      const auto& p = plan[next++];
      Key(p.key, p.down);
      REXLOG_INFO("KK dev UI key: {} {} handled={} at {:.3f}", rex::ui::VirtualKeyToString(p.key),
                  p.down ? "down" : "up", test_key_handled_, t);
    }
  }
  void Key(rex::ui::VirtualKey key, bool down) {
    rex::ui::KeyEvent e(window_, key, 1, !down, false, false, false, false);
    if (down) OnKeyDown(e);
    else OnKeyUp(e);
    if (down && !e.is_handled()) rex::ui::ProcessKeyEvent(e);
    test_key_handled_ = e.is_handled();
  }
  std::string TrialName(const char* which) const {
    return std::to_string(step_) + "-" + plan_[step_].cvar + "-V" + std::to_string(value_) + "-" + verify_value_ + "-T" +
           std::to_string(trial_) + "-" + which;
  }
  int trial_ = 0;
  void Burst(const std::string& name, int next, double now) {
    burst_name_ = name;
    burst_count_ = 0;
    burst_next_ = next;
    test_state_ = 110;
    test_next_ = now;
  }
  std::string burst_name_;
  int burst_count_ = 0, burst_next_ = 0;
  bool test_key_handled_ = false;
  std::chrono::steady_clock::time_point test_wall0_{};
  std::string step_base_, verify_value_;
  size_t value_ = 0;
  int test_state_ = -1;
  double test_next_ = 0;
  std::filesystem::path test_dir_;
  std::string test_config_;
  std::vector<TestStep> plan_;
  size_t step_ = 0;
  int toggle_ = 0;
  int test_pass_ = 0, test_fail_ = 0;
  std::vector<std::pair<std::string, std::string>> test_ab_, test_orig_;
#endif

  rex::ui::Window* window_;
  std::filesystem::path user_root_;
  std::filesystem::path last_capture_;
  bool cursor_shown_ = false, nav_before_ = false, had_focus_ = false;
  std::map<int, bool> held_;
  std::string status_;
  std::chrono::steady_clock::time_point status_until_{};
  float bias_ = 0, column_ = 0;
  bool dragging_bias_ = false;
  ui::Input pad_input_{false};
  bool key_compare_ = false, pad_compare_ = false, pad_ready_ = false;
  bool pad_navigation_ = false, scroll_to_focus_ = false;
  int focus_row_ = 0;
  std::string focus_cvar_;
};

}  // namespace

bool DebugOverlayOpen() { return g_open.load(std::memory_order_relaxed) && !REXCVAR_GET(kk_graphics_panel_compact); }

bool DebugOverlayFilterPad(uint32_t user, uint8_t* state) {
  if (user >= 4) return false;
  auto s16 = [&](int at) { return int16_t(uint16_t(state[at] << 8 | state[at + 1])); };
  panel::PadState pad{uint16_t(state[4] << 8 | state[5]), state[6], state[7], s16(8), s16(10), s16(12), s16(14)};
  const bool in_play = play::SessionActive() && !play::CoverScreen() && !play::PauseMenuOpen();
  bool consumed, request;
  {
    std::lock_guard lock(g_pad_mutex);
#if defined(KK_DEV_TOOLS)
    if (user == 0 && in_play && g_controller_test_override) {
      pad = g_controller_test_pad;
      auto store16 = [&](int at, uint16_t value) { state[at] = uint8_t(value >> 8); state[at + 1] = uint8_t(value); };
      store16(4, pad.buttons);
      state[6] = pad.lt; state[7] = pad.rt;
      store16(8, uint16_t(pad.lx)); store16(10, uint16_t(pad.ly));
      store16(12, uint16_t(pad.rx)); store16(14, uint16_t(pad.ry));
    }
#endif
    const bool pending = g_pad[user].toggle;
    consumed = g_pad[user].Filter(pad, in_play && user == 0, DebugOverlayOpen());
    request = !pending && g_pad[user].toggle;
    if (user == 0) g_pad_at = std::chrono::steady_clock::now();
#if defined(KK_DEV_TOOLS)
    if (user == 0 && g_controller_test_override) {
      ++g_controller_test_samples;
      g_controller_test_consumed = consumed;
    }
#endif
  }
  if (request) RequestUiPaint();
  if (consumed) std::fill(state + 4, state + 16, uint8_t(0));
  return consumed;
}

void SetDebugOverlayConfigPath(const std::filesystem::path& config) { g_settings_path = config; }

void CreateDebugOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, const std::filesystem::path& config,
                        const std::filesystem::path& user_root) {
  static bool created = false;
  if (created || !window) return;
  created = true;
  if (!config.empty()) g_settings_path = config;
  new GraphicsPanel(drawer, window, user_root);  // lives for the rest of the session
}

}  // namespace kk
