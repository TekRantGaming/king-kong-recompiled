#include "launcher.h"
#include "handoff.h"
#include "startup_recovery.h"
#include "ingame_settings.h"
#include "pause_menu.h"
#include "play_session.h"
#include "benchmark.h"
#include "launcher_benchmark.h"
#include "launcher_art.h"
#include "launcher_music.h"
#include "launcher_sounds.h"
#include "launcher_text.h"
#include "launcher_ui.h"
#include "ps2_movies.h"
#include "shader_pack.h"
#include "native_shaders.h"
#include "display_modes.h"
#include "steam_shortcut.h"
#include "update.h"
#include "glyphs.h"
#include "discord_presence.h"
#include "online.h"
#include "diagnostics.h"
#include "dev_launcher_chain.h"
#include "hooks/kk_native_ui.h"  // the temporal upscalers this PC runs

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#else
#include <cstdio>
#include <dlfcn.h>
#include <sys/wait.h>
#endif

#include <imgui.h>
#include <SDL3/SDL_dialog.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>

#include "art.h"
#include "changelog.h"
#include "cheats.h"
#include "platform.h"
#include "settings.h"
#include "settings_layout.h"
#include "launcher_policy.h"
#include "graphics_presets.h"
#include "iso.h"
#include "toast.h"

REXCVAR_DEFINE_STRING(kk_launcher_open, "", "KK",
                      "Internal: the screen the launcher opens on, set by the game when it goes back to the "
                      "launcher: play (the Play menu) or chapters (Chapter select). Never saved.");

namespace kk {
namespace {

using text::F;
using text::T;
using ui::Action;
using ui::Item;
using ui::ItemType;
namespace color = ui::color;

constexpr uint32_t kTitleId = 0x555307D3;
struct TexturePackFolderChoice {
  std::mutex mutex;
  bool busy = false;
  std::string selected;
} texture_pack_folder_choice;
constexpr const char* kProjectUrl = "https://github.com/TekRantGaming/king-kong-recompiled";
constexpr const char* kDiscordUrl = "https://discord.gg/PET2xU5FJq";  // permanent (expires_at null)
#include "discord_icon.inc"
#include "steam_icon.inc"
constexpr const char* kOnlineAbout =
    "Shows how many players are online, at the top right of the launcher. To count you, the launcher sends a tiny "
    "anonymous check-in every 5 minutes with just four things: a random number that isn't linked to you, the "
    "port's version (stable or beta), Windows or Linux, and whether you're in the game. That's all. No name, "
    "account, IP address or anything from your PC is kept, and switching it off deletes the random number.";
#define KK_TIMES "\xC3\x97"
#define KK_DOT "\xC2\xB7"
#define KK_DEG "\xC2\xB0"
// Said wherever the native renderer's shaders are being prepared.
#define KK_SLOW_PC "Your PC may run slower until this finishes, because preparing the shaders uses its processor."

// ---------------------------------------------------------- cvar helpers ---
// Every change bumps g_changes; the launcher saves the settings shortly after.
int g_changes = 0;

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
bool GetBool(const char* name) { return Get(name) == "true"; }
int GetInt(const char* name, int fallback = 0) {
  try {
    return std::stoi(Get(name));
  } catch (...) {
    return fallback;
  }
}
float GetFloat(const char* name, float fallback) {
  try {
    return std::stof(Get(name));
  } catch (...) {
    return fallback;
  }
}
void Set(const char* name, const std::string& value) {
  if (Get(name) == value) return;
  rex::cvar::SetFlagByName(name, value);
  ++g_changes;
}
void SetBool(const char* name, bool v) { Set(name, v ? "true" : "false"); }
void SetInt(const char* name, int v) { Set(name, std::to_string(v)); }
void Reset(const char* name) {
  rex::cvar::ResetToDefault(name);
  ++g_changes;
}

// The pages are built every frame and look up many settings: by name through an index of the registry's
// positions (task 144: it searched the whole registry each time), checked against the name so a registry
// that grew or moved is searched again. UI thread only.
const rex::cvar::FlagEntry* FindFlag(std::string_view name) {
  static std::unordered_map<std::string, size_t> index;
  static size_t indexed_size = 0;
  auto& registry = rex::cvar::GetRegistry();
  if (registry.size() != indexed_size) {
    index.clear();
    for (size_t i = 0; i < registry.size(); ++i) index.emplace(registry[i].name, i);
    indexed_size = registry.size();
  }
  const auto it = index.find(std::string(name));
  if (it == index.end()) return nullptr;  // the index has every entry of a registry this size
  if (registry[it->second].name == name) return &registry[it->second];
  for (auto& e : registry)  // moved since it was indexed
    if (e.name == name) return &e;
  return nullptr;
}

std::string Upper(std::string s) { return text::Upper(std::move(s)); }

std::string Size(int w, int h) { return std::to_string(w) + " " KK_TIMES " " + std::to_string(h); }

// ImGuiKey -> Win32 virtual-key code (ReXGlue's VirtualKey uses the same values).
int ImGuiKeyToVk(ImGuiKey k) {
  if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return 'A' + (k - ImGuiKey_A);
  if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return '0' + (k - ImGuiKey_0);
  if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) return 0x70 + (k - ImGuiKey_F1);
  if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) return 0x60 + (k - ImGuiKey_Keypad0);
  switch (k) {
    case ImGuiKey_Tab: return 0x09;
    case ImGuiKey_LeftArrow: return 0x25;
    case ImGuiKey_RightArrow: return 0x27;
    case ImGuiKey_UpArrow: return 0x26;
    case ImGuiKey_DownArrow: return 0x28;
    case ImGuiKey_PageUp: return 0x21;
    case ImGuiKey_PageDown: return 0x22;
    case ImGuiKey_Home: return 0x24;
    case ImGuiKey_End: return 0x23;
    case ImGuiKey_Insert: return 0x2D;
    case ImGuiKey_Delete: return 0x2E;
    case ImGuiKey_Backspace: return 0x08;
    case ImGuiKey_Space: return 0x20;
    case ImGuiKey_Enter: case ImGuiKey_KeypadEnter: return 0x0D;
    case ImGuiKey_LeftShift: return 0xA0;
    case ImGuiKey_RightShift: return 0xA1;
    case ImGuiKey_LeftCtrl: return 0xA2;
    case ImGuiKey_RightCtrl: return 0xA3;
    case ImGuiKey_LeftAlt: return 0xA4;
    case ImGuiKey_RightAlt: return 0xA5;
    case ImGuiKey_Semicolon: return 0xBA;
    case ImGuiKey_Equal: return 0xBB;
    case ImGuiKey_Comma: return 0xBC;
    case ImGuiKey_Minus: return 0xBD;
    case ImGuiKey_Period: return 0xBE;
    case ImGuiKey_Slash: return 0xBF;
    case ImGuiKey_GraveAccent: return 0xC0;
    case ImGuiKey_LeftBracket: return 0xDB;
    case ImGuiKey_Backslash: return 0xDC;
    case ImGuiKey_RightBracket: return 0xDD;
    case ImGuiKey_Apostrophe: return 0xDE;
    case ImGuiKey_KeypadDecimal: return 0x6E;
    case ImGuiKey_KeypadDivide: return 0x6F;
    case ImGuiKey_KeypadMultiply: return 0x6A;
    case ImGuiKey_KeypadSubtract: return 0x6D;
    case ImGuiKey_KeypadAdd: return 0x6B;
    default: return 0;
  }
}

#if defined(_WIN32)
std::filesystem::path BrowseForDiscImage() {
  wchar_t file[MAX_PATH] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter = L"Xbox 360 disc image (*.iso)\0*.iso\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = L"Select your King Kong Xbox 360 disc image";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&ofn) ? std::filesystem::path(file) : std::filesystem::path();
}
#else
// Linux: the desktop's own file picker through zenity (GNOME, SteamOS and
// most others) or kdialog (KDE).
std::filesystem::path BrowseForDiscImage() {
  const char* commands[] = {
      "zenity --file-selection --title='Select your King Kong Xbox 360 disc image' "
      "--file-filter='Xbox 360 disc image (*.iso) | *.iso *.ISO' --file-filter='All files | *' 2>/dev/null",
      "kdialog --title 'Select your King Kong Xbox 360 disc image' --getopenfilename \"$HOME\" "
      "'*.iso *.ISO|Xbox 360 disc image' 2>/dev/null",
  };
  for (const char* cmd : commands) {
    FILE* pipe = popen(cmd, "r");
    if (!pipe) continue;
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    const int status = pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (!out.empty()) return std::filesystem::path(out);
    // 127: the tool isn't installed, so try the next one; anything else means
    // the player cancelled.
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 127) return {};
  }
  return {};
}
#endif

// The languages the game has (user_language, Xbox 360 language numbers), in
// their own names. The launcher's text follows the same setting.
constexpr std::pair<int, const char*> kLanguages[] = {
    {1, "English"}, {3, "Deutsch"}, {5, "Espa\xC3\xB1ol"}, {4, "Fran\xC3\xA7" "ais"}, {6, "Italiano"}};

// Settings that the presenter/window read before the launcher runs; changing
// them needs a relaunch to take effect.
// gpu_plugin: the renderer (Graphics > Renderer) is loaded before the launcher opens.
// native_upscaler: on Vulkan, DLSS and XeSS need the device made with their extensions (the native
// renderer asks for them when it makes the device, before the launcher opens).
// VSync: Vulkan chooses its present mode when the swapchain is created.
constexpr const char* kRestartCvars[] = {"present_effect", "window_width", "window_height", "monitor", "gpu_plugin",
                                         "native_upscaler", "native_backend",
                                         "d3d12_allow_variable_refresh_rate_and_tearing"};

// The launcher's pages (owner, 16 Oct: a tidy top level). Home is the main menu: Play (its own pages,
// PlayView, with Extras among them), Settings, Achievements and About. Settings has the six categories of
// settings_layout.h as tabs; Achievements and About are pages of their own.
enum Page { kHome, kDisplay, kGraphics, kAudio, kControls, kGameplay, kAdvanced, kAchievements, kAbout, kPageCount };
constexpr const char* kPageNames[kPageCount] = {"Home", "Display", "Graphics", "Audio", "Controls", "Gameplay",
                                               "Advanced", "Achievements", "About"};
constexpr Page kFirstSettingsPage = kDisplay, kLastSettingsPage = kAdvanced;
constexpr bool IsSettingsPage(Page page) { return page >= kFirstSettingsPage && page <= kLastSettingsPage; }
static_assert(kLastSettingsPage - kFirstSettingsPage + 1 == settings_layout::kPageCount);
// Sub-pages of Controls.
enum SubPage { kNoSubPage, kRemapPage, kKeysPage };
// Play on Home: the Play menu and its pages (docs/menus.md).
enum class PlayView { kNone, kMenu, kLoad, kNew, kChapters, kExtras };

struct Achievement {
  uint32_t id = 0;
  std::string label, description, unachieved;
  uint32_t gamerscore = 0;
  bool unlocked = false;
  rex::ui::ImmediateTexture* icon = nullptr;
};

struct Option {
  std::string label;
  std::string value;
};

// --------------------------------------------------- temporal upscalers ---
// The native renderer's temporal upscalers (native_upscaler; native-renderer/docs/upscalers.md), on
// Display > Upscaling beside FSR 1 and NIS: only with the native renderer, and only those this PC's
// graphics card runs with the graphics API the game uses (asked once, in the background:
// nr::kk::QueryUpscalers). DLSS is named in text only: no NVIDIA logos or artwork (their use needs
// NVIDIA's approval). No frame generation. The default is the best one the card runs (settings.h,
// ApplyUpscalerDefaults); the Original look keeps them off.
struct TemporalUpscaler {
  const char* label;
  const char* value;  // native_upscaler
};
constexpr TemporalUpscaler kTemporalUpscalers[] = {
    {"NVIDIA DLSS", "dlss"}, {"AMD FSR 4", "fsr4"}, {"AMD FSR 3.1", "fsr3"}, {"Intel XeSS", "xess"}};
// Their modes (native_upscaler_quality) and how many times fewer pixels a side each draws.
struct TemporalMode {
  const char* label;
  const char* value;
  double ratio;
};
constexpr TemporalMode kTemporalModes[] = {{"Native AA", "native", 1.0},
                                           {"Quality", "quality", 1.5},
                                           {"Balanced", "balanced", 1.7},
                                           {"Performance", "performance", 2.0},
                                           {"Ultra Performance", "ultra_performance", 3.0}};

bool UsingNativeRenderer();
bool HasTemporalSetting() { return FindFlag("native_upscaler") != nullptr; }
// The temporal upscaler in use ("" for none: off, or not the native renderer).
std::string CurrentTemporalUpscaler() {
  if (!HasTemporalSetting() || !UsingNativeRenderer()) return {};
  const std::string v = Get("native_upscaler");
  return v == "off" || v.empty() ? std::string() : v;
}
void SetTemporalUpscaler(const char* value) {
  if (HasTemporalSetting()) Set("native_upscaler", value);
}
// Which of them this PC runs (none until the answer is in).
struct OfferedUpscalers {
  bool known = false, fsr3 = false, fsr4 = false, dlss = false, xess = false;
  bool Has(std::string_view v) const {
    return (v == "fsr3" && fsr3) || (v == "fsr4" && fsr4) || (v == "dlss" && dlss) || (v == "xess" && xess);
  }
};
OfferedUpscalers OfferedTemporalUpscalers() {
  OfferedUpscalers o;
  if (!HasTemporalSetting() || !UsingNativeRenderer()) return o;
  const nr::kk::UpscalerChoices c = nr::kk::QueryUpscalers();
  o.known = c.state == 2;
  o.fsr3 = c.fsr3;
  o.fsr4 = c.fsr4;
  o.dlss = c.dlss;
  o.xess = c.xess;
  return o;
}

// ------------------------------------------------------------ presets ---
// Graphics presets set the upscaler, render quality, anti-aliasing, texture
// filtering, ambient occlusion and GI (motion blur and fog are left to taste).
// The preset shown is the one the settings match; anything else is Custom.
// Medium is the default settings (the default upscaler at Quality).

// The upscaler modes (kk_render_quality); -1 for another render quality.
constexpr const char* kUpscalerModes[] = {"native", "quality", "balanced", "performance"};
constexpr const char* kUpscalerModeNames[] = {"Native", "Quality", "Balanced", "Performance"};
int UpscalerMode() {
  const std::string q = Get("kk_render_quality");
  for (int i = 0; i < 4; ++i)
    if (q == kUpscalerModes[i]) return i;
  return -1;
}

// Whether the game's Vulkan device, made for the temporal upscaler `before`, lacks what `after` needs
// (native-renderer/plugin/native_graphics_system.cpp, PrepareVulkanForUpscalers).
bool UpscalerNeedsNewDevice(const std::string& before, const std::string& after) {
  auto needs = [](const std::string& v, bool vendor_dlss, bool vendor_xess) {
    return std::pair{v == "dlss" || (v == "auto" && vendor_dlss), v == "xess" || (v == "auto" && vendor_xess)};
  };
  const GpuVendor vendor = DetectGpu().vendor;
  const auto has = needs(before, vendor == GpuVendor::kNvidia, vendor == GpuVendor::kIntel);
  const auto want = needs(after, false, false);
  return (want.first && !has.first) || (want.second && !has.second);
}

// A setting at its default (a missing one counts as at its default).
bool AtDefault(const char* name) {
  const auto* flag = FindFlag(name);
  return !flag || Get(name) == flag->default_value;
}

// The preset's upscaler: the default one (present_effect and native_upscaler at their defaults), and with a
// temporal one, the preset's mode.
bool PresetUpscalerMatches(const GraphicsPreset& p) {
  if (!AtDefault("present_effect") || !AtDefault("native_upscaler")) return false;
  return CurrentTemporalUpscaler().empty() || Get("native_upscaler_quality") == p.temporal;
}

int MatchGraphicsPreset() {
  return FindGraphicsPreset(Get, PresetUpscalerMatches);
}

void ApplyGraphicsPreset(int index) {
  const GraphicsPreset& p = kGraphicsPresets[index];
  // The default upscaler (the best temporal one this PC runs, else FSR 1) and the preset's mode for it.
  Reset("present_effect");
  if (HasTemporalSetting()) {
    Reset("native_upscaler");
    Set("native_upscaler_quality", p.temporal);
  }
  ApplyGraphicsPresetSettings(p, Set);
}

// The Original look: the Xbox 360's own settings (OriginalLookSettings).
// Switching to it keeps the Modern ones in kk_modern_settings, and switching
// back puts them back.
void SetOriginalLook(bool on) {
  if (on == GetBool("kk_original_look")) return;
  if (on) {
    std::string saved;
    for (const auto& [name, value] : OriginalLookSettings()) saved += std::string(name) + "=" + Get(name) + ";";
    Set("kk_modern_settings", saved);
    for (const auto& [name, value] : OriginalLookSettings()) Set(name, value);
  } else {
    const std::string saved = Get("kk_modern_settings");
    if (saved.empty()) {  // no saved Modern choices: use the hardware defaults
      SetBool("kk_original_look", false);
      Reset("native_gi");  // the Original look's Off is not a Modern choice
      const auto output = PrimaryScreenSize();
      ApplyHardwareRecommendation(true, output.first, output.second);
      SetInt("kk_frame_rate", 60);  // Remastered plays at 60 FPS (the owner's choice); Original keeps 30
    }
    std::stringstream in(saved);
    for (std::string item; std::getline(in, item, ';');) {
      const size_t eq = item.find('=');
      if (eq != std::string::npos) Set(item.substr(0, eq).c_str(), item.substr(eq + 1));
    }
    Set("kk_modern_settings", "");
  }
  SetBool("kk_original_look", on);
}

// ------------------------------------------------------------- renderer ---
// Graphics > Renderer: kk_renderer, which gpu_plugin follows (ApplyRendererChoice). A gpu_plugin in
// the settings file (an earlier test build's) is taken out so that the choice holds.
bool UsingNativeRenderer() { return Get("gpu_plugin") == "native"; }

// ------------------------------------------------------- graphics style ---
// The setup's last step: Original (the Original look) or Remastered (the Modern look, named
// Remastered on the Graphics page, with a preset for this graphics card and the native renderer's
// GTAO ambient occlusion). Both keep the native renderer. native-renderer/docs/beta.md.
void ApplyRemastered() {
  SetOriginalLook(false);
  const auto output = PrimaryScreenSize();
  ApplyHardwareRecommendation(true, output.first, output.second);
  SetInt("ao_mode", 1);
  SetInt("ao_strength", 3);  // Strong: GTAO's default since Beta 3 (the owner's choice)
  SetInt("kk_frame_rate", 60);  // Remastered plays at 60 FPS (the owner's choice); Original keeps 30
  ++g_changes;
  ApplyRendererChoice();
}

// The Remastered settings first: the Original look keeps the settings it replaces and puts them back
// when Look is switched to Remastered on the Graphics page, which then gives exactly Remastered.
void ApplyOriginal() {
  ApplyRemastered();
  SetOriginalLook(true);
}

// Settings only this beta has (the native renderer's, the renderer choice, the fullscreen
// resolution): put back to their defaults before switching to stable, which has none of them.
// gpu_plugin is never saved by the beta (ApplyRendererChoice), but one from an earlier test build's
// settings file goes too. Saves, the stable settings and everything else stay.
void ResetBetaOnlySettings() {
  for (auto& e : rex::cvar::GetRegistry()) {
    if (e.type == rex::cvar::FlagType::Command) continue;
    if (e.source == rex::cvar::Source::kCommandLine || e.source == rex::cvar::Source::kEnvironment) continue;
    const bool beta = e.name.rfind("native_", 0) == 0 || e.category == "Native renderer" || e.name == "kk_renderer" ||
                      e.name == "kk_fullscreen_mode" || e.name == "gpu_plugin";
    if (beta && e.getter() != e.default_value) REXLOG_INFO("KK: switching to stable: {} back to its default", e.name);
    if (beta) rex::cvar::ResetToDefault(e.name);
  }
  ++g_changes;
}

// ----------------------------------------------------------- backdrop ---
// A box blur of the colour channels, `passes` times (close to a Gaussian).
void Blur(std::vector<uint8_t>& px, int w, int h, int r, int passes) {
  std::vector<uint8_t> tmp(px.size());
  auto pass = [&](const uint8_t* src, uint8_t* dst, bool horizontal) {
    const int n = horizontal ? w : h, lines = horizontal ? h : w;
    const int stride = horizontal ? 4 : w * 4, line_stride = horizontal ? w * 4 : 4;
    for (int l = 0; l < lines; ++l) {
      const uint8_t* s = src + size_t(l) * line_stride;
      uint8_t* d = dst + size_t(l) * line_stride;
      for (int c = 0; c < 3; ++c) {
        int sum = 0;
        for (int k = -r; k <= r; ++k) sum += s[std::clamp(k, 0, n - 1) * stride + c];
        for (int i = 0; i < n; ++i) {
          d[i * stride + c] = uint8_t(sum / (2 * r + 1));
          sum += s[std::min(i + r + 1, n - 1) * stride + c] - s[std::max(i - r, 0) * stride + c];
        }
      }
      for (int i = 0; i < n; ++i) d[i * stride + 3] = 255;
    }
  };
  for (int p = 0; p < passes; ++p) {
    pass(px.data(), tmp.data(), true);
    pass(tmp.data(), px.data(), false);
  }
}

// The launcher's backdrop from the captured menu screen: the part right of the
// game's logo (moon, cliffs and the sea), 16:9, with the save message's box
// in the middle painted out. `soft` is a blurred copy for behind the settings.
void MakeBackdrops(const art::Image& src, art::Image* sharp, art::Image* soft) {
  const int sw = src.width, sh = src.height;
  const int x0 = int(float(sw) * 0.30f), cw = sw - x0;
  const int ch = std::min(sh, cw * 9 / 16);
  const int y0 = std::clamp(int(float(sh) * 0.15f), 0, sh - ch);
  art::Image out;
  out.width = cw;
  out.height = ch;
  out.rgba.resize(size_t(cw) * ch * 4);
  for (int y = 0; y < ch; ++y)
    std::memcpy(&out.rgba[size_t(y) * cw * 4], &src.rgba[(size_t(y0 + y) * sw + x0) * 4], size_t(cw) * 4);
  // The box (its width depends on the language): fill its rows with a blend
  // of the rows just above and below it.
  const int ya = std::clamp(int(float(sh) * 0.40f) - y0, 1, ch - 3);
  const int yb = std::clamp(int(float(sh) * 0.545f) - y0, ya + 1, ch - 2);
  const int xb = std::clamp(int(float(sw) * 0.80f) - x0, 0, cw);
  auto px = [&](std::vector<uint8_t>& v, int x, int y) { return &v[(size_t(y) * cw + x) * 4]; };
  for (int x = 0; x < xb; ++x) {
    const uint8_t* above = px(out.rgba, x, ya - 1);
    const uint8_t* below = px(out.rgba, x, yb + 1);
    for (int y = ya; y <= yb; ++y) {
      const float t = float(y - ya + 1) / float(yb - ya + 2);
      uint8_t* d = px(out.rgba, x, y);
      for (int c = 0; c < 3; ++c) d[c] = uint8_t(float(above[c]) + (float(below[c]) - float(above[c])) * t);
      d[3] = 255;
    }
  }
  // Soften the painted area into its surroundings.
  art::Image blurred = out;
  Blur(blurred.rgba, cw, ch, std::max(2, cw / 110), 3);
  const float feather = float(ch) / 12.0f;
  for (int y = 0; y < ch; ++y) {
    const float dy = y < ya ? float(ya - y) : y > yb ? float(y - yb) : 0.0f;
    if (dy > feather) continue;
    for (int x = 0; x < cw; ++x) {
      const float dx = x >= xb ? float(x - xb + 1) : 0.0f;
      const float d = std::sqrt(dx * dx + dy * dy);
      if (d > feather) continue;
      const float t = 1.0f - d / feather;
      const float k = t * t * (3 - 2 * t);
      uint8_t* o = px(out.rgba, x, y);
      const uint8_t* b = px(blurred.rgba, x, y);
      for (int c = 0; c < 3; ++c) o[c] = uint8_t(float(o[c]) + (float(b[c]) - float(o[c])) * k);
    }
  }
  Blur(blurred.rgba, cw, ch, std::max(3, cw / 70), 3);
  *sharp = std::move(out);
  *soft = std::move(blurred);
}

// A copy `step` times smaller (each pixel the average of step x step).
art::Image Shrink(const art::Image& src, int step) {
  const int kStep = step;
  art::Image out;
  out.width = src.width / kStep;
  out.height = src.height / kStep;
  if (!out) return out;
  out.rgba.resize(size_t(out.width) * out.height * 4);
  for (int y = 0; y < out.height; ++y)
    for (int x = 0; x < out.width; ++x) {
      int sum[3] = {};
      for (int dy = 0; dy < kStep; ++dy)
        for (int dx = 0; dx < kStep; ++dx) {
          const uint8_t* p = &src.rgba[(size_t(y * kStep + dy) * src.width + x * kStep + dx) * 4];
          for (int c = 0; c < 3; ++c) sum[c] += p[c];
        }
      uint8_t* d = &out.rgba[(size_t(y) * out.width + x) * 4];
      for (int c = 0; c < 3; ++c) d[c] = uint8_t(sum[c] / (kStep * kStep));
      d[3] = 255;
    }
  return out;
}

// A small, blurred copy of a backdrop, for behind the settings.
art::Image SoftCopy(const art::Image& src) {
  art::Image out = Shrink(src, 4);
  if (out) Blur(out.rgba, out.width, out.height, std::max(2, out.width / 100), 3);
  return out;
}

// Add to Steam, shared with its worker threads.
struct SteamState {
  std::atomic<bool> available{false};   // Steam is here, and this copy wasn't started from it
  std::atomic<bool> in_library{false};  // every account has it already
  std::atomic<bool> busy{false}, done{false};
  std::atomic<int> phase{0};  // kSteamPhases
  steam::Result result;       // read once done
  bool reopened = false;
};
constexpr const char* kSteamPhases[] = {"Downloading the artwork from SteamGridDB", "Waiting for Steam to close",
                                        "Adding King Kong to your library", "Opening Steam again"};

// The settings pages (including the game's own audio and subtitle options, kept in its saves): their rows, built by the
// launcher and, in game, by the pause menu (InGameSettings below, ingame_settings.h), which leaves
// out the rows that need a restart. The launcher is one of these.
class SettingsPages {
 protected:
  SettingsPages(rex::ui::ImmediateDrawer* immediate, LauncherCallbacks callbacks, bool in_game,
                std::filesystem::path user_dir)
      : cb_(std::move(callbacks)),
        input_(/*own_gamepads=*/!in_game),
        glyphs_(immediate, rex::filesystem::GetExecutableFolder() / "glyphs"),
        in_game_(in_game), profile_user_dir_(std::move(user_dir)) {}

  // The button pictures the Button prompts setting picks, as in the game.
  std::string PadSet() const {
    const std::string p = Get("kk_button_prompts");
    if (p == "auto") return input_.pad_style().empty() ? DetectedPadPrompts() : input_.pad_style();
    if (p == "xbox360" || p == "xbox_series" || p == "ps5" || p == "ps2" || p == "steam_deck" || p == "keyboard")
      return p;
    return input_.pad_device() == ui::Device::kPlayStation ? "ps5" : "xbox_series";
  }
  // The prompts' pictures: keys while the keyboard and mouse are in use.
  std::string PromptSet() const { return input_.device() == ui::Device::kKeyboard ? "keyboard" : PadSet(); }

  void Status(std::string text) {
    status_ = std::move(text);
    status_time_ = ImGui::GetTime();
  }

  // Item builders.
  static Item Section(const std::string& label) {
    Item it;
    it.type = ItemType::kSection;
    it.label = label;  // translated and upper-cased as it's drawn
    return it;
  }

  // A choice bound to a cvar's values; values the cvar refuses in this build
  // are left out.
  static Item CvarChoice(std::string label, std::string desc, std::string cvar, std::vector<Option> options) {
    if (const auto* flag = FindFlag(cvar); flag && !flag->constraints.allowed_values.empty()) {
      const auto& allowed = flag->constraints.allowed_values;
      std::erase_if(options, [&](const Option& o) {
        return std::find(allowed.begin(), allowed.end(), o.value) == allowed.end();
      });
    }
    Item it;
    it.type = ItemType::kChoice;
    it.label = std::move(label);
    it.description = std::move(desc);
    const std::string cur = Get(cvar.c_str());
    it.index = -1;
    it.value_text = cur;
    std::vector<std::string> values;
    for (size_t i = 0; i < options.size(); ++i) {
      it.options.push_back(options[i].label);
      values.push_back(options[i].value);
      if (options[i].value == cur) it.index = int(i);
    }
    it.on_choice = [cvar, values](int i) { Set(cvar.c_str(), values[size_t(i)]); };
    it.on_default = [cvar] { Reset(cvar.c_str()); };
    if (const auto* flag = FindFlag(cvar))
      for (const Option& o : options)
        if (o.value == flag->default_value) it.default_text = o.label;
    return it;
  }

  static Item CvarToggle(std::string label, std::string desc, std::string cvar, const char* off = "Off",
                         const char* on = "On", bool invert = false) {
    return CvarChoice(std::move(label), std::move(desc), std::move(cvar),
                      {{off, invert ? "true" : "false"}, {on, invert ? "false" : "true"}});
  }

  static Item CvarSlider(std::string label, std::string desc, std::string cvar, float lo, float hi, float step,
                         std::function<std::string(float)> format) {
    Item it;
    it.type = ItemType::kSlider;
    it.label = std::move(label);
    it.description = std::move(desc);
    it.lo = lo;
    it.hi = hi;
    it.step = step;
    it.value = std::clamp(GetFloat(cvar.c_str(), lo), lo, hi);
    it.value_text = format(it.value);
    const bool integral = step >= 1.0f;
    it.on_value = [cvar, integral](float v) {
      Set(cvar.c_str(), integral ? std::to_string(int(std::lround(v))) : std::to_string(v));
    };
    it.on_default = [cvar] { Reset(cvar.c_str()); };
    if (const auto* flag = FindFlag(cvar)) {
      try {
        it.default_text = format(std::stof(flag->default_value));
      } catch (...) {
      }
    }
    return it;
  }

  static Item ActionItem(std::string label, std::string desc, std::string value, std::function<void()> fn) {
    Item it;
    it.type = ItemType::kAction;
    it.label = std::move(label);
    it.description = std::move(desc);
    it.value_text = std::move(value);
    it.on_activate = std::move(fn);
    return it;
  }

  static Item& Disable(Item& it, bool disabled, std::string note) {
    if (disabled) {
      it.disabled = true;
      it.disabled_note = std::move(note);
    }
    return it;
  }

  // The game's audio and subtitle options live in each save (profile_options.cpp); the launcher and
  // the pause menu set them in every save at once. They show the game being played (in the pause
  // menu) or else the most recently played save, read at most twice a second (the pages are built
  // every frame) and again at once after a write.
  static constexpr double kProfileReadInterval = 0.5;
  // A change is written once the value has settled (a slider is dragged through many steps).
  static constexpr double kProfileWriteDelay = 0.4;

  const std::vector<play::SaveSlot>& ProfileSaves() {
    const double now = ImGui::GetTime();
    if (profile_saves_at_ < 0 || now - profile_saves_at_ >= kProfileReadInterval) {
      profile_saves_ = play::ListSaves(profile_user_dir_);
      profile_saves_at_ = now;
    }
    return profile_saves_;
  }

  // The options as shown: the cached read with the changes still waiting to be written on top.
  // Not available when there is no save yet (or, in play, no game profile).
  play::ProfileOptions ShownProfileOptions() {
    const double now = ImGui::GetTime();
    if (profile_read_at_ < 0 || now - profile_read_at_ >= kProfileReadInterval) {
      play::ProfileOptions read;
      if (in_game_) read = play::ReadProfileOptions(profile_user_dir_, -1);
      if (!read.available) {
        const int slot = profile_slot_ >= 0 ? profile_slot_ : play::MostRecentSave(ProfileSaves());
        if (slot >= 0) read = play::ReadProfileOptions(profile_user_dir_, slot);
      }
      profile_read_ = std::move(read);
      profile_read_at_ = now;
    }
    play::ProfileOptions shown = profile_read_;
    if (shown.available) {
      float* volumes[] = {&shown.master, &shown.voices, &shown.music, &shown.effects};
      bool* toggles[] = {&shown.subtitles, &shown.old_movie, &shown.horizontal_flip, &shown.aiming_visor, &shown.inventory};
      for (size_t i = 0; i < size_t(play::ProfileOption::kCount); ++i) {
        const float observed = i >= 4 ? float(*toggles[i - 4]) : *volumes[i];
        if (std::abs(observed - profile_values_[i]) < 0.001f || !shown.error.empty()) profile_waiting_[i] = false;
        if (profile_dirty_[i] || profile_waiting_[i]) {
          if (i >= 4) *toggles[i - 4] = profile_values_[i] != 0;
          else *volumes[i] = profile_values_[i];
        }
      }
    }
    return shown;
  }

  void ChangeProfileOption(play::ProfileOption option, float value) {
    if (!ShownProfileOptions().available) return;
    profile_values_[size_t(option)] = value;
    profile_dirty_[size_t(option)] = true;
    profile_changed_at_ = ImGui::GetTime();
  }

  // Writes the waiting changes to every save: `now` when leaving, otherwise once they have settled.
  void FlushProfileOptions(bool now = true) {
    if (std::none_of(profile_dirty_.begin(), profile_dirty_.end(), [](bool b) { return b; })) return;
    if (!now && ImGui::GetTime() - profile_changed_at_ < kProfileWriteDelay) return;
    play::ProfileChange change;
    change.values = profile_values_;
    change.set = profile_dirty_;
    profile_dirty_.fill(false);
    const bool ok = play::SetProfileOptionsInAllSaves(profile_user_dir_, change);
    for (size_t i = 0; i < change.set.size(); ++i)
      if (change.set[i]) profile_waiting_[i] = ok;
    profile_read_at_ = profile_saves_at_ = -1;  // read again
    if (!ok) Status("The setting could not be saved in every save. Those saves were left unchanged.");
  }

  void ResetAudio() {
    for (int i = 0; i < 4; ++i) ChangeProfileOption(play::ProfileOption(i), 100);
    for (const char* name : {"kk_achievement_sound", "kk_achievement_sound_file", "kk_achievement_volume"}) Reset(name);
    if (!in_game_) for (const char* name : {"kk_launcher_music", "kk_launcher_music_volume",
                                           "kk_launcher_sounds", "kk_launcher_sounds_volume"}) Reset(name);
  }
  void ResetSubtitles() {
    profile_read_at_ = -1;  // its default follows the language, which may have just been reset
    const auto profile = ShownProfileOptions();
    if (profile.available) ChangeProfileOption(play::ProfileOption::kSubtitles, float(profile.subtitle_default));
  }

  // Why the options can't be changed now.
  const char* ProfileUnavailableNote() const {
    return in_game_ ? "No game profile is available yet."
                    : "Start a game to create a save, then change its settings here or in the pause menu.";
  }

  // Gameplay's Subtitles row.
  Item SubtitlesItem() {
    FlushProfileOptions(false);
    const auto profile = ShownProfileOptions();
    if (!profile.error.empty()) Status(profile.error);
    Item it;
    it.type = ItemType::kChoice;
    it.label = "Subtitles";
    it.description = "Show the game's dialogue subtitles. Applies during play and to every save.";
    it.options = {"Off", "On"};
    it.index = profile.subtitles ? 1 : 0;
    it.default_text = profile.subtitle_default ? "On" : "Off";
    it.on_choice = [this](int i) { ChangeProfileOption(play::ProfileOption::kSubtitles, float(i)); };
    it.on_default = [this] { ResetSubtitles(); };
    Disable(it, !profile.available, ProfileUnavailableNote());
    return it;
  }

  Item ProfileToggle(const char* label, const char* description, play::ProfileOption option, bool value) {
    Item it;
    it.type = ItemType::kChoice;
    it.label = label;
    it.description = description;
    it.options = {"Off", "On"};
    it.index = value ? 1 : 0;
    it.default_text = "Off";
    it.on_choice = [this, option](int i) { ChangeProfileOption(option, float(i)); };
    it.on_default = [this, option] { ChangeProfileOption(option, 0); };
    Disable(it, !ShownProfileOptions().available, ProfileUnavailableNote());
    return it;
  }

  // A save's bonus progress, read at most twice a second (the pages are built every frame; task 144: the
  // Graphics page and Extras read the save file on every frame).
  play::ExtraProgress ExtraProgressFor(int slot) {
    const double now = ImGui::GetTime();
    if (slot != extra_progress_slot_ || extra_progress_at_ < 0 || now - extra_progress_at_ >= kProfileReadInterval ||
        profile_read_at_ < 0) {
      extra_progress_ = play::ReadExtraProgress(profile_user_dir_, slot);
      extra_progress_slot_ = slot;
      extra_progress_at_ = now;
    }
    return extra_progress_;
  }

  void OriginalVideoItems(std::vector<Item>& items) {
    const auto profile = ShownProfileOptions();
    const int slot = profile.slot;
    auto progress = ExtraProgressFor(slot);
    progress.museum |= GetBool("kk_cheats") && GetBool("kk_cheat_bonus");
    items.push_back(Section("Extras"));
    for (int id : {1, 8}) {
      const bool old = id == 1;
      const bool unlocked = play::ExtraUnlocked(*play::FindExtra(id), progress);
      auto it = ProfileToggle(old ? "Old movie filter" : "Horizontal flip",
          unlocked ? "An original bonus video mode, unlocked in Extras."
                   : "An original bonus video mode. Unlock it in Extras before changing it.",
          old ? play::ProfileOption::kOldMovie : play::ProfileOption::kHorizontalFlip,
          old ? profile.old_movie : profile.horizontal_flip);
      Disable(it, !unlocked, "Locked");
      items.push_back(std::move(it));
    }
  }

  // The Audio page: the game's four volumes.
  std::vector<Item> AudioItems() {
    std::vector<Item> items;
    FlushProfileOptions(false);
    const auto profile = ShownProfileOptions();
    if (!profile.error.empty()) Status(profile.error);
    items.push_back(Section("Game audio"));
    // Shown master, music, effects, then voices (settings_layout.h); `i` stays the save's option number.
    const char* labels[] = {"Master volume", "Voices volume", "Music volume", "Effects volume"};
    const char* descriptions[] = {"How loud all game audio is. Applies during play and to every save.",
                                  "How loud the characters' voices are. Applies during play and to every save.",
                                  "How loud the game's music is. Applies during play and to every save.",
                                  "How loud the game's sound effects are. Applies during play and to every save."};
    const float values[] = {profile.master, profile.voices, profile.music, profile.effects};
    for (const int i : {0, 2, 3, 1}) {
      Item it;
      it.type = ItemType::kSlider;
      it.label = labels[i];
      it.description = descriptions[i];
      it.lo = 0; it.hi = 100; it.step = 1;
      it.value = values[i];
      it.value_text = std::to_string(int(std::lround(values[i]))) + "%";
      it.default_text = "100%";
      it.on_value = [this, i](float v) { ChangeProfileOption(play::ProfileOption(i), v); };
      it.on_default = [this, i] { ChangeProfileOption(play::ProfileOption(i), 100); };
      Disable(it, !profile.available, ProfileUnavailableNote());
      items.push_back(std::move(it));
    }
    if (!in_game_) {
      items.push_back(Section("Launcher"));
      items.push_back(CvarToggle("Menu music",
                                 "The game's main menu music while the launcher is open, from your copy of the game.",
                                 "kk_launcher_music"));
      {
        Item it = CvarSlider("Menu music volume", "How loud the launcher's music is.", "kk_launcher_music_volume", 0,
                             100, 5, [](float v) { return std::to_string(int(std::lround(v))) + "%"; });
        items.push_back(std::move(Disable(it, !GetBool("kk_launcher_music"), "Switch Menu music on to set its volume.")));
      }
      items.push_back(CvarToggle("Menu sounds",
                                 "The game's own menu sounds as you move around the launcher and choose things.",
                                 "kk_launcher_sounds"));
      {
        Item it = CvarSlider("Menu sound volume", "How loud the launcher's menu sounds are.",
                             "kk_launcher_sounds_volume", 0, 100, 5,
                             [](float v) { return std::to_string(int(std::lround(v))) + "%"; });
        items.push_back(
            std::move(Disable(it, !GetBool("kk_launcher_sounds"), "Switch Menu sounds on to set their volume.")));
      }
    }
    items.push_back(Section("Achievements"));
    items.push_back(CvarToggle("Sound", "The sound that plays with each pop-up.", "kk_achievement_sound"));
    const bool sound = GetBool("kk_achievement_sound");
    const char* sound_note = "Switch Sound on to choose it.";
    {
      if (sounds_.empty() || ImGui::GetTime() - sounds_scanned_ > 3.0) {
        sounds_ = ListSounds(profile_user_dir_);
        sounds_.insert(sounds_.begin(), std::filesystem::path());  // built-in chime
        sounds_scanned_ = ImGui::GetTime();
      }
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Sound to play";
      it.description =
          "The built-in chime or a sound you added. Put .wav files in the sounds folder to see them here.";
      const std::string cur = Get("kk_achievement_sound_file");
      it.index = 0;
      std::vector<std::string> values;
      for (size_t i = 0; i < sounds_.size(); ++i) {
        it.options.push_back(sounds_[i].empty() ? std::string("Original chime") : SoundLabel(sounds_[i]));
        values.push_back(sounds_[i].string());
        if (values.back() == cur) it.index = int(i);
      }
      it.on_choice = [this, values](int i) {
        Set("kk_achievement_sound_file", values[size_t(i)]);
        PlayAchievementSound(profile_user_dir_);  // preview
      };
      it.on_default = [] { Reset("kk_achievement_sound_file"); };
      it.default_text = "Original chime";
      items.push_back(std::move(Disable(it, !sound, sound_note)));
    }
    {
      Item it = CvarSlider("Volume", "How loud the sound is. It plays as you change it.", "kk_achievement_volume", 0,
                           100, 5, [](float v) { return std::to_string(int(std::lround(v))) + "%"; });
      auto set = it.on_value;
      it.on_value = [this, set](float v) {
        set(v);
        preview_at_ = ImGui::GetTime() + 0.3;  // once it settles
      };
      items.push_back(std::move(Disable(it, !sound, sound_note)));
    }
    items.push_back(ActionItem("Open sounds folder", "Where your own .wav sounds go.", "", [this] {
      OpenInExplorer(SoundsDir(profile_user_dir_));
      sounds_.clear();  // rescan when the list is next built
    }));
    items.push_back(Section("Reset"));
    items.push_back(ActionItem("Reset page", "Put these settings back to their defaults.", "", [this] {
      ui::Modal m;
      m.open = true; m.title = "Reset page";
      m.body = "Put these settings back to their defaults?";
      m.buttons = {"Reset", "Cancel"}; m.cancel = m.focus = 1; m.width = 560;
      m.on_button = [this](int i) { if (i == 0) ResetAudio(); };
      modal_ = std::move(m);
    }));
    return items;
  }

  static constexpr const char* kSetByOriginal =
      "Set by the Original Xbox 360 look. Switch Look to Remastered on the Graphics page to change it.";

  // The native renderer's lighting options (native-renderer/docs/lighting.md): rows on the Graphics
  // page while the native renderer draws the game. Its own cvars (native_*), which the stable version
  // does not have.
  static void LightingItems(std::vector<Item>& items, bool original) {
    // Task 062: keep experimental code available to the coordinator, hide unverified
    // water coverage, shaft placement and sparse dynamic-shadow gains for Beta 4.
    constexpr bool ready_for_beta4 = false;
    if (!ready_for_beta4 || !NativeShaderPack::Wanted()) return;
    {
      Item it = CvarChoice("Shadows",
                           "Original keeps the game's blocky shadows. High draws them at twice the detail with soft "
                           "edges; Ultra at twice the detail, with edges that get softer the farther a shadow falls from "
                           "what casts it.",
                           "native_shadow_quality", {{"Original", "original"}, {"High", "high"}, {"Ultra", "ultra"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarChoice("Water reflections",
                           "Screen space reflects what is on screen in the water (rocks, trees, the ship), and keeps the "
                           "game's own reflection where nothing is found.",
                           "native_water_reflections", {{"Original", "original"}, {"Screen space", "ssr"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarChoice("Light shafts",
                           "Volumetric lights the game's mist and fog from the sun: shadows cut shafts of light through "
                           "it. Only where the game has fog, so clear scenes stay clear. High is finer.",
                           "native_volumetrics", {{"Original", "off"}, {"Volumetric low", "low"}, {"Volumetric high", "high"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
  }

  // ------------------------------------------------------------ Display ---
  // Display > Frame rate (Beta 2: from the Gameplay page).
  Item FrameRateItem() {
    std::vector<Option> opts;
    for (int f : kFrameRateChoices)
      opts.push_back({f <= 0 ? "Unlimited" : f == 30 ? "30 FPS (Xbox 360)" : std::to_string(f) + " FPS",
                      std::to_string(f)});
    const std::string cur = Get("kk_frame_rate");
    bool listed = false;
    for (const auto& o : opts) listed = listed || o.value == cur;
    if (!listed && !cur.empty()) opts.push_back({F("{0} FPS (settings file)", {cur}), cur});
    Item it = CvarChoice("Frame rate",
                         "How many pictures the game draws a second (your screen's refresh rate is separate). 30 "
                         "matches the Xbox 360 and keeps every animation right. Higher is smoother, but some "
                         "character animations are not right above 30 yet. For the smoothest motion, pick a rate "
                         "that divides your screen's evenly, such as 60 on a 120 Hz screen.",
                         "kk_frame_rate", opts);
    return std::move(Disable(it, GetBool("kk_original_look"), kSetByOriginal));
  }

  // Display > Upscaling (Beta 2: from the Graphics page): the upscaler and how many pixels the game
  // draws for the screen, which the Resolution above decides.
  void UpscalingItems(std::vector<Item>& items) {
    const auto [out_w, out_h] = cb_.output_size ? cb_.output_size() : std::pair<int, int>{1280, 720};
    const bool original = GetBool("kk_original_look");
    const char* draws = GetBool("fullscreen") ? "Draws {0} for your {1} screen." : "Draws {0} for your {1} window.";
    items.push_back(Section("Upscaling"));
    const std::string temporal = CurrentTemporalUpscaler();
    {
      // Each choice: its name, present_effect (the spatial upscalers, in the presenter) and
      // native_upscaler (the temporal ones, in the native renderer; docs/upscalers.md).
      struct Choice {
        std::string label, present, temporal;
      };
      // NIS is in the D3D12 presenter only (the Vulkan one, used on Linux, would give FSR).
      std::vector<Choice> choices = {{"Off", "bilinear", "off"}, {"AMD FSR 1", "fsr", "off"}};
#if defined(_WIN32)
      choices.push_back({"NVIDIA NIS", "nis", "off"});
      const char* which = "AMD FSR 1 and NVIDIA Image Scaling (NIS) work on any graphics card.";
#else
      const char* which = "AMD FSR 1 works on any graphics card.";
#endif
      const OfferedUpscalers offered = OfferedTemporalUpscalers();
      bool any_temporal = false;
      for (const TemporalUpscaler& t : kTemporalUpscalers) {
        if (offered.Has(t.value)) {
          choices.push_back({t.label, "bilinear", t.value});
          any_temporal = true;
        } else if (temporal == t.value) {  // chosen before, not offered here (it falls back to FSR 3.1)
          choices.push_back({F("{0} (not on this PC)", {T(std::string(t.label))}), "bilinear", t.value});
        }
      }
      if (!temporal.empty()) {
        bool listed = false;
        for (const Choice& c : choices) listed = listed || c.temporal == temporal;
        if (!listed) choices.push_back({F("{0} (settings file)", {temporal}), "bilinear", temporal});
      }
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Upscaler";
      std::string desc =
          std::string("Scales the picture up to your screen and sharpens it, for a clearer image when the game draws "
                      "fewer pixels than your screen has. ") +
          which + " Off uses plain smooth scaling.";
      if (any_temporal || !temporal.empty())
        desc += " DLSS, FSR 4, FSR 3.1 and XeSS rebuild a sharp picture from several frames and smooth jagged edges "
                "too; only those your graphics card supports are listed. The default is the best one it runs: DLSS "
                "on NVIDIA cards, FSR 4 or FSR 3.1 on AMD cards, XeSS on Intel cards, FSR 3.1 on others, and FSR 1 "
                "where none of them runs. Choosing one of them restarts the game when you press Play.";
      it.description = desc;
      it.index = -1;
      const std::string present = Get("present_effect");
      for (size_t i = 0; i < choices.size(); ++i) {
        it.options.push_back(choices[i].label);
        const bool match = temporal.empty() ? choices[i].temporal == "off" && choices[i].present == present
                                            : choices[i].temporal == temporal;
        if (match && it.index < 0) it.index = int(i);
      }
      if (it.index < 0) it.value_text = present;
      if (UsingNativeRenderer() && !offered.known && HasTemporalSetting())
        it.facts.push_back("Checking which other upscalers your graphics card runs...");
      it.on_choice = [choices](int i) {
        const Choice& c = choices[size_t(i)];
        Set("present_effect", c.present);
        SetTemporalUpscaler(c.temporal.c_str());
        if (c.temporal != "off") {
          // The temporal upscalers take the picture at your screen's size (Native) and draw fewer
          // pixels themselves (their mode).
          Set("kk_render_quality", "native");
        } else if (i != 0 && UpscalerMode() < 0) {
          // The upscaler modes are Native to Performance; others start at Native.
          Set("kk_render_quality", "native");
        }
      };
      it.on_default = [] {
        Reset("present_effect");
        if (HasTemporalSetting()) Reset("native_upscaler");
      };
      {
        // The default: the best temporal upscaler this PC runs, else FSR 1 (settings.h, ApplyUpscalerDefaults).
        const auto* tflag = FindFlag("native_upscaler");
        const std::string tdefault = tflag && UsingNativeRenderer() ? tflag->default_value : std::string("off");
        const auto* pflag = FindFlag("present_effect");
        for (const Choice& c : choices) {
          if (tdefault != "off" ? c.temporal == tdefault
                                : c.temporal == "off" && pflag && c.present == pflag->default_value)
            it.default_text = c.label;
        }
      }
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    if (!temporal.empty()) {
      // The temporal upscaler's mode: it draws the game at the output size divided by the mode's ratio
      // and rebuilds the output size (the screen's, in 720p steps: Render quality Native).
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Upscaler mode";
      it.description =
          "How many pixels the game draws before the upscaler rebuilds the picture at your screen's size. Native AA "
          "draws every pixel and only smooths the edges (DLAA with DLSS); Quality, Balanced, Performance and Ultra "
          "Performance draw fewer for more speed.";
      const std::string cur = Get("native_upscaler_quality");
      it.index = -1;
      const int scale = RenderScaleFor("native", out_h);
      const int ow = 1280 * scale, oh = 720 * scale;
      for (size_t m = 0; m < std::size(kTemporalModes); ++m) {
        it.options.push_back(kTemporalModes[m].label);
        if (cur == kTemporalModes[m].value) {
          it.index = int(m);
          const double r = kTemporalModes[m].ratio;
          it.facts.push_back(F(draws, {Size(int(std::lround(ow / r)), int(std::lround(oh / r))), Size(out_w, out_h)}));
          if (ow != out_w || oh != out_h)
            it.facts.push_back(F("The upscaler's picture is {0}, scaled to fit.", {Size(ow, oh)}));
        }
      }
      if (it.index < 0) it.value_text = cur;
      it.on_choice = [](int i) { Set("native_upscaler_quality", kTemporalModes[i].value); };
      it.on_default = [] { Reset("native_upscaler_quality"); };
      if (const auto* flag = FindFlag("native_upscaler_quality"))
        for (const TemporalMode& m : kTemporalModes)
          if (flag->default_value == m.value) it.default_text = m.label;
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
      return;  // the render quality is Native with these (no Internal resolution either)
    }
    if (Get("present_effect") != "bilinear") {
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Upscaler mode";
      it.description =
          "How many pixels the game draws before the upscaler scales them to your screen. Native draws at your "
          "screen's size and only sharpens; Quality, Balanced and Performance draw fewer for more speed. The game "
          "draws in steps of its original 720p, so some modes can be the same at your screen size.";
      it.options.assign(std::begin(kUpscalerModeNames), std::end(kUpscalerModeNames));
      const int mode = UpscalerMode();
      it.index = mode;
      if (mode >= 0) {
        const int scale = RenderScaleFor(kUpscalerModes[mode], out_h);
        it.facts.push_back(F(draws, {Size(1280 * scale, 720 * scale), Size(out_w, out_h)}));
        std::string same;
        for (int m = 0; m < 4; ++m) {
          if (m == mode || RenderScaleFor(kUpscalerModes[m], out_h) != scale) continue;
          same += same.empty() ? T(std::string(kUpscalerModeNames[m]))
                               : " " + T(std::string("and")) + " " + T(std::string(kUpscalerModeNames[m]));
        }
        if (!same.empty()) it.facts.push_back(F("Same as {0} here.", {same}));
      } else {
        it.value_text = "Custom";
        it.facts.push_back("Now set to another render quality. Pick a mode to use one of these.");
      }
      it.on_choice = [](int i) { Set("kk_render_quality", kUpscalerModes[i]); };
      it.on_default = [] { Reset("kk_render_quality"); };
      if (const auto* flag = FindFlag("kk_render_quality"))
        for (int m = 0; m < 4; ++m)
          if (flag->default_value == kUpscalerModes[m]) it.default_text = kUpscalerModeNames[m];
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    } else {
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Render quality";
      it.description =
          "How sharply the game is drawn compared with your screen. Native matches it; Quality, Balanced and the "
          "Performance modes draw fewer pixels and scale up (an upscaler makes that sharper). The game renders in "
          "steps of its original 720p.";
      const std::string cur = Get("kk_render_quality");
      std::vector<std::string> values;
      it.index = -1;
      for (const auto& p : RenderPresets()) {
        const int scale = RenderScaleFor(p.id, out_h);
        if (cur == p.id) {
          it.index = int(values.size());
          it.facts.push_back(F(draws, {Size(1280 * scale, 720 * scale), Size(out_w, out_h)}));
        }
        it.options.push_back(p.label);
        values.push_back(p.id);
      }
      it.options.push_back("Custom");
      values.push_back("custom");
      if (cur == "custom") it.index = int(values.size()) - 1;
      it.value_text = cur;
      it.on_choice = [values](int i) { Set("kk_render_quality", values[size_t(i)]); };
      it.on_default = [] { Reset("kk_render_quality"); };
      if (const auto* flag = FindFlag("kk_render_quality"))
        for (const auto& p : RenderPresets())
          if (flag->default_value == p.id) it.default_text = p.label;
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    if (Get("kk_render_quality") == "custom") {
      Item it = CvarChoice("Internal resolution", "Draw the game at an exact multiple of its native 1280 " KK_TIMES " 720.",
                           "resolution_scale",
                           {{"1" KK_TIMES "  " KK_DOT "  1280 " KK_TIMES " 720", "1"},
                            {"2" KK_TIMES "  " KK_DOT "  2560 " KK_TIMES " 1440", "2"},
                            {"3" KK_TIMES "  " KK_DOT "  3840 " KK_TIMES " 2160", "3"},
                            {"4" KK_TIMES "  " KK_DOT "  5120 " KK_TIMES " 2880", "4"},
                            {"5" KK_TIMES "  " KK_DOT "  6400 " KK_TIMES " 3600", "5"},
                            {"6" KK_TIMES "  " KK_DOT "  7680 " KK_TIMES " 4320", "6"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
  }

  std::vector<Item> DisplayItems() {
    // The order of settings_layout.h (kDisplayRows).
    std::vector<Item> items;
    items.push_back(Section("Screen"));
    items.push_back(WindowModeItem());
    items.push_back(MonitorItem());
    // Each mode's own size: the window's, or the screen's in exclusive fullscreen (Borderless has none).
    if (!GetBool("fullscreen")) items.push_back(WindowSizeItem());
    if (ExclusiveFullscreen()) {
      items.push_back(ResolutionItem());
      items.push_back(RefreshRateItem());
    }
    items.push_back(CvarChoice("Aspect ratio",
                               "The game is 16:9. Letterbox keeps its shape on other screens; Stretch fills them.",
                               "present_letterbox", {{"Letterbox 16:9", "true"}, {"Stretch", "false"}}));
    items.push_back(Section("Frame rate"));
    items.push_back(CvarToggle("VSync",
                               "On waits for the display for tear-free frames. Off has the lowest latency and lets "
                               "G-Sync and FreeSync displays run freely.",
                               "d3d12_allow_variable_refresh_rate_and_tearing", "Off", "On", /*invert=*/true));
    items.push_back(FrameRateItem());
    if (FindFlag("present_hdr")) {
      items.push_back(Section("HDR output"));
      items.push_back(CvarToggle("HDR",
          "Requires Windows HDR on the current display. Uses SDR when HDR is unavailable.", "present_hdr"));
      if (GetBool("present_hdr")) {
        auto nits = [](float value) { return std::to_string(int(std::lround(value))) + " nits"; };
        items.push_back(CvarSlider("Peak brightness (nits)",
            "Highlight ceiling, limited to the display's reported peak brightness.",
            "present_hdr_peak", 80, 10000, 20, nits));
        items.push_back(CvarSlider("Paper white (nits)",
            "Brightness of neutral whites and menus. Limited by peak brightness.",
            "present_hdr_paper_white", 80, 500, 10, nits));
      }
    }
    return items;
  }

  // Keyboard & mouse shows the keys in the game's hints and turns the camera
  // with the mouse; back on Controller, the controller's prompts come back.
  static void SetInputMode(bool keyboard) {
    SetBool("mnk_mode", keyboard);
    const std::string prompts = Get("kk_button_prompts");
    if (keyboard) {
      if (prompts != "keyboard") Set("kk_pad_prompts", prompts);
      Set("kk_button_prompts", "keyboard");
      SetBool("mnk_mouse", true);
    } else if (prompts == "keyboard") {
      Set("kk_button_prompts", Get("kk_pad_prompts"));
    }
  }

  void SetFullscreen(bool on) {
    SetBool("fullscreen", on);
    if (cb_.set_fullscreen) cb_.set_fullscreen(on);
  }

  Item WindowSizeItem() {
    // window_width/height are in logical pixels (96 DPI); offer real pixel sizes
    // that fit on the screen and convert with the window's DPI scale.
    const double scale = cb_.dpi_scale ? cb_.dpi_scale() : 1.0;
    auto to_physical = [&](int logical) { return int(logical * scale + 0.5); };
    const int w = to_physical(GetInt("window_width")), h = to_physical(GetInt("window_height"));
    static const std::pair<int, int> kSizes[] = {{0, 0},       {1280, 720},  {1600, 900},
                                                 {1920, 1080}, {2560, 1440}, {3200, 1800}};
    const auto [screen_w, screen_h] = cb_.screen_size ? cb_.screen_size() : std::pair<int, int>{1 << 16, 1 << 16};
    auto close_to = [](int a, int b) { return std::abs(a - b) <= 2; };
    Item it;
    it.type = ItemType::kChoice;
    it.label = "Window size";
    it.description = "The window's size in windowed mode. Applies when the game starts.";
    it.index = -1;
    it.value_text = Size(w, h);
    it.default_text = "Default";
    std::vector<std::pair<int, int>> sizes;
    for (auto [sw, sh] : kSizes) {
      if (sw >= screen_w || sh >= screen_h) continue;  // must fit with its frame
      if (close_to(sw, w) && close_to(sh, h)) it.index = int(sizes.size());
      it.options.push_back(sw == 0 ? std::string("Default") : Size(sw, sh));
      sizes.emplace_back(sw, sh);
    }
    it.on_choice = [sizes, scale](int i) {
      const auto [sw, sh] = sizes[size_t(i)];
      SetInt("window_width", int(sw / scale + 0.5));
      SetInt("window_height", int(sh / scale + 0.5));
    };
    it.on_default = [] {
      Reset("window_width");
      Reset("window_height");
    };
    return it;
  }

  // Fullscreen resolution (display_modes.h): Desktop, or one of the monitor's sizes, with its refresh
  // rate on a row of its own (RefreshRateItem). Both are kept in kk_fullscreen_mode ("2560x1440@144").
  void LoadDisplayModes() {
    const int monitor = GetInt("monitor", 0);
    if (display_modes_monitor_ == monitor) return;
    display_modes_monitor_ = monitor;
    display_modes_.clear();
    const std::vector<DisplayMode> all = cb_.display_modes ? cb_.display_modes(monitor) : std::vector<DisplayMode>{};
    auto whole = [](float hz) { return std::fabs(hz - std::round(hz)) < 0.05f; };
    for (const DisplayMode& m : all) {
      if (m.width < 1280 || m.height < 720) continue;  // below the game's own 720p
      // 59.94 or 119.88 only when the display doesn't also have 60 or 120 at that size.
      const bool twin = !whole(m.refresh) && std::any_of(all.begin(), all.end(), [&](const DisplayMode& o) {
        return o.width == m.width && o.height == m.height && whole(o.refresh) &&
               std::fabs(o.refresh - std::round(m.refresh)) < 0.05f;
      });
      if (!twin) display_modes_.push_back(m);  // SDL's order: largest first, then the highest rate
    }
  }

  // The refresh rates the display has at a size, highest first.
  std::vector<float> RatesAt(int width, int height) const {
    std::vector<float> rates;
    for (const DisplayMode& m : display_modes_)
      if (m.width == width && m.height == height) rates.push_back(m.refresh);
    return rates;
  }

  // Window mode: Windowed (fullscreen off), Borderless (fullscreen, kk_fullscreen_mode "desktop": a
  // window filling the screen at the desktop's resolution) or Fullscreen (kk_fullscreen_mode a mode:
  // exclusive, the screen switched to it while the game runs; the launcher itself stays borderless).
  static bool ExclusiveFullscreen() {
    return GetBool("fullscreen") && ParseDisplayMode(Get("kk_fullscreen_mode")).has_value();
  }

  // The desktop's own mode, as listed (a 59.94 that the list shows as 60 becomes 60), for Fullscreen
  // until a Resolution is picked.
  std::string DesktopModeValue() {
    LoadDisplayModes();
    DisplayMode desktop = cb_.desktop_mode ? cb_.desktop_mode(GetInt("monitor", 0)) : DisplayMode{};
    for (const DisplayMode& m : display_modes_)
      if (m.width == desktop.width && m.height == desktop.height && std::fabs(m.refresh - desktop.refresh) < 0.1f)
        desktop.refresh = m.refresh;
    if (desktop.width <= 0 && !display_modes_.empty()) desktop = display_modes_.front();
    return desktop.width > 0 ? desktop.Value() : "desktop";
  }

  Item WindowModeItem() {
    Item it;
    it.type = ItemType::kChoice;
    it.label = "Window mode";
    it.description =
        "Windowed plays in a window. Borderless fills the screen with a window at your desktop's resolution, so "
        "Alt+Tab and other windows switch instantly. Fullscreen changes your screen to the Resolution and Refresh "
        "rate you pick below while the game runs.";
    it.options = {"Windowed", "Borderless", "Fullscreen"};
    it.index = !GetBool("fullscreen") ? 0 : ExclusiveFullscreen() ? 2 : 1;
    it.default_text = "Windowed";
    it.on_choice = [this](int i) {
      if (i == 1) Set("kk_fullscreen_mode", "desktop");
      if (i == 2 && !ParseDisplayMode(Get("kk_fullscreen_mode"))) Set("kk_fullscreen_mode", DesktopModeValue());
      SetFullscreen(i != 0);
    };
    it.on_default = [this] {
      Reset("kk_fullscreen_mode");
      Reset("fullscreen");
      if (cb_.set_fullscreen) cb_.set_fullscreen(GetBool("fullscreen"));
    };
    if (it.index == 2) it.facts.push_back("The launcher stays borderless; the screen changes when the game starts.");
    return it;
  }

  // Fullscreen only: the screen's size while the game runs.
  Item ResolutionItem() {
    LoadDisplayModes();
    const auto cur = ParseDisplayMode(Get("kk_fullscreen_mode"));
    Item it;
    it.type = ItemType::kChoice;
    it.label = "Resolution";
    it.description =
        "The size your screen switches to while the game runs, at the Refresh rate below. The render quality "
        "follows it. Applies when the game starts.";
    std::vector<std::pair<int, int>> sizes;
    for (const DisplayMode& m : display_modes_) {
      if (!sizes.empty() && sizes.back() == std::pair<int, int>{m.width, m.height}) continue;
      sizes.emplace_back(m.width, m.height);
      it.options.push_back(Size(m.width, m.height));
    }
    it.index = -1;
    for (size_t i = 0; i < sizes.size(); ++i)
      if (sizes[i] == std::pair<int, int>{cur->width, cur->height}) it.index = int(i);
    if (it.index < 0) {  // a size this display doesn't have (another monitor's, or the settings file's)
      it.index = int(sizes.size());
      sizes.emplace_back(cur->width, cur->height);
      it.options.push_back(F("{0} (not on this display)", {Size(cur->width, cur->height)}));
    }
    it.facts.push_back(F("The game will be shown at {0}.", {Size(cur->width, cur->height)}));
    const auto desktop = ParseDisplayMode(DesktopModeValue());
    if (desktop) it.default_text = Size(desktop->width, desktop->height);
    it.on_choice = [this, sizes, cur](int i) {
      const auto [w, h] = sizes[size_t(i)];
      const std::vector<float> rates = RatesAt(w, h);
      // The rate chosen before when this size has it, else this size's highest.
      float rate = rates.empty() ? (cur ? cur->refresh : 0.0f) : rates.front();
      for (const float r : rates)
        if (cur && std::fabs(r - cur->refresh) < 0.01f) rate = r;
      Set("kk_fullscreen_mode", DisplayMode{w, h, rate}.Value());
    };
    it.on_default = [this] { Set("kk_fullscreen_mode", DesktopModeValue()); };
    return it;
  }

  // Fullscreen only: the display's rates at the Resolution's size.
  Item RefreshRateItem() {
    const auto cur = ParseDisplayMode(Get("kk_fullscreen_mode"));
    Item it;
    it.type = ItemType::kChoice;
    it.label = "Refresh rate";
    it.description =
        "How many times a second your screen refreshes at the Resolution above: your screen's setting. How many "
        "pictures the game draws a second is the Frame rate, below. Applies when the game starts.";
    std::vector<float> rates = RatesAt(cur->width, cur->height);
    if (rates.empty()) rates.push_back(cur->refresh);
    it.index = -1;
    for (size_t i = 0; i < rates.size(); ++i) {
      const DisplayMode m{cur->width, cur->height, rates[i]};
      it.options.push_back(m.RateLabel());
      if (std::fabs(rates[i] - cur->refresh) < 0.01f) it.index = int(i);
    }
    it.value_text = cur->RateLabel();
    it.default_text = DisplayMode{cur->width, cur->height, rates.front()}.RateLabel();
    it.on_choice = [w = cur->width, h = cur->height, rates](int i) {
      Set("kk_fullscreen_mode", DisplayMode{w, h, rates[size_t(i)]}.Value());
    };
    it.on_default = [w = cur->width, h = cur->height, top = rates.front()] {
      Set("kk_fullscreen_mode", DisplayMode{w, h, top}.Value());
    };
    return it;
  }

  Item MonitorItem() {
    // In the SDK's order (review finding 1: Windows' own order can differ with several monitors).
    if (monitors_.empty()) monitors_ = ListDisplays();
    if (monitors_.empty()) monitors_ = ListMonitors();
    std::vector<Option> opts = {{"Default", "0"}};
    for (size_t i = 0; i < monitors_.size(); ++i) {
      const auto& m = monitors_[i];
      opts.push_back({F("Display {0}", {std::to_string(i + 1)}) + (m.primary ? " " + T(std::string("(main)")) : "") +
                          "  " KK_DOT "  " + Size(m.width, m.height),
                      std::to_string(i + 1)});
    }
    return CvarChoice("Monitor", "Which display the game opens on. Applies when the game starts.", "monitor",
                      std::move(opts));
  }

  // ----------------------------------------------------------- Graphics ---
  std::vector<Item> GraphicsItems() {
    {
      std::lock_guard lock(texture_pack_folder_choice.mutex);
      if (!texture_pack_folder_choice.selected.empty()) {
        Set("native_texture_pack_folder", texture_pack_folder_choice.selected);
        texture_pack_folder_choice.selected.clear();
        ++g_changes;
      }
    }
    const bool original = GetBool("kk_original_look");
    std::vector<Item> items;
    items.push_back(Section("Preset"));
    {
      Item it = CvarChoice("Look",
                           "Original keeps the Xbox 360 look. Remastered enables graphics presets and custom graphics settings. Its setup defaults are 60 FPS and Strong GTAO; Display sets the frame-rate cap.",
                           "kk_original_look", {{"Original Xbox 360", "true"}, {"Remastered", "false"}});
      it.on_choice = [](int i) { SetOriginalLook(i == 0); };
      it.on_default = [] { SetOriginalLook(false); };
      items.push_back(std::move(it));
    }
    {
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Preset";
      it.description = "Sets image quality, upscaling, ambient occlusion and global illumination. Changing a setting shows Custom.";
      for (const auto& p : kGraphicsPresets) it.options.push_back(p.label);
      it.options.push_back("Custom");
      const int match = MatchGraphicsPreset();
      it.index = original ? -1 : match < 0 ? kGraphicsPresetCount : match;
      it.value_text = "Xbox 360";  // the Original look's own settings
      it.on_choice = [](int i) {
        if (i < kGraphicsPresetCount) ApplyGraphicsPreset(i);
      };
      it.on_default = [] { ApplyGraphicsPreset(1); };
      it.default_text = "Medium";
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }

    if (!in_game_) {
      Item it;
      it.label = "Recommended";
      it.description = "Detect your hardware and recommend render resolution, upscaling, ambient occlusion and global illumination. "
                       "Settings you changed are kept. The frame-rate cap, graphics API and other lighting effects are kept.";
      const std::string tier = Get("kk_hardware_tier");
      it.value_text = tier == "low" ? "Low" : tier == "mid" ? "Medium" : tier == "high" ? "High" : tier;
      it.on_activate = [this] {
        const auto output = cb_.output_size ? cb_.output_size() : PrimaryScreenSize();
        ApplyHardwareRecommendation(true, output.first, output.second);
        ++g_changes;
      };
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }

    GraphicsApiItem(items);
    UpscalingItems(items);
    items.push_back(Section("Image"));
    {
      Item it = CvarChoice("Anti-aliasing", "Smooths jagged edges after the frame is drawn.", "swap_post_effect",
                           {{"Off", "none"}, {"FXAA", "fxaa"}, {"SMAA", "smaa"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarChoice("Sharpness", "Adaptive sharpening. Skipped when the upscaler's own sharpening is active.",
                           "native_sharpness", {{"Off", "off"}, {"Low", "low"}, {"Medium", "medium"}, {"High", "high"}});
      it.type = ItemType::kSlider;
      it.lo = 0; it.hi = 3; it.step = 1;
      it.value = float(std::max(it.index, 0));
      it.value_text = it.options[size_t(it.value)];
      it.on_value = [choose = it.on_choice](float v) { choose(std::clamp(int(std::lround(v)), 0, 3)); };
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    // 2x MSAA is not offered as a choice: it is how the Xbox 360 drew those
    // surfaces (native_2x_msaa, on by default), and the shader pack is built with it.
    {
      Item it = CvarChoice("Texture filtering", "Keeps the ground and distant textures sharp at steep angles.",
                           "anisotropic_override",
                           {{"Game", "-1"}, {"Off", "0"}, {"2" KK_TIMES, "2"}, {"4" KK_TIMES, "3"}, {"8" KK_TIMES, "4"},
                            {"16" KK_TIMES, "5"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    items.push_back(Section("Lighting"));
    {
      // Ambient occlusion lives in the GPU plugin (ao_mode, ao_strength). On the native renderer it is
      // GTAO (ApplyPortDefaults), worked out from the scene's depth at the end of the opaque pass, so
      // water, smoke, fire, fur and the sky drawn after it are never darkened (native-renderer/docs/ao.md).
      const bool native = NativeRendererAvailable() && UsingNativeRenderer();
      Item it = CvarChoice("Ambient occlusion",
                           native ? "Soft shading where surfaces meet: in corners and creases, and on the ground under "
                                    "rocks, grass and people (GTAO). Water, smoke, fire and the sky are never darkened. "
                                    "Costs about 1 to 2 ms a frame at 4K."
                                  : "Soft shading where surfaces meet: in corners and creases, and on the ground under "
                                    "rocks, grass and people. Costs about 1 ms a frame at 4K.",
                           "ao_mode", {{"Off", "0"}, {"On", "1"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
      if (GetInt("ao_mode", 0) != 0) {
        // GTAO: the shading's darkness (XeGTAO's final power 1.6, 2.2, 3.0) and how far it reaches from
        // each crease (6, 8, 12 near-plane distances). See docs/ao.md, "In the game".
        Item st = CvarChoice("AO strength",
                             native ? "How dark the shading gets and how far it spreads from each corner. Strong is "
                                      "recommended; Light and Medium are subtler."
                                    : "How dark the shading gets. Light is recommended.",
                             "ao_strength",
                             native ? std::vector<Option>{{"Light", "1"}, {"Medium", "2"}, {"Strong (recommended)", "3"}}
                                    : std::vector<Option>{{"Light (recommended)", "1"}, {"Medium", "2"}, {"Strong", "3"}});
        items.push_back(std::move(Disable(st, original, kSetByOriginal)));
        if (native) {
          Item quality = CvarChoice("AO quality",
              "More samples reduce grain on hands and flat surfaces. Medium keeps the previous sample count. High costs more GPU time.",
              "native_ao_samples", {{"Low", "1"}, {"Medium", "2"}, {"High", "3"}});
          items.push_back(std::move(Disable(quality, original, kSetByOriginal)));
        }
      }
    }
    if (FindFlag("native_gi") && rex::cvar::GetFlagByName("gpu_plugin") == "native") {
      // Global illumination lives in the native renderer (native_gi, native-renderer/docs/gi.md). A build
      // without it has no such setting and ignores the line in the settings file.
      Item it = CvarChoice("Global illumination",
                           "Light bouncing off sunlit surfaces into the shade beside them: sunlit rock tints the "
                           "shadow next to it, the jungle's green reaches the people in it. Medium costs about 1 ms a "
                           "frame at 1440p. On by default, following the graphics preset. Original keeps it off.",
                           "native_gi", {{"Off", "off"}, {"Low", "low"}, {"Medium", "medium"}, {"High", "high"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
      Item distance = CvarChoice("GI distance",
          "Limits bounced light to nearby surfaces to save GPU time. Near: 16 m, Medium: 32 m, Far: 64 m. Full has no limit. Light fades smoothly over the final quarter of the distance.",
          "native_gi_distance", {{"Near", "near"}, {"Medium", "medium"}, {"Far", "far"}, {"Full", "full"}});
      items.push_back(std::move(Disable(distance, original || Get("native_gi") == "off",
                                       original ? kSetByOriginal : "Switch Global illumination on to set its distance.")));
    }
    LightingItems(items, original);

    items.push_back(Section("Effects"));
    {
      Item it = CvarToggle("Motion blur",
                           "The trail the game blends over fast moments, mostly in Kong's sequences and some "
                           "transitions.",
                           "kk_motion_blur");
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarToggle("Screen blur",
                           "Some levels (Necropolis, Brontosaurus) blur the whole picture slightly. The blur is sized "
                           "for the Xbox 360's 720p, so on a sharper screen it makes those levels look low resolution. "
                           "Off keeps them sharp.",
                           "kk_big_blur");
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarToggle("Distance fog",
                           "The haze over far-away scenery. Off shows distant scenery clearly, but the fog is part of "
                           "Skull Island's look and also hides the edges of each area, so some empty or unfinished "
                           "backdrops can show.",
                           "kk_fog");
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }

    OriginalVideoItems(items);
    items.push_back(Section("Advanced"));
    {
      Item it = CvarChoice("Effect resolution", "Resolution of bloom, blur and glow buffers.",
                           "native_effect_buffers", {{"Original", "false"}, {"Output", "true"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    items.push_back(CvarChoice("Texture pack", "Use replacement textures made from your own extracted files.",
                              "native_texture_pack", {{"Off", "false"}, {"On", "true"}}));
    {
      const std::string folder = Get("native_texture_pack_folder");
      Item it = ActionItem("Texture pack folder", "Choose the folder containing your texture pack.",
                           folder.empty() ? T("Choose folder") : folder, [] {
        {
          std::lock_guard lock(texture_pack_folder_choice.mutex);
          if (texture_pack_folder_choice.busy) return;
          texture_pack_folder_choice.busy = true;
        }
        const std::string current = Get("native_texture_pack_folder");
        SDL_ShowOpenFolderDialog([](void*, const char* const* files, int) {
          std::lock_guard lock(texture_pack_folder_choice.mutex);
          if (files && files[0]) texture_pack_folder_choice.selected = files[0];
          texture_pack_folder_choice.busy = false;
        }, nullptr, nullptr, current.empty() ? nullptr : current.c_str(), false);
      });
      it.on_default = [] { Reset("native_texture_pack_folder"); };
      { std::lock_guard lock(texture_pack_folder_choice.mutex); it.disabled = texture_pack_folder_choice.busy; }
      items.push_back(std::move(it));
    }
    RebuildShadersItem(items);
    return items;
  }

  // Graphics > Renderer: the native renderer's graphics API (Vulkan or DirectX 12).
  static void GraphicsApiItem(std::vector<Item>& items) {
    if (!NativeRendererAvailable()) return;
    if (UsingNativeRenderer() && FindFlag("native_backend")) {
      items.push_back(Section("Renderer"));
      Item api;
      api.type = ItemType::kChoice;
      api.label = "Graphics API";
#if defined(__linux__)
      api.description = "How the native renderer talks to your graphics card. Auto uses Vulkan. Changes apply when the game starts.";
      api.options = {"Auto", "Vulkan"};
#else
      api.description = "Auto uses Vulkan. DirectX 12 is needed for AMD FSR 4. Changes apply when the game starts.";
      api.options = {"Auto", "Vulkan", "DirectX 12"};
#endif
      const std::string backend = Get("native_backend");
      api.index = backend == "auto" ? 0 : backend == "vulkan" ? 1 : backend == "d3d12" ? 2 : -1;
      api.value_text = backend;
      api.default_text = "Auto";
      api.on_choice = [](int i) {
        startup::ManualChoice();  // the player's choice wins over a startup fallback
        Set("native_backend", i == 0 ? "auto" : i == 1 ? "vulkan" : "d3d12");
        ApplyUpscalerDefaults();
      };
      api.on_default = [] {
        startup::ManualChoice();
        Reset("native_backend");
        ApplyUpscalerDefaults();
      };
      items.push_back(std::move(api));
    }
  }

  std::vector<Item> AdvancedItems() {
    std::vector<Item> items;
#if defined(_WIN32)
    items.push_back(Section("Cutscenes"));
    items.push_back(CvarToggle("Play PS2 cutscenes",
        "Play your own PS2 movies between missions. You must supply the movies and choose their folder.",
        "kk_ps2_cutscenes"));
    {
      const auto folder = movies::Folder().u8string();
      Item it = ActionItem("PS2 movie folder", "Choose the folder containing BK_xxxx.mp4.", "", [this] {
        const auto path = movies::BrowseFolder();
        if (!path.empty()) {
          Set("kk_ps2_movie_folder", path);
        }
      });
      it.value_text.assign(reinterpret_cast<const char*>(folder.data()), folder.size());
      it.on_default = [] { Reset("kk_ps2_movie_folder"); };
      items.push_back(std::move(it));
    }
#endif

    items.push_back(Section("Diagnostics"));
    items.push_back(CvarToggle("Frame counter", "Shows the game's frame rate in the corner. F2 toggles it while playing.",
                               "kk_show_fps", "Hidden", "Shown"));
    return items;
  }

  // ----------------------------------------------------------- Gameplay ---
  std::vector<Item> GameplayItems() {
    // The order of settings_layout.h (kGameplayRows).
    const bool original = GetBool("kk_original_look");
    std::vector<Item> items;
    items.push_back(Section("Subtitles and language"));
    items.push_back(SubtitlesItem());
    items.push_back(CvarChoice("Language", "The game's language, where the game includes it.", "user_language",
                               {{"English", "1"},
                                {"Deutsch", "3"},
                                {"EspaÃ±ol", "5"},
                                {"FranÃ§" "ais", "4"},
                                {"Italiano", "6"}}));
    items.push_back(Section("HUD"));
    {
      const auto profile = ShownProfileOptions();
      items.push_back(ProfileToggle("Aiming visor", "Show the game's aiming visor.", play::ProfileOption::kAimingVisor,
                                    profile.aiming_visor));
      items.push_back(ProfileToggle("Inventory", "Show the game's inventory indicators.", play::ProfileOption::kInventory,
                                    profile.inventory));
    }
    items.push_back(CvarToggle("Notifications", "A pop-up when you unlock an achievement in game.",
                               "kk_achievement_toasts"));
    items.push_back(CvarChoice("Pop-up style",
                               "Xbox 360 unfolds from a green ring at the bottom of the screen. Steam slides up in "
                               "the bottom-right corner. Try them with Test notification.",
                               "kk_achievement_style", {{"Xbox 360", "xbox360"}, {"Steam", "steam"}}));
    items.push_back(Section("Camera"));
    {
      Item it = CvarSlider("Field of view",
                           "How wide the camera sees. 69" KK_DEG " is the original for Jack; Kong, cutscene and other "
                           "cameras widen by the same amount. Jack's gun keeps its usual size.",
                           "kk_fov", 69, 110, 1, [](float v) {
                             const int d = int(std::lround(v));
                             return d <= 69 ? F("{0}Â° (original)", {std::to_string(d)}) : std::to_string(d) + KK_DEG;
                           });
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    items.push_back(CvarToggle("Camera shake",
                               "The camera shaking from the V-Rex's footsteps, explosions and Kong's blows.",
                               "kk_camera_shake"));
    {
      Item it = CvarSlider("Camera shake strength",
                           "How hard the camera shakes. 100% is the Xbox 360's; lower it if the shaking bothers you.",
                           "kk_camera_shake_strength", 10, 200, 5,
                           [](float v) { return std::to_string(int(std::lround(v))) + "%"; });
      items.push_back(
          std::move(Disable(it, !GetBool("kk_camera_shake"), "Switch Camera shake on to set its strength.")));
    }
    items.push_back(Section("Game"));
    items.push_back(CvarToggle("Startup logos",
                               "The Ubisoft, Universal and WingNut movies before the title screen. Story movies "
                               "still play.",
                               "kk_skip_intros", "Play", "Skip"));
    auto cheats = CheatItems();
    items.insert(items.end(), std::make_move_iterator(cheats.begin()), std::make_move_iterator(cheats.end()));
    return items;
  }

  // ----------------------------------------------------------- Controls ---
  std::vector<Item> ControlsItems() {
    std::vector<Item> items;
    const auto percent = [](float v) { return std::to_string(int(std::lround(v))) + "%"; };
    items.push_back(Section("Input"));
    {
      Item it = CvarToggle("Input",
                           "Controllers work automatically. Keyboard & mouse plays with a keyboard and mouse instead: "
                           "the game's hints show the keys and the mouse turns the camera. The keys are under "
                           "Keyboard bindings.",
                           "mnk_mode", "Controller", "Keyboard & mouse");
      it.on_choice = [](int i) { SetInputMode(i == 1); };
      it.on_default = [] { SetInputMode(false); };
      items.push_back(std::move(it));
    }
    {
      Item it = CvarChoice("Button prompts",
                           "Which controller's buttons the game and this launcher show in menus and hints. Automatic "
                           "matches the controller you're using.",
                           "kk_button_prompts",
                               {{"Automatic", "auto"},
                                {"Xbox 360", "xbox360"},
                                {"Xbox Series", "xbox_series"},
                                {"PlayStation 5", "ps5"},
                                {"PlayStation 2", "ps2"},
                                {"Steam Deck", "steam_deck"},
                                {"Keyboard", "keyboard"}});
      if (Get("kk_button_prompts") == "auto") {
        static const std::pair<const char*, const char*> kStyles[] = {
            {"xbox360", "Xbox 360"}, {"xbox_series", "Xbox Series"}, {"ps5", "PlayStation 5"},
            {"ps2", "PlayStation 2"}, {"steam_deck", "Steam Deck"}};
        const std::string now = PadSet();
        for (const auto& [id, label] : kStyles)
          if (now == id) it.facts.push_back(F("Now showing {0}.", {T(std::string(label))}));
      }
      items.push_back(std::move(it));
    }
    items.push_back(Section("Controller"));
    items.push_back(CvarSlider("Camera sensitivity", "How fast the right stick turns the camera and moves your aim.",
                               "kk_camera_sensitivity", 25, 300, 5, percent));
    items.push_back(CvarToggle("Camera response",
                               "Modern turns the camera the same way in every direction, so circles and diagonals feel "
                               "even. Original is the Xbox 360's own: small pushes up and down turn much more slowly "
                               "than small pushes sideways.",
                               "kk_camera_modern", "Original", "Modern"));
    items.push_back(CvarSlider("Stick deadzone", "Ignores small stick movements. Raise it if the camera or cursor drifts.",
                               "kk_deadzone", 0, 40, 1, percent));
    items.push_back(CvarToggle("Camera horizontal",
                               "Which way the camera turns when you push the right stick left or right.",
                               "kk_invert_rs_x", "Normal", "Inverted"));
    items.push_back(CvarToggle("Camera vertical", "Which way the right stick moves the camera up and down.",
                               "kk_invert_rs_y", "Normal", "Inverted"));
    {
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Left stick";
      it.description = "Invert the left stick's axes.";
      it.options = {"Normal", "Invert X", "Invert Y", "Both"};
      const bool x = GetBool("kk_invert_ls_x"), y = GetBool("kk_invert_ls_y");
      it.index = x && y ? 3 : x ? 1 : y ? 2 : 0;
      it.on_choice = [](int i) {
        SetBool("kk_invert_ls_x", i == 1 || i == 3);
        SetBool("kk_invert_ls_y", i == 2 || i == 3);
      };
      it.on_default = [] {
        Reset("kk_invert_ls_x");
        Reset("kk_invert_ls_y");
      };
      it.default_text = "Normal";
      items.push_back(std::move(it));
    }
    items.push_back(CvarToggle("Aim",
                               "Hold: keep the left trigger held to raise the gun, as on the console. Toggle: press "
                               "it once to raise the gun and again to lower it. Pausing lowers it.",
                               "kk_toggle_aim", "Hold", "Toggle"));
    items.push_back(CvarToggle("Vibration", "Controller rumble.", "kk_vibration"));
    {
      Item it = CvarSlider("Vibration strength",
                           "How strong the rumble is. Above 100% it is stronger than on the Xbox 360, up to the "
                           "controller's full strength.",
                           "kk_vibration_strength", 10, 200, 5,
                           percent);
      items.push_back(std::move(Disable(it, !GetBool("kk_vibration"), "Switch Vibration on to set its strength.")));
    }

    const bool mnk = GetBool("mnk_mode");
    const char* mnk_note = "Used when Input is Keyboard & mouse.";
    items.push_back(Section("Keyboard & mouse"));
    {
      Item it = CvarSlider("Mouse sensitivity",
                           "How far the camera turns as you move the mouse. It turns the same however fast you move "
                           "it, as in other first-person games.",
                           "kk_mouse_sensitivity", 0.1f, 5.0f, 0.05f, [](float v) {
                             char buf[32];
                             std::snprintf(buf, sizeof(buf), "%.2f" KK_TIMES, v);
                             return std::string(buf);
                           });
      items.push_back(std::move(Disable(it, !mnk, mnk_note)));
    }
    {
      Item it = CvarToggle("Mouse camera",
                           "Turn the camera with the mouse. Off, it turns with the keys bound to the right stick.",
                           "mnk_mouse");
      items.push_back(std::move(Disable(it, !mnk, mnk_note)));
    }
    {
      Item it = CvarToggle("Mouse vertical", "Which way moving the mouse up and down moves the camera.",
                           "kk_mouse_invert_y", "Normal", "Inverted");
      items.push_back(std::move(Disable(it, !mnk || !GetBool("mnk_mouse"), mnk_note)));
    }

    items.push_back(Section("Mapping"));
    {
      Item it;
      it.type = ItemType::kLink;
      it.label = "Button remapping";
      it.description = "Choose what each controller button does in the game.";
      it.on_activate = [this] { sub_ = kRemapPage; };
      items.push_back(std::move(it));
    }
    {
      Item it;
      it.type = ItemType::kLink;
      it.label = "Keyboard bindings";
      it.description = "The keys that stand in for each controller button when Input is Keyboard & mouse.";
      it.on_activate = [this] { sub_ = kKeysPage; };
      items.push_back(std::move(it));
    }
    // The graphics panel (debug_overlay.cpp): its keys work in play with either input.
    items.push_back(Section("Keys while playing"));
    {
      Item it;
      it.type = ItemType::kInfo;
      it.label = "Graphics panel";
      it.value_text = "F8";
      it.description = "F8 opens a panel over the game for trying graphics settings while you play. Hold F7 to see your saved settings for comparison. F6 saves a screenshot and its settings in the screenshots folder. Changes are temporary until you choose Save as my settings.";
      items.push_back(std::move(it));
    }

    return items;
  }

  // The picture for a controller button on remapping rows.
  rex::ui::ImmediateTexture* PadGlyph(Pad pad, float* aspect) {
    static const char* const kNames[] = {"dpad_up", "dpad",  "dpad",        "dpad",        "start", "back",
                                         "stick_click", "stick_click", "lb", "rb", "a", "b", "x", "y", "lt", "rt"};
    const size_t i = size_t(pad);
    if (i >= std::size(kNames)) return nullptr;
    const std::string set = PadSet();
    if (auto* t = glyphs_.Named(set, kNames[i], aspect)) return t;
    return glyphs_.Named("xbox_series", kNames[i], aspect);
  }

  std::vector<Item> RemapItems() {
    std::vector<Item> items;
    items.push_back(Section(T("Controls") + std::string("  " KK_DOT "  ") + T("Button remapping")));
    items.push_back(ActionItem("Reset to defaults", "Every button back to doing what it does on the Xbox 360.", "",
                               [this] {
                                 for (size_t i = 0; i < kPadCount; ++i) SetMapping(Pad(i), Pad(i));
                                 ++g_changes;
                                 Status("Buttons reset to the defaults.");
                               }));
    for (size_t i = 0; i < kPadCount; ++i) {
      const Pad physical = static_cast<Pad>(i);
      const Pad target = GetMapping(physical);
      Item it;
      it.type = ItemType::kChoice;
      it.label = GetPadInfo(physical).label;
      it.description = F("What pressing {0} does in the game.", {T(std::string(GetPadInfo(physical).label))});
      it.icon = PadGlyph(physical, &it.icon_aspect);
      it.options.push_back("Nothing");
      for (size_t j = 0; j < kPadCount; ++j) it.options.push_back(GetPadInfo(static_cast<Pad>(j)).label);
      it.index = target == Pad::kNone ? 0 : int(target) + 1;
      if (target != physical) it.value_color = color::kAccent;  // changed from the default
      it.on_choice = [physical](int k) {
        SetMapping(physical, k == 0 ? Pad::kNone : static_cast<Pad>(k - 1));
        ++g_changes;
      };
      it.on_default = [physical] {
        SetMapping(physical, physical);
        ++g_changes;
      };
      it.default_text = GetPadInfo(physical).label;
      items.push_back(std::move(it));
    }
    return items;
  }

  std::vector<Item> KeyItems() {
    std::vector<const rex::cvar::FlagEntry*> binds;
    for (auto& e : rex::cvar::GetRegistry())
      if (e.category == "Input/Keybinds/Controller") binds.push_back(&e);
    std::vector<Item> items;
    items.push_back(Section(T("Controls") + std::string("  " KK_DOT "  ") + T("Keyboard bindings")));
    items.push_back(ActionItem("Reset to defaults", "Every key back to its default.", "", [this, binds] {
      for (auto* e : binds) Reset(e->name.c_str());
      Status("Keys reset to the defaults.");
    }));
    auto pretty = [](std::string keys) {  // "Semicolon,Space" -> "Semicolon, Space"
      for (size_t at = 0; (at = keys.find(',', at)) != std::string::npos; at += 2) keys.insert(at + 1, " ");
      return keys;
    };
    // What each button does in the game (the Xbox 360 manual), so the keys make sense.
    static const std::map<std::string, const char*> kActions = {
        {"keybind_a", "In play: Jack calls his companion; Kong hits. In menus: select."},
        {"keybind_b", "In play: Jack checks his reserve bullets; Kong jumps, dodges and climbs. In menus: back."},
        {"keybind_x", "In play: Kong grabs and throws, and picks up and puts down Ann."},
        {"keybind_y", "In play: Jack drops his spear; Kong pushes back, bites and goes into fury mode."},
        {"keybind_left_trigger", "In play: Jack takes aim (raises his gun)."},
        {"keybind_right_trigger", "In play: Jack shoots, takes, uses and repels."},
        {"keybind_left_shoulder", "Not used in play."},
        {"keybind_right_shoulder", "In play: Jack reloads."},
        {"keybind_lstick_up", "Moves Jack and Kong."},
        {"keybind_lstick_down", "Moves Jack and Kong."},
        {"keybind_lstick_left", "Moves Jack and Kong."},
        {"keybind_lstick_right", "Moves Jack and Kong."},
        {"keybind_lstick_press", "In play: Jack crouches."},
        {"keybind_rstick_up", "Turns the camera when Mouse camera is off."},
        {"keybind_rstick_down", "Turns the camera when Mouse camera is off."},
        {"keybind_rstick_left", "Turns the camera when Mouse camera is off."},
        {"keybind_rstick_right", "Turns the camera when Mouse camera is off."},
        {"keybind_rstick_press", "In play: Jack zooms in."},
        {"keybind_dpad_up", "Moves around the game's menus."},
        {"keybind_dpad_down", "Moves around the game's menus."},
        {"keybind_dpad_left", "Moves around the game's menus."},
        {"keybind_dpad_right", "Moves around the game's menus."},
        {"keybind_start", "Opens the pause menu."},
        {"keybind_back", "Not used in play."},
        {"keybind_guide", "Not used in play."},
    };
    for (auto* e : binds) {
      const std::string value = pretty(e->getter());
      std::string desc = T("Select, then press the key or mouse button to use. Esc cancels.");
      if (auto a = kActions.find(e->name); a != kActions.end()) desc = std::string(T(a->second)) + " " + desc;
      Item it = ActionItem(e->description, desc,
                           value.empty() ? "None" : value, [this, name = e->name, label = e->description] {
                             capturing_ = name;
                             ui::Modal m;
                             m.open = true;
                             m.title = "Press a key or mouse button";
                             m.body = F("For {0}. Esc cancels.", {T(label)});
                             m.buttons = {"Cancel"};
                             m.cancel = 0;
                             m.on_button = [this](int) { capturing_.clear(); };
                             modal_ = std::move(m);
                           });
      it.value_color = color::kText;
      it.on_default = [name = e->name] { Reset(name.c_str()); };
      it.default_text = e->default_value.empty() ? "None" : pretty(e->default_value);
      items.push_back(std::move(it));
    }
    return items;
  }

  // Waits for a key while a binding is being captured.
  void CaptureKey() {
    if (capturing_.empty()) return;
    if (!modal_.open) {  // closed with the controller
      capturing_.clear();
      return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      capturing_.clear();
      modal_.open = false;
      input_.ConsumeAll();
      return;
    }
    // A mouse button, unless it's a click on the pop-up's own button (Cancel).
    static constexpr std::pair<ImGuiMouseButton, const char*> kMouse[] = {
        {ImGuiMouseButton_Left, "LMB"}, {ImGuiMouseButton_Right, "RMB"}, {ImGuiMouseButton_Middle, "MMB"}};
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    bool on_button = false;
    for (const auto& [a, b] : modal_.button_rects)
      on_button = on_button || (mouse.x >= a.x && mouse.y >= a.y && mouse.x < b.x && mouse.y < b.y);
    for (const auto& [button, name] : kMouse) {
      if (ImGui::GetFrameCount() <= modal_.shown_frame + 1 || !ImGui::IsMouseClicked(button)) continue;
      if (button == ImGuiMouseButton_Left && on_button) continue;
      Set(capturing_.c_str(), name);
      capturing_.clear();
      modal_.open = false;
      input_.ConsumeAll();
      return;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
      const auto key = static_cast<ImGuiKey>(k);
      if (key == ImGuiKey_Escape || !ImGui::IsKeyPressed(key, false)) continue;
      const int vk = ImGuiKeyToVk(key);
      if (!vk) continue;
      const std::string name = rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(vk));
      if (!name.empty()) Set(capturing_.c_str(), name);
      capturing_.clear();
      modal_.open = false;
      input_.ConsumeAll();
      return;
    }
  }

  // ------------------------------------------------------------- Cheats ---
  std::vector<Item> CheatItems() {
    std::vector<Item> items;
    items.push_back(Section("Cheats"));
    items.push_back(CvarToggle("Cheats",
                               "Switches on the cheats you pick below when a chapter starts, the same as typing their "
                               "codes on the game's Cheat screen. Changes made while playing apply at once. They "
                               "last until you quit the game.",
                               "kk_cheats"));
    const bool on = GetBool("kk_cheats");
    items.push_back(Section("Cheat list"));
    for (const auto& cheat : kCheats) {
      // Every chapter is open in the Play menu's Chapter select (docs/menus.md): no "All chapters".
      if (std::string_view(cheat.id) == "chapters") continue;
      Item it = CvarToggle(cheat.label, "One of the game's own cheats.", std::string("kk_cheat_") + cheat.id);
      it.facts.push_back(F("Code: {0}", {cheat.code}));
      items.push_back(std::move(Disable(it, !on, "Switch Cheats on to use it.")));
    }
    return items;
  }

  // Graphics > Advanced: Rebuild shaders (the launcher's only: the game uses them while it runs). The same
  // job as Home's shader status, shown on the shader screen with its one progress bar.
  std::function<void()> rebuild_shaders_;
  void RebuildShadersItem(std::vector<Item>& items) {
    if (in_game_ || !rebuild_shaders_ || !NativeShaderPack::Wanted()) return;
    items.push_back(ActionItem("Rebuild shaders",
                               "Prepares all of the game's shaders again, for Vulkan and DirectX 12. It takes a few "
                               "minutes. It can help after a graphics driver update, or if the game shows graphics "
                               "problems.",
                               "", rebuild_shaders_));
  }

  // A built page against settings_layout.h (`layout_page`: its settings_layout::Page).
  static bool CheckLayout(int layout_page, const std::vector<Item>& items, std::string* why) {
    std::vector<settings_layout::Built> rows;
    for (const Item& it : items) rows.push_back({it.type == ItemType::kSection, it.label});
    return settings_layout::InLayoutOrder(settings_layout::Page(layout_page), rows, why);
  }
#if defined(KK_DEV_TOOLS)
  // Developer builds log each page's check once per outcome ("KK dev: layout ... ok" or "FAIL").
  void DevCheckLayout(int layout_page, const std::vector<Item>& items, const char* where) {
    std::string why;
    const bool ok = CheckLayout(layout_page, items, &why);
    if (!layout_logged_.insert(std::string(where) + std::to_string(layout_page) + why).second) return;
    if (ok)
      REXLOG_INFO("KK dev: layout {} {} ok ({} rows)", where, settings_layout::kPageNames[layout_page], items.size());
    else
      REXLOG_ERROR("KK dev: layout {} {} FAIL {}", where, settings_layout::kPageNames[layout_page], why);
  }
  std::set<std::string> layout_logged_;
#endif

  // In game: the rows that change while the game runs. Left out (they need a restart, or only make
  // sense before the game starts): the window, resolution, refresh rate and monitor, the frame rate
  // cap and VSync, the upscaler and render quality, the look and preset, the renderer, the language
  // and the startup logos. Sections left empty go too.
  static std::vector<Item> LiveOnly(std::vector<Item> items) {
    static const std::set<std::string> kRestartOnly = {
        "Window mode", "Window size", "Resolution", "Refresh rate", "Monitor", "Frame rate", "VSync",
        "Upscaler", "Upscaler mode", "Render quality", "Internal resolution", "Look", "Preset", "Renderer",
          "Graphics API", "Language", "Startup logos"};
    std::vector<Item> out;
    for (Item& it : items) {
      if (it.type != ItemType::kSection && kRestartOnly.count(it.label)) continue;
      if (it.disabled_note == kSetByOriginal) it.disabled_note = kSetByOriginalInGame;
      out.push_back(std::move(it));
    }
    std::vector<Item> kept;
    for (size_t i = 0; i < out.size(); ++i) {
      const bool empty_section = out[i].type == ItemType::kSection &&
                                 (i + 1 == out.size() || out[i + 1].type == ItemType::kSection);
      if (!empty_section) kept.push_back(std::move(out[i]));
    }
    return kept;
  }
  static constexpr const char* kSetByOriginalInGame =
      "Set by the Original Xbox 360 look. Change Look in the launcher to set it.";

  double preview_at_ = 0;
  std::vector<std::filesystem::path> sounds_;
  double sounds_scanned_ = -10.0;
  void PreviewAudio() {
    if (preview_at_ > 0 && ImGui::GetTime() >= preview_at_ && !ImGui::IsMouseDown(0)) {
      preview_at_ = 0;
      PlayAchievementSound(profile_user_dir_);
    }
  }
  LauncherCallbacks cb_;
  ui::Input input_;
  ui::Glyphs glyphs_;
  ui::Modal modal_;
  SubPage sub_ = kNoSubPage;
  std::string status_;
  double status_time_ = -100;
  std::string capturing_;
  std::vector<MonitorInfo> monitors_;
  std::vector<DisplayMode> display_modes_;  // the Resolution choices, for display_modes_monitor_
  int display_modes_monitor_ = -1;
  const bool in_game_;  // drawn by the pause menu
  std::filesystem::path profile_user_dir_;
  int profile_slot_ = -1;
  std::vector<play::SaveSlot> profile_saves_;
  double profile_saves_at_ = -1;
  play::ProfileOptions profile_read_;
  double profile_read_at_ = -1;
  // Changes waiting to be written to every save (FlushProfileOptions).
  std::array<float, size_t(play::ProfileOption::kCount)> profile_values_{};
  std::array<bool, size_t(play::ProfileOption::kCount)> profile_dirty_{};
  std::array<bool, size_t(play::ProfileOption::kCount)> profile_waiting_{};
  double profile_changed_at_ = 0;
  play::ExtraProgress extra_progress_;
  int extra_progress_slot_ = -2;
  double extra_progress_at_ = -1;
};

class Launcher final : public rex::ui::ImGuiDialog, private SettingsPages {
 public:
  Launcher(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, LauncherPaths paths,
           LauncherCallbacks callbacks)
      : ImGuiDialog(drawer),
        SettingsPages(immediate, std::move(callbacks), /*in_game=*/false, paths.user_dir),
        immediate_(immediate),
        paths_(std::move(paths)) {
    glyphs_.AddSet("xbox360", art::LauncherArtDir(paths_.user_dir) / "glyphs" / "xbox360");
    for (const char* name : kRestartCvars) restart_baseline_.push_back(Get(name));
    files_ok_ = GameFilesPresent(paths_.game_dir);
    home_focus_ = 0;
    rebuild_shaders_ = [this] { OpenRebuildShaders(); };
    EnforceOriginalLook();
    // Which temporal upscalers this PC runs (Display > Upscaling): asked now, in the background.
    ApplyRendererChoice();      // the renderer decided, the default upscaler follows it
    OfferedTemporalUpscalers();
    LoadArt();
    // Installed before the launcher took its art from the game files: take it now.
    if (files_ok_ && !art::HasLauncherArt(ArtDir())) StartArtExtraction(false);
    // The native renderer's shaders (native_shaders.h): checked each time the launcher opens, built
    // in the background from the game files when missing or stale, and built again in full after
    // every update (a version this launcher hasn't run: an update, a beta, a channel switch), shown by
    // the after-update screen (DrawShaderScreen). Not on a fresh install: its setup builds them.
    if (files_ok_) {
      std::error_code config_ec;
      const bool first_start = Get("kk_last_version").empty() &&
                               !std::filesystem::exists(paths_.config_path, config_ec);
      const bool updated = UpdatedSinceLastRun();
      native_pack_.Start(CacheDir(), paths_.game_dir, updated);
      if (first_start && NativeShaderPack::Wanted()) {
        // Existing game files with fresh user settings still need the setup's one shader screen.
        setup_shaders_ = true;
        REXLOG_INFO("KK: first start: preparing both native shader packs on the setup screen");
      }
      if (first_start) {
        // ... and its last step, Original or Remastered, as after an install.
        setup_style_ = true;
        style_focus_ = 1;
      }
      if (updated && NativeShaderPack::Wanted()) {
        update_shaders_ = true;
        REXLOG_INFO("KK: first start of v{} (last v{}): the native renderer's shaders are built again", KK_VERSION,
                    Get("kk_last_version"));
      }
    }
    toast_ = std::make_unique<AchievementToast>(drawer, immediate_, paths_.game_dir, paths_.user_dir);
    CleanUpAfterUpdate();
    online::Start();
    discord::Start();
    const bool testing = std::getenv("KK_AUTOPLAY") != nullptr;
    if (GetBool("kk_check_updates") && !testing) StartUpdateCheck(false);
    StartSteamCheck();
    // Menu sounds (the device opens in OnDraw while they're on).
    ui::SetSoundHandler([this](ui::Sound sound, float value) {
      if (!GetBool("kk_launcher_sounds")) return;
      if (!ui_sounds_ && files_ok_) ui_sounds_ = std::make_unique<LauncherSounds>(paths_.game_dir);  // just switched on
      if (!ui_sounds_) return;
      const float v = GetInt("kk_launcher_sounds_volume", 50) / 100.0f;
      ui_sounds_->Play(sound, value, v * v);
    });
    // The shader pack keeps itself up to date: each time the launcher opens,
    // the published pack is downloaded if it is newer than the one installed.
    if (!testing) StartPackDownload();
    // "What's new" once after an update: the settings file says the launcher
    // has run before (a fresh install has none), and an older version ran last.
    const std::string last = Get("kk_last_version");
    if (last != KK_VERSION && !testing) {
      std::error_code ec;
      if (!last.empty() || std::filesystem::exists(paths_.config_path, ec)) {
        whats_new_from_ = last;  // empty: from before the launcher remembered (show this version)
        open_whats_new_ = true;
        REXLOG_INFO("KK: What's new pending (last run v{}): it opens once the game files, shaders and look are ready", last);
      }
      Set("kk_last_version", KK_VERSION);
      SaveSettings(paths_.config_path);
    }
    // Back from the game (Quit game, the end of a chapter-select run): straight to the Play menu
    // or Chapter select.
    const std::string open_on = Get("kk_launcher_open");
    rex::cvar::ResetToDefault("kk_launcher_open");  // never saved
    if (files_ok_ && open_on == "benchmark") {  // back from a benchmark run: its results
      page_ = kAbout;
      open_benchmark_results_ = true;
    }
    if (files_ok_ && (open_on == "play" || open_on == "chapters")) {
      OpenPlayMenu();
      if (open_on == "chapters") OpenPlayView(PlayView::kChapters);
    }
    if (files_ok_ && open_on == "extras") {  // back from an Extras bonus: Play > Extras
      OpenPlayMenu();
      OpenPlayView(PlayView::kExtras);
    }
    saved_changes_ = seen_changes_ = g_changes;
  }

  ~Launcher() override {
    FlushProfileOptions();
    ui::SetSoundHandler(nullptr);
    if (install_thread_.joinable()) {
      progress_.cancel = true;
      install_thread_.join();
    }
    if (pack_thread_.joinable()) pack_thread_.join();
    if (art_thread_.joinable()) art_thread_.join();
    for (auto& texture : textures_) KeepTextureAlive(std::move(texture));
    KeepTextureAlive(std::move(discord_icon_));
    KeepTextureAlive(std::move(steam_icon_));
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    {
      // The frame rate (task 144). The SDK holds the launcher to the display's refresh rate on Windows
      // only: elsewhere it draws as fast as it can (a handheld's battery), so keep it to 60 frames a
      // second there. Once nothing has moved for a second (no input, no page change, no pop-up opening,
      // no notification), only the backdrop's slow drift is left: 30 frames a second draw it the same
      // and save most of the launcher's processor and graphics time while it sits on a page. The first
      // input after that is answered within a frame of 33 ms.
      using clock = std::chrono::steady_clock;
      const bool idle = ImGui::GetTime() - last_activity_ > 1.0;
#if defined(_WIN32)
      const auto frame = idle ? std::chrono::microseconds(33333) : std::chrono::microseconds(0);
#else
      const auto frame = std::chrono::microseconds(idle ? 33333 : 16667);
#endif
      if (frame.count() > 0) {
        if (next_frame_ > clock::now()) std::this_thread::sleep_until(next_frame_);
        next_frame_ = std::max(next_frame_, clock::now() - frame) + frame;
      } else {
        next_frame_ = clock::now();
      }
    }
#if defined(KK_DEV_TOOLS)
    const auto draw_start = std::chrono::steady_clock::now();
#endif
    // Testing aid: KK_AUTOPLAY=1 presses Play after a couple of seconds (120 frames); KK_AUTOPLAY=40
    // after 40 s (60 frames a second), to keep the launcher open as a player might.
    static const char* autoplay = std::getenv("KK_AUTOPLAY");
    static const int autoplay_after = autoplay ? std::max(120, int(std::atof(autoplay) * 60)) : 0;
#if defined(KK_DEV_TOOLS)
    // Exercise the same action as the About button without taking focus or
    // synthesizing desktop input. Restore the previous text clipboard.
    static bool diagnostics_tested = false;
    if (!diagnostics_tested && std::getenv("KK_DEV_DIAGNOSTICS")) {
      diagnostics_tested = true;
      const char* previous = ImGui::GetClipboardText();
      const std::string saved = previous ? previous : "";
      for (const auto& item : AdvancedPageItems()) if (item.label == "Copy diagnostics" && item.on_activate) item.on_activate();
      if (const char* copied = ImGui::GetClipboardText()) {
        std::ofstream file(paths_.user_dir / "diagnostics-clipboard.txt", std::ios::binary);
        file << copied;
      }
      ImGui::SetClipboardText(saved.c_str());
      REXLOG_INFO("KK dev: diagnostics clipboard action exercised; saved crash reports {}", HasCrashReports(paths_.user_dir));
    }
#endif
    if (autoplay && files_ok_ && ++autoplay_frames_ == autoplay_after) StartGame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(color::kBase));
    ImGui::Begin("##kk_launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoNav);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    o_ = vp->Pos;
    w_ = vp->Size.x;
    h_ = vp->Size.y;
    // Laid out for 1280 x 720 and scaled with the window.
    const float s = std::clamp(std::min(w_ / 1280.0f, h_ / 720.0f), 0.7f, 3.0f);
    m_.s = s;
    m_.row_h = 46 * s;
    m_.tall_h = 74 * s;
    m_.section_h = 52 * s;
    if (leaving_) {  // the game is starting in a new process (StartGame): the backdrop only, the music fading
      if (music_) music_->Update(0.0f, io.DeltaTime);
      DrawBackdrop(dl);
      ImGui::End();
      return;
    }

    input_.set_keyboard_enabled(capturing_.empty() && !editing_name_);  // keys go to the binding or the name
    input_.Update();
    if (ui_sounds_) {
      const float volume = GetBool("kk_launcher_sounds") ? GetInt("kk_launcher_sounds_volume", 50) / 100.0f : 0.0f;
      ui_sounds_->SetVolume(volume * volume);
    }
    if (pending_click_ != Action::kCount) {
      input_.Inject(pending_click_, ui::Device::kKeyboard);
      pending_click_ = Action::kCount;
    }
#if defined(KK_DEV_TOOLS)
    DevTour();
    DevInput();
#endif
    Tick();
    // A fallback the player has not been told about: shown once, then the launcher no longer opens for it.
    if (!recovery_shown_ && !modal_.open && startup::NeedsNotice()) {
      recovery_shown_ = true;
      std::string failed, selected;
      startup::NoticeApis(failed, selected);
      const std::string recovery =
          F("{0} did not start correctly on this PC, so the game now uses {1}. You can keep it or try {0} "
            "again.", {failed, selected}) +
          "\n\n" + F("Details: {0}", {startup::Message()});
      startup::NoticeShown();
      OpenModal("Renderer startup recovery", recovery,
                {"Continue with safe renderer", "Try native again"}, 0, [this](int i) {
                  if (i == 1 && startup::TryNativeAgain()) ++g_changes;
                });
    }
    if (files_ok_ && !music_) music_ = std::make_unique<LauncherMusic>(paths_.game_dir);
    if (music_) {
      // Silent while the native renderer's shaders are built (setup, after an update, before Play): the
      // build takes nearly every hardware thread and the music would stutter. It fades back in after.
      const bool on = GetBool("kk_launcher_music") && !native_pack_.Working();
      const float v = on ? GetInt("kk_launcher_music_volume", 50) / 100.0f : 0.0f;
      music_->Update(v * v, io.DeltaTime);  // a gentler curve than linear
    }
    if (files_ok_ && GetBool("kk_launcher_sounds") && !ui_sounds_)
      ui_sounds_ = std::make_unique<LauncherSounds>(paths_.game_dir);

    DrawBackdrop(dl);
    if (ShaderScreen()) {
      DrawShaderScreen(dl);
    } else if (InSetup()) {
      DrawSetup(dl);
    } else {
      if (open_benchmark_results_ && !modal_.open) {
        open_benchmark_results_ = false;
        OpenBenchmarkResults();
      }
      if (!modal_.open) HandleGlobalInput();
      if (play_ != PlayView::kNone) {
        DrawPlayHeader(dl);
        if (play_ == PlayView::kMenu) DrawPlayMenu(dl);
        else DrawPlayPage(dl);
      } else {
        DrawTopBar(dl);
        if (page_ == kHome) DrawHome(dl);
        else DrawSettings(dl);
      }
    }
    ui::DrawModal(dl, modal_, o_, ImVec2(o_.x + w_, o_.y + h_), m_, input_);
    DrawBottomBar(dl);  // over a pop-up's shade: its prompts stay clear
    ImGui::End();
    NoteActivity(io);
    handoff::ContentDrawn();  // started by the game going back: it closes once this page is on screen
#if defined(KK_DEV_TOOLS)
    DevPerf(draw_start);
#endif
  }

  // What keeps the launcher at its full frame rate (OnDraw): input of any kind, a page, view, pop-up or
  // setup step changing, a notification showing, the shader job's bar moving.
  void NoteActivity(const ImGuiIO& io) {
    bool active = input_.mouse_moved() || io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
                  io.InputQueueCharacters.Size > 0 || ImGui::IsAnyMouseDown() || !capturing_.empty();
    for (int a = 0; a < int(Action::kCount) && !active; ++a) active = input_.Pressed(Action(a)) || input_.HeldFor(Action(a)) > 0;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !active; ++k) active = ImGui::IsKeyDown(ImGuiKey(k));
    const int view = int(page_) * 16 + int(play_) * 4 + int(sub_) + (modal_.open ? 1000 : 0) + SetupStage() * 10000 +
                     (ShaderScreen() ? 100000 : 0) + (leaving_ ? 1000000 : 0);
    active = active || view != last_view_ || (toast_ && toast_->NeedsContinuousPaint()) || native_pack_.Working() ||
             installing_ || art_progress_.busy || (update_ && update_->busy) || status_focus_ != last_status_focus_;
    last_view_ = view;
    last_status_focus_ = status_focus_;
    if (active) last_activity_ = ImGui::GetTime();
  }

#if defined(KK_DEV_TOOLS)
  // Developer: KK_DEV_LAUNCHER_PERF=1 logs the launcher's own drawing cost every two seconds (frames, the
  // processor time of OnDraw on average and at most) and how long after the process started it first drew.
  void DevPerf(std::chrono::steady_clock::time_point start) {
    static const bool on = std::getenv("KK_DEV_LAUNCHER_PERF") != nullptr;
    if (!on) return;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    static int frames = 0;
    static double total = 0, worst = 0, since = -1;
    static bool first = true;
    if (first) {
      first = false;
#if defined(_WIN32)
      FILETIME created, exited, kernel, user, now_ft;
      GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
      GetSystemTimeAsFileTime(&now_ft);
      auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
      REXLOG_INFO("KK dev: launcher perf: first frame {:.0f} ms after the process started",
                  double(ticks(now_ft) - ticks(created)) / 10000.0);
#endif
    }
    ++frames;
    total += ms;
    worst = std::max(worst, ms);
    const double now = ImGui::GetTime();
    if (since < 0) since = now;
    if (now - since >= 2.0) {
      REXLOG_INFO("KK dev: launcher perf: {} frames in {:.1f} s ({:.0f} a second), OnDraw {:.2f} ms average, {:.2f} ms "
                  "worst, idle {}",
                  frames, now - since, frames / (now - since), total / frames, worst, now - last_activity_ > 1.0);
      frames = 0;
      total = worst = 0;
      since = now;
    }
  }
#endif

 private:
  float S() const { return m_.s; }
  float Margin() const { return 64 * m_.s; }

  // A value read from disk at most every `seconds` (the pages are built every frame; task 144: some read a
  // file or a folder on every frame).
  template <class T, class F>
  const T& Cached(std::pair<double, T>& slot, double seconds, F&& read) {
    const double now = ImGui::GetTime();
    if (now - slot.first >= seconds) {
      slot.second = read();
      slot.first = now;
    }
    return slot.second;
  }
  std::pair<double, bool> crash_reports_cache_{-1e9, false}, launcher_art_cache_{-1e9, false};
  std::map<std::string, std::pair<double, bool>> port_achievement_cache_;

  // ---------------------------------------------------------------- art ---
  rex::ui::ImmediateTexture* MakeTexture(const art::Image& img) {
    if (!img || !immediate_) return nullptr;
    textures_.push_back(immediate_->CreateTexture(uint32_t(img.width), uint32_t(img.height),
                                                  rex::ui::ImmediateTextureFilter::kLinear, false,
                                                  img.rgba.data()));
    return textures_.back().get();
  }

  std::filesystem::path ArtDir() const { return art::LauncherArtDir(paths_.user_dir); }

  // The artwork: the game's logo and stills from the movie, taken from the
  // game files; without them, the capture of the title screen from the first
  // play.
  void LoadArt() {
    slides_.clear();
    logo_ = nullptr;
    still_low_ = nullptr;
    if (art::HasLauncherArt(ArtDir())) {
      const art::LauncherArt a = art::LoadLauncherArt(ArtDir());
      if (a.logo) {
        logo_ = MakeTexture(a.logo);
        logo_aspect_ = float(a.logo.width) / float(a.logo.height);
      }
      for (const art::Image& img : a.backdrops) AddSlide(img, SoftCopy(img));
    }
    if (slides_.empty()) {
      if (auto img = art::LoadImage(art::TitleCapturePath(paths_.user_dir)); img && img.width >= 64) {
        art::Image sharp, soft;
        MakeBackdrops(img, &sharp, &soft);
        AddSlide(sharp, soft);
      }
    }
    title_icon_ = MakeTexture(art::LoadImage(art::TitleIconPath(paths_.game_dir)));
    LoadAchievements();
  }

  // Loads the art again (after the install, or after it was extracted).
  void ReloadArt() {
    for (auto& texture : textures_) KeepTextureAlive(std::move(texture));
    textures_.clear();
    chapter_pictures_.clear();
    chapter_aspects_.clear();
    title_icon_ = glow_ = nullptr;
    glyphs_.Reload();
    LoadArt();
  }

  // A soft moonlight glow (smooth falloff, no bands) for the night sky.
  static art::Image Glow() {
    constexpr int kSize = 256;
    art::Image img;
    img.width = img.height = kSize;
    img.rgba.resize(size_t(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y)
      for (int x = 0; x < kSize; ++x) {
        const float dx = (x + 0.5f) / kSize * 2 - 1, dy = (y + 0.5f) / kSize * 2 - 1;
        const float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
        const float a = 0.30f * std::exp(-d * d * 4.5f) * (1 - d * d);
        uint8_t* p = &img.rgba[(size_t(y) * kSize + x) * 4];
        p[0] = 160;
        p[1] = 178;
        p[2] = 200;
        const uint32_t hash = uint32_t(x * 73856093) ^ uint32_t(y * 19349663);
        const float dither = float(hash % 1024) / 1024.0f;  // breaks up the 8-bit steps
        p[3] = uint8_t(std::clamp(a * 255 + dither, 0.0f, 255.0f));
      }
    return img;
  }

  void AddSlide(const art::Image& sharp, const art::Image& soft) {
    if (!sharp) return;
    if (slides_.empty()) still_low_ = MakeTexture(Shrink(sharp, 3));  // the setup's Original card
    slides_.push_back({MakeTexture(sharp), MakeTexture(soft ? soft : sharp), float(sharp.width) / float(sharp.height)});
  }

  // Takes the logo and stills from the game files on a worker thread; during
  // the first install it's the setup screen's last step.
  void StartArtExtraction(bool during_setup) {
    if (art_progress_.busy) return;
    if (art_thread_.joinable()) art_thread_.join();
    art_progress_.busy = true;
    art_progress_.done = art_progress_.failed = false;
    setup_art_ = during_setup;
    art_thread_ = std::thread([this, game = paths_.game_dir, dir = ArtDir()] {
      art::ExtractLauncherArt(game, dir, art_progress_);
    });
  }

  void FinishArtIfDone() {
    if (art_progress_.busy || !art_thread_.joinable()) return;
    art_thread_.join();
    if (art_progress_.done) {
      ReloadArt();
      slide_clock_ = 0;
    } else {
      REXLOG_WARN("KK: launcher art: {}", art_progress_.message);
    }
    if (setup_art_) {
      setup_art_ = false;
      // With the native renderer, the shaders from the new game files; then, last, the look.
      if (NativeShaderPack::Wanted()) {
        native_pack_.Start(CacheDir(), paths_.game_dir);
        setup_shaders_ = true;
      }
      setup_style_ = true;
      style_focus_ = 1;
      page_ = kHome;
      home_focus_ = 0;
      home_mix_ = 0;  // the art fades in
    }
  }

  void LoadAchievements() {
    achievements_.clear();
    have_achievement_names_ = false;
    const auto icons = art::AchievementIcons(paths_.game_dir);
    std::map<uint32_t, Achievement> by_id;
    for (auto& [id, path] : icons) by_id[id].id = id;
    try {
      // Names: the list extracted from the disc, else the one the game wrote on first play.
      std::error_code exists_ec;
      auto source = art::AchievementDir(paths_.game_dir) / "achievements.toml";
      if (!std::filesystem::exists(source, exists_ec)) source = art::AchievementCachePath(paths_.user_dir);
      auto table = toml::parse_file(source.string());
      if (auto* list = table["achievements"].as_array()) {
        for (auto& node : *list) {
          auto* e = node.as_table();
          if (!e) continue;
          const uint32_t id = uint32_t((*e)["id"].value_or<int64_t>(0));
          auto& a = by_id[id];
          a.id = id;
          a.label = (*e)["label"].value_or<std::string>("");
          a.description = (*e)["description"].value_or<std::string>("");
          a.unachieved = (*e)["unachieved_description"].value_or<std::string>("");
          a.gamerscore = uint32_t((*e)["gamerscore"].value_or<int64_t>(0));
        }
        have_achievement_names_ = !list->empty();
      }
    } catch (...) {
    }
    try {
      auto unlocks = toml::parse_file(art::AchievementUnlockPath(paths_.user_dir).string());
      if (auto* t = unlocks["unlocked"].as_table())
        for (auto& [key, value] : *t) by_id[uint32_t(std::stoul(std::string(key.str())))].unlocked = true;
    } catch (...) {
    }
    for (auto& [id, a] : by_id) {
      if (id == 0) continue;
      if (auto it = icons.find(id); it != icons.end()) a.icon = MakeTexture(art::LoadImage(it->second));
      achievements_.push_back(a);
    }
  }

  // The stills behind everything, one after another (each drifts slowly
  // closer while it's up): clear on Home, blurred and darker behind the
  // settings, cross-fading between the two.
  static constexpr float kSlideTime = 10.0f, kFadeTime = 2.0f;

  void DrawBackdrop(ImDrawList* dl) {
    const float s = S();
    const bool home = page_ == kHome && !InSetup() && (play_ == PlayView::kNone || play_ == PlayView::kMenu);
    home_mix_ = ui::Approach(home_mix_ * 100.0f, home ? 100.0f : 0.0f, 9.0f) / 100.0f;
    slide_clock_ += std::min(ImGui::GetIO().DeltaTime, 0.1f);
    const ImVec2 p1(o_.x + w_, o_.y + h_);
    if (!slides_.empty() && !PlainSetup()) {
      const int n = int(slides_.size());
      const int cur = int(slide_clock_ / kSlideTime) % n, next = (cur + 1) % n;
      const float into = float(std::fmod(slide_clock_, double(kSlideTime)));
      const float fade = n > 1 ? std::clamp((into - (kSlideTime - kFadeTime)) / kFadeTime, 0.0f, 1.0f) : 0.0f;
      auto draw = [&](int i, float age, bool soft, float alpha) {
        const Slide& sl = slides_[size_t(i)];
        // Fill the window, cropping the still's sides or top and bottom.
        const float view = w_ / h_;
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        if (view > sl.aspect) {
          const float v = sl.aspect / view;
          v0 = (1 - v) * 0.5f;
          v1 = v0 + v;
        } else {
          const float u = view / sl.aspect;
          u0 = (1 - u) * 0.5f;
          u1 = u0 + u;
        }
        // Closer over time, towards the right (the left is under the menu).
        const float zoom = 1.0f + 0.07f * std::clamp(age / (kSlideTime + kFadeTime), 0.0f, 1.0f);
        const float cu = (u0 + u1) * 0.5f, cv = (v0 + v1) * 0.5f;
        const float fu = cu + (0.62f - cu) * (1 - 1 / zoom), fv = cv + ((i % 2 ? 0.42f : 0.55f) - cv) * (1 - 1 / zoom);
        const float hu = (u1 - u0) * 0.5f / zoom, hv = (v1 - v0) * 0.5f / zoom;
        dl->AddImage(ui::Tex(soft ? sl.soft : sl.sharp), o_, p1, ImVec2(fu - hu, fv - hv), ImVec2(fu + hu, fv + hv),
                     ui::WithAlpha(IM_COL32_WHITE, alpha));
      };
      const float age_next = into - (kSlideTime - kFadeTime);
      draw(cur, into + kFadeTime, true, 1.0f);
      if (fade > 0) draw(next, age_next, true, fade);
      if (home_mix_ > 0.01f) {
        draw(cur, into + kFadeTime, false, home_mix_);
        if (fade > 0) draw(next, age_next, false, home_mix_ * fade);
      }
    } else {
      // Nothing yet (before the install): night sky with the moon's glow.
      dl->AddRectFilledMultiColor(o_, p1, IM_COL32(22, 28, 36, 255), IM_COL32(26, 33, 42, 255),
                                  color::kBase, color::kBase);
      if (!glow_) glow_ = MakeTexture(Glow());
      const ImVec2 moon(o_.x + w_ * 0.76f, o_.y + h_ * 0.28f);
      const float r = h_ * 0.62f;
      dl->AddImage(ui::Tex(glow_), ImVec2(moon.x - r, moon.y - r), ImVec2(moon.x + r, moon.y + r));
    }
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float hm = home_mix_;
    dl->AddRectFilled(o_, p1, ui::WithAlpha(color::kBase, PlainSetup() ? 0.1f : lerp(0.80f, 0.16f, hm)));
    // Darker on the left for the menu and the list, and at the top and bottom
    // for the tabs and prompts.
    const ImU32 c0 = ui::WithAlpha(color::kBase, lerp(0.30f, 0.88f, hm)), c1 = ui::WithAlpha(color::kBase, 0.0f);
    dl->AddRectFilledMultiColor(o_, ImVec2(o_.x + w_ * 0.72f, p1.y), c0, c1, c1, c0);
    const ImU32 t0 = ui::WithAlpha(color::kBase, lerp(0.5f, 0.9f, hm));  // the tabs stay clear over bright skies
    dl->AddRectFilledMultiColor(o_, ImVec2(p1.x, o_.y + 180 * s), t0, t0, c1, c1);
    const ImU32 b0 = ui::WithAlpha(color::kBase, lerp(0.55f, 0.85f, hm));
    dl->AddRectFilledMultiColor(ImVec2(o_.x, p1.y - 180 * s), p1, c1, c1, b0, b0);
  }

  // ------------------------------------------------------------ top bar ---
  // The top of every page but the setup's: on Settings the six categories as tabs (LB and RB, or a click,
  // move between them) with SETTINGS at the right; on Achievements and About the page's name; on Home
  // nothing but the online player count. The Play pages have their own (DrawPlayHeader).
  void DrawTopBar(ImDrawList* dl) {
    const float s = S();
    const online::Counts counts = online::Get();
    const bool show_counts = counts.known && !InSetup() && page_ == kHome;
    const ui::Fonts f = ui::GetFonts();
    const float right = o_.x + w_ - Margin();
    const float ly = std::round(o_.y + 74 * s);
    if (page_ != kHome) {
      const std::string caption = IsSettingsPage(page_) ? Upper(T("Settings")) : std::string("KING KONG");
      const float caption_fs = 15 * s, caption_tr = 4 * s;
      const float caption_width = ui::TrackedWidth(f.display, caption_fs, caption, caption_tr);
      float ux = 0, uw = 0;
      if (IsSettingsPage(page_)) {
        if (tab_names_lang_ != int(text::Current()) || tab_names_.empty()) {  // made once per language
          tab_names_.clear();
          for (int p = kFirstSettingsPage; p <= kLastSettingsPage; ++p) tab_names_.push_back(Upper(T(kPageNames[p])));
          tab_names_lang_ = int(text::Current());
        }
        const int clicked = ui::DrawTabs(dl, glyphs_, PromptSet(), tab_names_, int(page_) - kFirstSettingsPage,
                                         ImVec2(o_.x + Margin(), o_.y + 44 * s), m_, !modal_.open, &ux, &uw,
                                         w_ - Margin() * 2 - caption_width - 32 * s);
        if (clicked >= 0 && clicked != int(page_) - kFirstSettingsPage) {
          ui::Cue(ui::Sound::kTab);
          GoTo(Page(kFirstSettingsPage + clicked));
        }
      } else {
        const float fs = 15 * s, tr = 1.6f * s, cy = o_.y + 44 * s;
        const std::string name = Upper(T(kPageNames[page_]));
        ui::DrawTracked(dl, f.display, fs, ImVec2(o_.x + Margin(), cy - fs * 0.5f), color::kText, name, tr);
        ux = o_.x + Margin();
        uw = ui::TrackedWidth(f.display, fs, name, tr);
      }
      if (tab_x_ < 0) {
        tab_x_ = ux;
        tab_w_ = uw;
      }
      tab_x_ = ui::Approach(tab_x_, ux, 22.0f);
      tab_w_ = ui::Approach(tab_w_, uw, 22.0f);
      dl->AddLine(ImVec2(o_.x + Margin(), ly), ImVec2(o_.x + w_ - Margin(), ly), color::kHairline, 1.0f);
      dl->AddRectFilled(ImVec2(tab_x_, ly - 2 * s), ImVec2(tab_x_ + tab_w_, ly), color::kAccent);
      ui::DrawTracked(dl, f.display, caption_fs, ImVec2(right - caption_width, o_.y + 44 * s - caption_fs * 0.5f),
                      color::kTextDim, caption, caption_tr);
      return;
    }
    // The online player count, while it's on and the server has answered.
    if (show_counts) {
      const std::string text = Upper(F("Players online: {0}", {std::to_string(counts.online)}));
      const float fs = 13 * s, tr = 2.2f * s, cy = o_.y + 44 * s;
      const float tw = ui::TrackedWidth(f.display, fs, text, tr);
      ui::DrawTracked(dl, f.display, fs, ImVec2(right - tw, cy - fs * 0.5f), color::kTextDim, text, tr);
      dl->AddCircleFilled(ImVec2(right - tw - 12 * s, cy), 3.5f * s, color::kGood, 16);
    }
  }

  void GoTo(Page page) {
    if (page == kDisplay && page_ != kDisplay) {
      monitors_.clear();
      display_modes_monitor_ = -1;
      LoadDisplayModes();
    }
    if (page != page_) sub_ = kNoSubPage;
    if (IsSettingsPage(page)) settings_page_ = page;
    extras_return_focus_ = -1;  // only the Graphics visit an Extras row opened returns there
    side_focus_ = -1;
    play_ = PlayView::kNone;
    page_ = page;
  }

  // ---------------------------------------------------------- bottom bar ---
  void DrawBottomBar(ImDrawList* dl) {
    const float s = S();
    const float cy = o_.y + h_ - 42 * s;
    const ui::Fonts f = ui::GetFonts();
    const double now = ImGui::GetTime();
    if (!status_.empty() && now - status_time_ < 6.0) {
      const float a = float(std::clamp((6.0 - (now - status_time_)) / 0.6, 0.0, 1.0));
      dl->AddText(f.text, 15 * s, ImVec2(o_.x + Margin(), cy - 9 * s), ui::WithAlpha(color::kTextDim, a),
                  T(status_).c_str());
    } else if (now - saved_time_ < 1.6) {
      const float a = float(std::clamp((1.6 - (now - saved_time_)) / 0.5, 0.0, 1.0));
      dl->AddText(f.text, 15 * s, ImVec2(o_.x + Margin(), cy - 9 * s), ui::WithAlpha(color::kTextFaint, a),
                  T("Settings saved"));
    }
    const Action clicked = ui::DrawPrompts(dl, glyphs_, PromptSet(), Prompts(), o_.x + w_ - Margin(), cy, m_,
                                           true);
    if (clicked != Action::kCount) pending_click_ = clicked;
  }

  std::vector<ui::Prompt> Prompts() const {
    std::vector<ui::Prompt> p;
    if (modal_.open) {
      if (!capturing_.empty()) return {{Action::kBack, "Cancel"}};
      if (!modal_.reader.empty()) p.push_back({Action::kUp, "Scroll"});
      if (!modal_.buttons.empty()) p.push_back({Action::kAccept, "Select"});
      if (modal_.cancel >= 0) p.push_back({Action::kBack, "Back"});
      return p;
    }
    if (ShaderScreen()) return {{Action::kAccept, "Continue in the background"}};
    if (InSetup()) {
      switch (SetupStage()) {
        case 0:
          if (side_focus_ >= 0) return {{Action::kAccept, SidePanelVerb(false)}, {Action::kBack, "Back"}};
          if (menu_value_) return {{Action::kLeft, "Change"}, {Action::kAccept, "Select"}, {Action::kBack, "Quit"}};
          return {{Action::kAccept, "Select"}, {Action::kBack, "Quit"}};
        case 1: return {{Action::kBack, "Cancel"}};
        case 5: return {{Action::kLeft, "Change"}, {Action::kAccept, "Select"}};
        default: return {};
      }
    }
    if (play_ != PlayView::kNone) return PlayPrompts();
    if (page_ == kHome) {
      if (status_focus_) return {{Action::kAccept, "Rebuild shaders"}, {Action::kBack, "Back"}};
      if (side_focus_ < 0 && menu_value_) p.push_back({Action::kLeft, "Change"});
      p.push_back({Action::kAccept, side_focus_ >= 0 ? SidePanelVerb(true) : "Select"});
      p.push_back({Action::kBack, side_focus_ >= 0 ? "Back" : "Quit"});
      return p;
    }
    if (focused_) {
      switch (focused_->type) {
        case ItemType::kChoice:
        case ItemType::kSlider:
          if (!focused_->disabled) p.push_back({Action::kLeft, "Change"});
          break;
        case ItemType::kAction:
        case ItemType::kLink:
          if (!focused_->disabled && focused_->on_activate) p.push_back({Action::kAccept, "Select"});
          break;
        default:
          break;
      }
      if (focused_->on_default && !focused_->disabled) p.push_back({Action::kDefault, "Default"});
    }
    if (sub_ == kNoSubPage && !PageSettings(page_).empty()) p.push_back({Action::kResetPage, "Reset page"});
    if (CanPlay()) p.push_back({Action::kPlay, "Play"});
    p.push_back({Action::kBack, "Back"});
    return p;
  }

  // ------------------------------------------------------- global input ---
  void HandleGlobalInput() {
    ui::Input& in = input_;
    if (play_ != PlayView::kNone) return HandlePlayInput();
    if (in.Pressed(Action::kPrevTab) || in.Pressed(Action::kNextTab)) {
      // LB and RB move between the settings categories (only on Settings).
      if (IsSettingsPage(page_)) {
        const int dir = in.Pressed(Action::kNextTab) ? 1 : -1, n = kLastSettingsPage - kFirstSettingsPage + 1;
        ui::Cue(ui::Sound::kTab);
        GoTo(Page(kFirstSettingsPage + (int(page_) - kFirstSettingsPage + dir + n) % n));
      }
      in.Consume(Action::kPrevTab);
      in.Consume(Action::kNextTab);
    }
    if (in.Pressed(Action::kPlay)) {  // Start (P): the Play menu, from any page
      in.Consume(Action::kPlay);
      if (CanPlay()) OpenPlayMenu();
    }
    if (in.Pressed(Action::kResetPage) && page_ != kHome && sub_ == kNoSubPage && !PageSettings(page_).empty()) {
      in.Consume(Action::kResetPage);
      OpenResetPage();
    }
    if (in.Pressed(Action::kBack)) {
      in.Consume(Action::kBack);
      ui::Cue(ui::Sound::kBack);
      if (sub_ != kNoSubPage) sub_ = kNoSubPage;
      else if (page_ == kGraphics && extras_return_focus_ >= 0) {  // opened from Extras: back there
        const int focus = extras_return_focus_;
        OpenPlayMenu();
        OpenPlayView(PlayView::kExtras);
        play_lists_[int(PlayView::kExtras)].focus = focus;
        REXLOG_INFO("KK: back from Graphics to Extras, row {}", focus);
      } else if (page_ != kHome) GoTo(kHome);
      else if (status_focus_ || side_focus_ >= 0) status_focus_ = false, side_focus_ = -1;  // back to the menu
      else OpenQuit();
    }
  }

  // ---------------------------------------------------------------- Home ---
  struct HomeEntry {
    std::string label;
    bool enabled = true;
    std::function<void()> action;
    std::string value;                // shown after the label (left/right change it)
    std::function<void(int)> change;  // -1 / +1
    bool starts_game = false;         // starts the game at once (no select sound: it would be cut off)
  };

  std::vector<HomeEntry> HomeEntries() {
    std::vector<HomeEntry> e;
    e.push_back({"Play", CanPlay(), [this] { OpenPlayMenu(); }});
    // An update that was found stays one press away; checking, reinstalling and the rest are on About.
    if (update_ && update_->found && !installing_update_)
      e.push_back({F("Update to v{0}", {update_->found->version}), true, [this] { OpenUpdatePrompt(); }});
    e.push_back({"Settings", true, [this] { GoTo(settings_page_); }});
    e.push_back({"Achievements", true, [this] { GoTo(kAchievements); }});
    e.push_back({"About", true, [this] { GoTo(kAbout); }});
    e.push_back({"Quit", true, [this] { Quit(); }});
    return e;
  }

  // A column of large menu entries (Home and the setup screen): the focused
  // one moves in, with a brass mark. Returns the one chosen, or -1. `item_h`:
  // each entry's height when there's less room than usual (0: the usual).
  int DrawMenu(ImDrawList* dl, const std::vector<HomeEntry>& entries, int& focus, float x, float y, float alpha,
               float item_h = 0) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const int n = int(entries.size());
    if (n == 0) return -1;
    focus = std::clamp(focus, 0, n - 1);
    const int focus_before = focus;
    menu_value_ = bool(entries[size_t(focus)].change);
    const bool active = !modal_.open && side_focus_ < 0 && !status_focus_;
    // Home: down from the last entry (or up from the first) moves to the shaders' status below.
    if (active && menu_to_status_ && n > 1 &&
        ((focus == n - 1 && input_.Pressed(Action::kDown)) || (focus == 0 && input_.Pressed(Action::kUp)))) {
      input_.Consume(Action::kDown);
      input_.Consume(Action::kUp);
      status_focus_ = true;
      ui::Cue(ui::Sound::kMove);
    }
    if (active && n > 1) {
      if (input_.Pressed(Action::kUp)) focus = (focus + n - 1) % n;
      if (input_.Pressed(Action::kDown)) focus = (focus + 1) % n;
    }
    if (active && entries[size_t(focus)].change) {
      for (const int dir : {-1, +1}) {
        if (!input_.Pressed(dir < 0 ? Action::kLeft : Action::kRight)) continue;
        ui::Cue(ui::Sound::kChange);
        entries[size_t(focus)].change(dir);
      }
    }
    if (item_h <= 0) item_h = 42 * s;
    const float fs = item_h * (24.0f / 42.0f), tr = 3 * s;
    home_offset_.resize(size_t(std::max<int>(n, int(home_offset_.size()))), 0.0f);
    int chosen = -1;
    for (int i = 0; i < n; ++i) {
      const HomeEntry& e = entries[size_t(i)];
      const std::string label = Upper(T(e.label));
      const float iy = y + item_h * float(i);
      float tw = ui::TrackedWidth(f.display, fs, label, tr);
      const std::string value = Upper(e.value);
      if (!value.empty()) tw += 30 * s + ui::TrackedWidth(f.display, fs, value, tr) + 30 * s;
      const ImVec2 r0(x - 12 * s, iy), r1(x + 40 * s + tw, iy + item_h);
      if (active && ImGui::IsMouseHoveringRect(r0, r1, false)) {
        if (input_.mouse_moved()) focus = i;
        if (ImGui::IsMouseClicked(0)) {
          focus = i;
          chosen = i;
        }
      }
      const bool on = i == focus && side_focus_ < 0 && !status_focus_;
      home_offset_[size_t(i)] = ui::Approach(home_offset_[size_t(i)], on ? 22 * s : 0.0f, 18.0f);
      const float off = home_offset_[size_t(i)];
      const float k = off / (22 * s);
      if (k > 0.02f)
        dl->AddRectFilled(ImVec2(x, iy + item_h * 0.5f - 11 * s), ImVec2(x + 3 * s, iy + item_h * 0.5f + 11 * s),
                          ui::WithAlpha(color::kAccent, k * alpha));
      const ImU32 col = !e.enabled ? color::kTextFaint : on ? color::kText : color::kTextDim;
      const float ty = iy + (item_h - fs) * 0.5f;
      ui::DrawTracked(dl, f.display, fs, ImVec2(x + off, ty), ui::WithAlpha(col, alpha), label, tr);
      if (!value.empty()) {
        // The value between arrows when focused: < ENGLISH >
        const float vx = x + off + ui::TrackedWidth(f.display, fs, label, tr) + 30 * s;
        const float cy = iy + item_h * 0.5f, aw = 6 * s, ah = 10 * s;
        const ImU32 vc = ui::WithAlpha(on ? color::kAccent : color::kTextDim, alpha);
        const float vw = ui::TrackedWidth(f.display, fs, value, tr);
        if (on) {
          dl->AddTriangleFilled(ImVec2(vx - 16 * s, cy), ImVec2(vx - 16 * s + aw, cy - ah * 0.5f),
                                ImVec2(vx - 16 * s + aw, cy + ah * 0.5f), vc);
          dl->AddTriangleFilled(ImVec2(vx + vw + 10 * s, cy - ah * 0.5f), ImVec2(vx + vw + 10 * s + aw, cy),
                                ImVec2(vx + vw + 10 * s, cy + ah * 0.5f), vc);
        }
        ui::DrawTracked(dl, f.display, fs, ImVec2(vx, ty), vc, value, tr);
      }
    }
    if (active && input_.Pressed(Action::kAccept)) chosen = focus;
    if (focus != focus_before) ui::Cue(ui::Sound::kMove);
    // An entry with a value: selecting it moves to the next value.
    if (chosen >= 0 && entries[size_t(chosen)].change) {
      ui::Cue(ui::Sound::kChange);
      entries[size_t(chosen)].change(+1);
      chosen = -1;
    }
    if (chosen >= 0 && !entries[size_t(chosen)].enabled) {
      ui::Cue(ui::Sound::kDeny);
      chosen = -1;
    }
    // Play starts the game at once, and its sound would be cut off.
    if (chosen >= 0 && !entries[size_t(chosen)].starts_game) ui::Cue(ui::Sound::kSelect);
    return chosen;
  }

  // ------------------------------------------------------- side panels ---
  // Panels on the bottom status row of Home and the setup screen, as tall as
  // it and right-aligned with the button prompts: Add to Steam (while it can)
  // then Discord. Right from the menu (or the mouse) moves to them, Left and
  // Right between them; A or a click uses one; Left from the first, Up, Down
  // or B go back to the menu.
  struct SidePanel {
    rex::ui::ImmediateTexture* icon;
    ImU32 tile;  // the brand's colour, behind its white mark
    std::string title, text;
    const char* verb;  // the footer's A prompt while it has the focus
    Action key;        // its own button (X or Y), on the screens with the panels
    bool enabled;
    std::function<void()> action;
  };

  rex::ui::ImmediateTexture* BrandIcon(std::unique_ptr<rex::ui::ImmediateTexture>& slot, const unsigned char* mask,
                                       int size) {
    if (!slot && immediate_) {
      const size_t n = size_t(size) * size;
      std::vector<uint8_t> rgba(n * 4, 255);
      for (size_t i = 0; i < n; ++i) rgba[i * 4 + 3] = mask[i];
      slot = immediate_->CreateTexture(uint32_t(size), uint32_t(size), rex::ui::ImmediateTextureFilter::kLinear, false,
                                       rgba.data());
    }
    return slot.get();
  }

  std::vector<SidePanel> SidePanels(bool home) {
    std::vector<SidePanel> panels;
    if (home && steam_ && steam_->available && steam_->in_library) {
      // Already added (this copy's program is in a Steam library): opens Steam instead.
      panels.push_back({BrandIcon(steam_icon_, kSteamIcon, kSteamIconSize), IM_COL32(27, 40, 56, 255),
                        T("In your Steam library"), T("Start King Kong from Steam, Big Picture or a Steam Deck."),
                        "Open Steam", Action::kDefault, true, [] { steam::OpenSteam(); }});
    } else if (home && steam_ && steam_->available) {
      const bool busy = steam_->busy;
      const int phase = std::clamp(steam_->phase.load(), 0, int(std::size(kSteamPhases)) - 1);
      panels.push_back({BrandIcon(steam_icon_, kSteamIcon, kSteamIconSize), IM_COL32(27, 40, 56, 255),
                        T("Add King Kong to Steam"),
                        busy ? T(kSteamPhases[phase]) : T("Play it from your Steam library, with its artwork."),
                        "Add to Steam", Action::kDefault, !busy, [this] { OpenSteamPrompt(); }});
    }
    panels.push_back({BrandIcon(discord_icon_, kDiscordIcon, kDiscordIconSize), IM_COL32(88, 101, 242, 255),
                      T("Join King Kong Recompiled's Discord"), T("Chat with players, share feedback, shape the project."),
                      "Open in browser", Action::kResetPage, true, [] { OpenUrl(kDiscordUrl); }});
    return panels;
  }

  // The footer's A prompt while a side panel has the focus (matches SidePanels).
  const char* SidePanelVerb(bool home) const {
    const bool steam = home && steam_ && steam_->available;
    if (!steam || side_focus_ != 0) return "Open in browser";
    return steam_->in_library ? "Open Steam" : "Add to Steam";
  }

  // `row_top`: the top of the bottom status row (Game, Shader pack, Version).
  void DrawSidePanels(ImDrawList* dl, float alpha, bool home, const std::vector<HomeEntry>& entries, int focus,
                      float row_top) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const std::vector<SidePanel> panels = SidePanels(home);
    const int n = int(panels.size());
    if (side_focus_ >= n) side_focus_ = n - 1;

    // They arrive once per launch, a moment after the screen settles, from the
    // right: each fades in as it rises a few pixels into place. No sound.
    const double now = ImGui::GetTime();
    if (side_since_ < 0) side_since_ = now;
    auto arrival = [&](int i) {
      const float t = std::clamp(float((now - side_since_ - 0.6 - 0.15 * (n - 1 - i)) / 0.45), 0.0f, 1.0f);
      return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);  // ease out
    };
    if (arrival(n - 1) <= 0.0f) {
      side_focus_ = -1;
      return;
    }
    auto use = [&](int i) {
      const SidePanel& p = panels[size_t(i)];
      ui::Cue(p.enabled ? ui::Sound::kSelect : ui::Sound::kDeny);
      if (p.enabled) p.action();
    };

    // Controller and keyboard. Right is free unless the menu's entry uses it
    // for a value (Language).
    if (!modal_.open) {
      // Each panel's own button (X: Steam, Y: Discord) works from anywhere on this screen.
      for (int i = 0; i < n; ++i) {
        if (arrival(i) < 1.0f || !input_.Pressed(panels[size_t(i)].key)) continue;
        input_.Consume(panels[size_t(i)].key);
        use(i);
      }
      const bool right_free =
          entries.empty() || !entries[size_t(std::clamp(focus, 0, int(entries.size()) - 1))].change;
      if (side_focus_ < 0 && right_free && input_.Pressed(Action::kRight)) {
        input_.Consume(Action::kRight);
        side_focus_ = 0;
        while (side_focus_ < n - 1 && arrival(side_focus_) < 1.0f) ++side_focus_;  // the ones shown yet
        ui::Cue(ui::Sound::kMove);
      } else if (side_focus_ >= 0) {
        for (const Action a : {Action::kLeft, Action::kRight, Action::kUp, Action::kDown, Action::kBack}) {
          if (side_focus_ < 0 || !input_.Pressed(a)) continue;
          input_.Consume(a);
          const int next = a == Action::kLeft ? side_focus_ - 1 : a == Action::kRight ? side_focus_ + 1 : -1;
          if (next >= n) continue;  // already the last
          side_focus_ = next;
          ui::Cue(a == Action::kBack ? ui::Sound::kBack : ui::Sound::kMove);
        }
        if (side_focus_ >= 0 && input_.Pressed(Action::kAccept)) {
          input_.Consume(Action::kAccept);
          use(side_focus_);
        }
      }
    }

    // As tall as the status row (its caption, value and progress bar).
    const float h = 48 * s, ft = 13 * s, fs = 11 * s, tile = 34 * s;
    const float pad_l = 7 * s, pad_r = 11 * s, gap = 10 * s, between = 10 * s, r = 10 * s;
    const float key_h = 21 * s;
    const std::string prompt_set = PromptSet();
    auto key_glyph = [&](Action a, float* w) {
      float aspect = 1.0f;
      rex::ui::ImmediateTexture* t = glyphs_.Get(a, prompt_set, &aspect);
      *w = t ? key_h * aspect : 0.0f;
      return t;
    };
    std::vector<float> widths;
    for (const auto& p : panels) {
      const float text_w = std::max(ui::TextSize(f.semibold, ft, p.title).x, ui::TextSize(f.text, fs, p.text).x);
      float key_w = 0;
      key_glyph(p.key, &key_w);
      widths.push_back(pad_l + tile + gap + text_w + gap + key_w + pad_r);
    }
    const ImVec2 row(o_.x + w_ - Margin(), row_top - 1 * s);  // right end, top

    // Right to left from the button prompts' right edge.
    std::vector<float> lefts(static_cast<size_t>(n));
    float right = row.x;
    for (int i = n - 1; i >= 0; --i) {
      lefts[size_t(i)] = right - widths[size_t(i)];
      right = lefts[size_t(i)] - between;
    }

    // The mouse.
    int hovered = -1;
    for (int i = 0; i < n && !modal_.open; ++i)
      if (arrival(i) >= 1.0f && ImGui::IsMouseHoveringRect(ImVec2(lefts[size_t(i)], row.y),
                                                           ImVec2(lefts[size_t(i)] + widths[size_t(i)], row.y + h), false))
        hovered = i;
    if (input_.mouse_moved()) side_focus_ = hovered;
    if (hovered >= 0 && ImGui::IsMouseClicked(0)) use(hovered);

    for (int i = 0; i < n; ++i) {
      const SidePanel& p = panels[size_t(i)];
      const float arrive = arrival(i);
      const float a = alpha * arrive;
      if (a <= 0.0f) continue;
      const float w = widths[size_t(i)];
      const ImVec2 p0(lefts[size_t(i)], row.y + (1.0f - arrive) * 10 * s), p1(p0.x + w, p0.y + h);
      const bool focused = side_focus_ == i, on = focused || hovered == i;
      dl->AddRectFilled(ImVec2(p0.x, p0.y + 5 * s), ImVec2(p1.x, p1.y + 5 * s),
                        ui::WithAlpha(IM_COL32(0, 0, 0, 255), 0.18f * a), r);
      dl->AddRectFilled(p0, p1, ui::WithAlpha(on ? IM_COL32(30, 33, 40, 255) : IM_COL32(16, 18, 22, 255), (on ? 0.78f : 0.55f) * a), r);
      dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p0.y + h * 0.45f),
                        ui::WithAlpha(IM_COL32(255, 255, 255, 255), (on ? 0.05f : 0.025f) * a), r - 1,
                        ImDrawFlags_RoundCornersTop);
      dl->AddRect(p0, p1, ui::WithAlpha(color::kText, (on ? 0.30f : 0.08f) * a), r, 0, 1.0f * s);
      if (focused)  // the focus ring, as on the other focused things
        dl->AddRect(ImVec2(p0.x - 3 * s, p0.y - 3 * s), ImVec2(p1.x + 3 * s, p1.y + 3 * s),
                    ui::WithAlpha(color::kFocus, 0.85f * a), r + 3 * s, 0, 2.0f * s);

      const float dim = p.enabled ? 1.0f : 0.55f;
      const ImVec2 t0(p0.x + pad_l, p0.y + (h - tile) * 0.5f);
      dl->AddRectFilled(t0, ImVec2(t0.x + tile, t0.y + tile), ui::WithAlpha(p.tile, a * dim), 8 * s);
      if (p.icon) {
        const float g = 19 * s, gx = t0.x + (tile - g) * 0.5f, gy = t0.y + (tile - g) * 0.5f;
        dl->AddImage(ui::Tex(p.icon), ImVec2(gx, gy), ImVec2(gx + g, gy + g), ImVec2(0, 0), ImVec2(1, 1),
                     ui::WithAlpha(IM_COL32_WHITE, a * dim));
      }
      const float tx = t0.x + tile + gap;
      const float title_h = ft * 1.25f, text_h = fs * 1.25f;
      const float ty = p0.y + (h - (title_h + 1 * s + text_h)) * 0.5f;
      dl->AddText(f.semibold, ft, ImVec2(tx, ty), ui::WithAlpha(color::kText, a * dim), p.title.c_str());
      dl->AddText(f.text, fs, ImVec2(tx, ty + title_h + 1 * s), ui::WithAlpha(on ? color::kText : color::kTextDim, a * dim),
                  p.text.c_str());
      // Its button, in the picked prompt style.
      float key_w = 0;
      if (auto* key = key_glyph(p.key, &key_w)) {
        const ImVec2 k0(p1.x - pad_r - key_w, p0.y + (h - key_h) * 0.5f);
        dl->AddImage(ui::Tex(key), k0, ImVec2(k0.x + key_w, k0.y + key_h), ImVec2(0, 0), ImVec2(1, 1),
                     ui::WithAlpha(IM_COL32_WHITE, a * (on ? 1.0f : 0.8f) * dim));
      }
    }
  }

  // ------------------------------------------------------------------- Play ---
  // Play on Home opens the Play menu (docs/menus.md): Continue (the save played last), New game,
  // Load game and Chapter select. Every choice tells the game what to start with
  // (play::SetStartRequest, play_session.h) and starts it as Play always did; the game then shows
  // none of its own menus.
  static constexpr size_t kNameMax = 15;  // a save's name (the game's own are short: "A")

  void OpenPlayMenu() {
    if (!CanPlay()) {
      ui::Cue(ui::Sound::kDeny);
      return;
    }
    GoTo(kHome);
    status_focus_ = false;
    ReadSaves();
    play_ = PlayView::kMenu;
    play_focus_ = 0;
  }

  void OpenPlayView(PlayView view) {
    ReadSaves();
    play_ = view;
    ui::ListState& st = play_lists_[int(view)];
    st = {};
    // New game: the focus on the first slot (the empty ones are listed first).
    if (view == PlayView::kNew) st.focus = 3;
  }

  void ReadSaves() {
    saves_ = play::ListSaves(paths_.user_dir);
    saves_.resize(play::kSlotCount);
    for (int i = 0; i < play::kSlotCount; ++i) saves_[size_t(i)].index = i;
    if (!SlotUsed(chapter_slot_)) chapter_slot_ = -1;
  }
  bool SlotUsed(int i) const { return i >= 0 && i < int(saves_.size()) && saves_[size_t(i)].exists; }
  int UsedSlots() const {
    int n = 0;
    for (const auto& sv : saves_) n += sv.exists ? 1 : 0;
    return n;
  }
  int FirstEmptySlot() const {
    for (int i = 0; i < play::kSlotCount; ++i)
      if (!SlotUsed(i)) return i;
    return -1;
  }
  // Chapter select's save: the one picked on Load game, else the one played last (-1: none yet).
  int ChapterSlot() const { return chapter_slot_ >= 0 ? chapter_slot_ : play::MostRecentSave(saves_); }

  void PlayWith(const play::StartRequest& request) {
    if (!CanPlay()) {
      ui::Cue(ui::Sound::kDeny);
      return;
    }
    play::SetStartRequest(request);
    REXLOG_INFO("KK: Play menu: start kind {} slot {} chapter {}", int(request.kind), request.slot, request.chapter);
    Play();
  }

  void StartChapter(int entry) {
    int slot = ChapterSlot();
    if (slot < 0) slot = std::max(0, FirstEmptySlot());  // no save yet: the game makes one here first
    PlayWith({play::StartKind::kChapter, slot, entry, ""});
  }

  static std::string ChapterName(int entry) {
    for (const play::ChapterInfo& c : play::Chapters())
      if (c.entry == entry && !c.name.empty()) return c.name;
    return entry >= 0 ? F("Chapter {0}", {std::to_string(entry + 1)}) : std::string();
  }
  static std::string SaveName(const play::SaveSlot& sv) {
    return sv.name.empty() ? F("Slot {0}", {std::to_string(sv.index + 1)}) : sv.name;
  }
  static int ContinueChapter(const play::SaveSlot& sv) {
    return sv.continue_chapter >= 0 ? sv.continue_chapter : sv.chapter_reached;
  }
  static int64_t TotalScore(const play::SaveSlot& sv) {
    int64_t total = 0;
    for (const int v : sv.best_scores) total += std::max(0, v);
    return total;
  }
  static int BestScore(const play::SaveSlot* sv, int entry) {
    return sv && entry >= 0 && entry < int(sv->best_scores.size()) ? std::max(0, sv->best_scores[size_t(entry)]) : 0;
  }
  // "13 Oct 2026", or "Today, 14:02" and "Yesterday, 09:10" when `recent`.
  static std::string When(int64_t unix_time, bool recent = true) {
    if (unix_time <= 0) return T("Unknown");
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    auto local = [](int64_t t) {
      std::tm tm{};
      const std::time_t tt = std::time_t(t);
#if defined(_WIN32)
      localtime_s(&tm, &tt);
#else
      localtime_r(&tt, &tm);
#endif
      return tm;
    };
    const std::tm tm = local(unix_time);
    const std::tm now = local(int64_t(std::time(nullptr)));
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
    const int days_ago = (now.tm_year - tm.tm_year) * 366 + (now.tm_yday - tm.tm_yday);
    if (recent && days_ago == 0) return F("Today, {0}", {clock});
    if (recent && days_ago == 1) return F("Yesterday, {0}", {clock});
    return std::to_string(tm.tm_mday) + " " + T(kMonths[std::clamp(tm.tm_mon, 0, 11)]) + " " +
           std::to_string(tm.tm_year + 1900);
  }

  // The Play menu: Home's look (the clear stills, the logo, the large entries), with what the
  // focused entry does in the status row.
  std::vector<HomeEntry> PlayEntries() {
    std::vector<HomeEntry> e;
    const int recent = play::MostRecentSave(saves_);
    if (recent >= 0)
      e.push_back({"Continue", CanPlay(),
                   [this, recent] { PlayWith({play::StartKind::kContinue, recent, -1, ""}); }, "", nullptr, true});
    e.push_back({"New game", CanPlay(), [this] { OpenPlayView(PlayView::kNew); }});
    e.push_back({"Load game", UsedSlots() > 0, [this] { OpenPlayView(PlayView::kLoad); }});
    e.push_back({"Chapter select", CanPlay(), [this] { OpenPlayView(PlayView::kChapters); }});
    e.push_back({"Extras", CanPlay(), [this] { OpenPlayView(PlayView::kExtras); }});
    e.push_back({"Back", true, [this] { play_ = PlayView::kNone; }});
    return e;
  }

  void DrawPlayMenu(ImDrawList* dl) {
    const float s = S();
    const float x = o_.x + Margin(), a = home_mix_;
    std::vector<HomeEntry> entries = PlayEntries();
    const float y = DrawLogo(dl, x, entries.size(), a);
    const float room = o_.y + h_ - 150 * s - y;
    const float item_h = std::clamp(room / float(std::max<size_t>(entries.size(), 1)), 28 * s, 42 * s);
    if (const int i = DrawMenu(dl, entries, play_focus_, x, y, a, item_h); i >= 0) {
      entries[size_t(i)].action();
      if (play_ != PlayView::kMenu) return;
    }
    // What the focused entry does, in the status row's columns.
    std::vector<std::pair<std::string, std::string>> cols;
    const std::string label = entries[size_t(std::clamp(play_focus_, 0, int(entries.size()) - 1))].label;
    const int recent = play::MostRecentSave(saves_);
    if (label == "Continue" && recent >= 0) {
      const play::SaveSlot& sv = saves_[size_t(recent)];
      cols = {{"Save", SaveName(sv)}, {"Chapter", T(ChapterName(ContinueChapter(sv)))}, {"Last saved", When(sv.last_saved)}};
    } else if (label == "New game") {
      cols = {{"New game", T("The story from the beginning.")},
              {"Free slots", F("{0} of {1}", {std::to_string(play::kSlotCount - UsedSlots()),
                                               std::to_string(play::kSlotCount)})}};
    } else if (label == "Load game") {
      cols = {{"Saves", UsedSlots() ? F("{0} of {1} slots used", {std::to_string(UsedSlots()),
                                                                  std::to_string(play::kSlotCount)})
                                    : T("No saves yet.")}};
    } else if (label == "Chapter select") {
      const int slot = ChapterSlot();
      cols = {{"Chapters", F("All {0} open", {std::to_string(play::kChapterCount)})},
              {"Save", slot >= 0 ? SaveName(saves_[size_t(slot)]) : T("A new one is made")}};
    } else if (label == "Extras") {
      const int slot = ChapterSlot();
      cols = {{"Extras", T("Bonuses use this save's progress and scores.")},
              {"Save", slot >= 0 ? SaveName(saves_[size_t(slot)]) : T("No save yet")}};
    }
    DrawInfoColumns(dl, x, o_.y + h_ - 120 * s, a, cols);
  }

  // Captions and values in columns, as Home's status row.
  void DrawInfoColumns(ImDrawList* dl, float x, float y, float alpha,
                       const std::vector<std::pair<std::string, std::string>>& cols) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    float cx = x;
    for (const auto& [caption, value] : cols) {
      const float col_w = std::max(210 * s, ui::TextSize(f.text, 15.5f * s, value).x + 48 * s);
      ui::DrawTracked(dl, f.display, 12 * s, ImVec2(cx, y), ui::WithAlpha(color::kTextFaint, alpha), Upper(T(caption)),
                      2.2f * s);
      dl->AddText(f.text, 15.5f * s, ImVec2(cx, y + 20 * s), ui::WithAlpha(color::kText, alpha), value.c_str());
      cx += col_w;
    }
  }

  static const char* PlayViewName(PlayView view) {
    switch (view) {
      case PlayView::kLoad: return "Load game";
      case PlayView::kNew: return "New game";
      case PlayView::kChapters: return "Chapter select";
      case PlayView::kExtras: return "Extras";
      default: return "";
    }
  }

  // Instead of the tabs: PLAY, then the page's name, with the brass underline under the last (a
  // click on PLAY goes back to the menu); on Chapter select the save it starts with at the right.
  void DrawPlayHeader(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float fs = 15 * s, tr = 1.6f * s, cy = o_.y + 44 * s;
    float x = o_.x + Margin();
    const std::string play = Upper(T("Play"));
    const float pw = ui::TrackedWidth(f.display, fs, play, tr);
    const bool sub = play_ != PlayView::kMenu;
    const bool hot = sub && !modal_.open &&
                     ImGui::IsMouseHoveringRect(ImVec2(x - 10 * s, cy - 22 * s), ImVec2(x + pw + 10 * s, cy + 22 * s), false);
    if (hot && ImGui::IsMouseClicked(0)) {
      ui::Cue(ui::Sound::kBack);
      play_ = PlayView::kMenu;
    }
    ui::DrawTracked(dl, f.display, fs, ImVec2(x, cy - fs * 0.5f),
                    sub ? (hot ? color::kText : color::kTextDim) : color::kText, play, tr);
    float ux = x, uw = pw;
    if (sub) {
      x += pw + 14 * s;
      dl->AddRectFilled(ImVec2(x, cy - 1 * s), ImVec2(x + 8 * s, cy + 1 * s), color::kTextFaint);
      x += 8 * s + 14 * s;
      const std::string name = Upper(T(PlayViewName(play_)));
      ui::DrawTracked(dl, f.display, fs, ImVec2(x, cy - fs * 0.5f), color::kText, name, tr);
      ux = x;
      uw = ui::TrackedWidth(f.display, fs, name, tr);
    }
    if (tab_x_ < 0) tab_x_ = ux, tab_w_ = uw;
    tab_x_ = ui::Approach(tab_x_, ux, 22.0f);
    tab_w_ = ui::Approach(tab_w_, uw, 22.0f);
    const float ly = std::round(o_.y + 74 * s);
    dl->AddLine(ImVec2(o_.x + Margin(), ly), ImVec2(o_.x + w_ - Margin(), ly), color::kHairline, 1.0f);
    dl->AddRectFilled(ImVec2(tab_x_, ly - 2 * s), ImVec2(tab_x_ + tab_w_, ly), color::kAccent);
    if (play_ == PlayView::kChapters) {
      const int slot = ChapterSlot();
      const std::string right = Upper(slot >= 0 ? F("Save: {0}", {SaveName(saves_[size_t(slot)])}) : T("No save yet"));
      const float rs = 13 * s, rt = 2.2f * s, rw = ui::TrackedWidth(f.display, rs, right, rt);
      ui::DrawTracked(dl, f.display, rs, ImVec2(o_.x + w_ - Margin() - rw, cy - rs * 0.5f), color::kTextDim, right, rt);
    }
  }

  // A slot's row (Load game, New game): its name, then the slot, the chapter and when it was saved.
  Item SlotItem(const play::SaveSlot& sv) {
    Item it;
    it.tall = true;
    it.type = ItemType::kAction;
    const std::string slot = F("Slot {0}", {std::to_string(sv.index + 1)});
    if (!sv.exists) {
      it.label = slot;
      it.subtitle = "Empty";
      return it;
    }
    it.label = SaveName(sv);
    it.subtitle = slot + "  " KK_DOT "  " + T(ChapterName(ContinueChapter(sv))) + "  " KK_DOT "  " + When(sv.last_saved);
    it.facts.push_back(F("Chapter reached: {0}", {T(ChapterName(sv.chapter_reached))}));
    it.facts.push_back(F("Continues at: {0}", {T(ChapterName(ContinueChapter(sv)))}));
    it.facts.push_back(F("Last saved: {0}", {When(sv.last_saved)}));
    if (sv.created > 0) it.facts.push_back(F("Started: {0}", {When(sv.created, false)}));
    it.facts.push_back(F("Total of best scores: {0}", {std::to_string(TotalScore(sv))}));
    return it;
  }

  std::vector<Item> LoadItems() {
    std::vector<Item> items;
    row_data_.clear();
    items.push_back(Section("Saves"));
    row_data_.push_back(-1);
    for (const play::SaveSlot& sv : saves_) {
      Item it = SlotItem(sv);
      if (sv.exists) {
        it.description = "Continue this save where you left it.";
        it.value_text = std::to_string(TotalScore(sv));
        const int slot = sv.index;
        it.on_activate = [this, slot] { PlayWith({play::StartKind::kContinue, slot, -1, ""}); };
      } else {
        it.type = ItemType::kInfo;
        it.disabled = true;
        it.description = "No save in this slot. New game can start one here.";
      }
      items.push_back(std::move(it));
      row_data_.push_back(sv.exists ? sv.index : -1);
    }
    return items;
  }

  std::vector<Item> NewGameItems() {
    std::vector<Item> items;
    row_data_.clear();
    items.push_back(Section("Name"));
    row_data_.push_back(-1);
    {
      Item it = ActionItem("Name",
                           "The new save's name, typed with the keyboard. Left empty, the game names it as it "
                           "always does.",
                           new_name_.empty() ? "The game's" : new_name_, [this] { OpenNameEditor(); });
      it.value_color = color::kText;
      items.push_back(std::move(it));
      row_data_.push_back(-1);
    }
    items.push_back(Section("Slot"));
    row_data_.push_back(-1);
    for (const bool used : {false, true}) {  // the empty slots first
      for (const play::SaveSlot& sv : saves_) {
        if (sv.exists != used) continue;
        Item it = SlotItem(sv);
        const int slot = sv.index;
        if (!used) {
          it.description = "Starts the story from the beginning in this slot.";
          it.on_activate = [this, slot] { PlayWith({play::StartKind::kNewGame, slot, -1, new_name_}); };
        } else {
          it.description = "Starts a new game in place of this save. Your saves are backed up first.";
          it.value_text = "Overwrite";
          it.value_color = color::kWarn;
          it.on_activate = [this, slot] { ConfirmOverwrite(slot); };
        }
        items.push_back(std::move(it));
        row_data_.push_back(slot);
      }
    }
    return items;
  }

  // Chapter select: every entry open, in groups, with its best score in the save it starts with.
  std::vector<Item> ChapterItems() {
    std::vector<Item> items;
    row_data_.clear();
    const int slot = ChapterSlot();
    const play::SaveSlot* sv = SlotUsed(slot) ? &saves_[size_t(slot)] : nullptr;
    const auto& chapters = play::Chapters();
    constexpr size_t kGroup = 7;
    for (size_t i = 0; i < chapters.size(); ++i) {
      const int entry = chapters[i].entry;
      if (i % kGroup == 0) {
        const size_t last = std::min(chapters.size(), i + kGroup);
        items.push_back(Section(F("Chapters {0} to {1}", {std::to_string(i + 1), std::to_string(last)})));
        row_data_.push_back(-1);
      }
      Item it;
      it.type = ItemType::kAction;
      it.label = ChapterName(entry);
      const int best = BestScore(sv, entry);
      if (best > 0) it.value_text = std::to_string(best);
      it.on_activate = [this, entry] { StartChapter(entry); };
      items.push_back(std::move(it));
      row_data_.push_back(entry);
    }
    return items;
  }

  // Load game, New game and Chapter select: the settings pages' layout (list and description).
  void DrawPlayPage(ImDrawList* dl) {
    const float s = S();
    std::vector<Item> items = play_ == PlayView::kLoad     ? LoadItems()
                              : play_ == PlayView::kNew    ? NewGameItems()
                              : play_ == PlayView::kExtras ? ExtrasItems()
                                                           : ChapterItems();
    if (play_ == PlayView::kExtras) row_data_.clear();  // its rows are the bonuses, not slots or chapters
    ui::ListState& st = play_lists_[int(play_)];
    // The page's own buttons, on the focused row (last frame's): on Load game X deletes and Y opens
    // Chapter select with that save; on Chapter select Y changes the save.
    const int row = st.focus >= 0 && st.focus < int(row_data_.size()) ? row_data_[size_t(st.focus)] : -1;
    if (!modal_.open && play_ == PlayView::kLoad && SlotUsed(row)) {
      if (input_.Pressed(Action::kDefault)) {
        input_.Consume(Action::kDefault);
        ui::Cue(ui::Sound::kSelect);
        ConfirmDelete(row);
      } else if (input_.Pressed(Action::kResetPage)) {
        input_.Consume(Action::kResetPage);
        ui::Cue(ui::Sound::kSelect);
        chapter_slot_ = row;
        OpenPlayView(PlayView::kChapters);
        return;
      }
    }
    if (!modal_.open && play_ == PlayView::kChapters && input_.Pressed(Action::kResetPage)) {
      input_.Consume(Action::kResetPage);
      if (UsedSlots() > 1) {
        ui::Cue(ui::Sound::kChange);
        const int from = std::max(0, ChapterSlot());
        for (int k = 1; k <= play::kSlotCount; ++k) {
          const int next = (from + k) % play::kSlotCount;
          if (SlotUsed(next)) {
            chapter_slot_ = next;
            break;
          }
        }
      } else {
        ui::Cue(ui::Sound::kDeny);
      }
    }
    const float top = o_.y + 104 * s, bottom = o_.y + h_ - 96 * s;
    const float x0 = o_.x + Margin(), avail = w_ - Margin() * 2, list_w = std::round(avail * 0.56f);
    const Item* focused = ui::DrawList(dl, items, st, ImVec2(x0, top), ImVec2(x0 + list_w, bottom), m_, input_,
                                       !modal_.open);
    const ImVec2 d0(x0 + list_w + 76 * s, top + 18 * s), d1(o_.x + w_ - Margin(), bottom);
    if (play_ == PlayView::kChapters) {
      const int entry = st.focus >= 0 && st.focus < int(row_data_.size()) ? row_data_[size_t(st.focus)] : -1;
      DrawChapterPanel(dl, focused ? entry : -1, d0, d1);
    } else {
      ui::DrawDescription(dl, focused, d0, d1, m_);
    }
    focused_items_ = std::move(items);
    focused_ = st.focus >= 0 && st.focus < int(focused_items_.size()) ? &focused_items_[size_t(st.focus)] : nullptr;
  }

  rex::ui::ImmediateTexture* ChapterPicture(const std::filesystem::path& path, float* aspect) {
    if (path.empty()) return nullptr;
    auto it = chapter_pictures_.find(path);
    if (it == chapter_pictures_.end()) {
      const art::Image img = art::LoadImage(path);
      it = chapter_pictures_.emplace(path, MakeTexture(img)).first;
      if (img) chapter_aspects_[path] = float(img.width) / float(img.height);
    }
    if (it->second && aspect) *aspect = chapter_aspects_[path];
    return it->second;
  }

  // The chapter's card (the game's picture of it when the game files have one, its number over it), its
  // name, its best score and the save it starts with.
  void DrawChapterPanel(ImDrawList* dl, int entry, ImVec2 p0, ImVec2 p1) {
    if (entry < 0) return;
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float w = p1.x - p0.x;
    std::filesystem::path picture_path;
    for (const play::ChapterInfo& c : play::Chapters())
      if (c.entry == entry) picture_path = c.picture;
    float aspect = 16.0f / 9.0f;
    rex::ui::ImmediateTexture* picture = ChapterPicture(picture_path, &aspect);
    const float max_h = (p1.y - p0.y) * 0.46f;
    float pw = std::min(w, max_h * aspect), ph = pw / aspect;
    if (!picture) {
      ph = std::min(w * 9.0f / 16.0f, max_h);
      pw = ph * 16.0f / 9.0f;
    }
    const ImVec2 c0 = p0, c1(p0.x + pw, p0.y + ph);
    dl->AddRectFilled(c0, c1, ui::WithAlpha(color::kPanel, 0.82f));
    if (picture) {
      dl->AddImage(ui::Tex(picture), c0, c1);
      // A shade at the bottom for the number.
      dl->AddRectFilledMultiColor(ImVec2(c0.x, c0.y + ph * 0.45f), c1, ui::WithAlpha(color::kBase, 0.0f),
                                  ui::WithAlpha(color::kBase, 0.0f), ui::WithAlpha(color::kBase, 0.75f),
                                  ui::WithAlpha(color::kBase, 0.75f));
    } else {
      dl->AddRectFilledMultiColor(c0, c1, ui::WithAlpha(color::kAccent, 0.10f), ui::WithAlpha(color::kAccent, 0.02f),
                                  ui::WithAlpha(color::kBase, 0.0f), ui::WithAlpha(color::kAccent, 0.05f));
    }
    dl->AddRect(c0, c1, ui::WithAlpha(color::kText, 0.10f));
    dl->AddRectFilled(c0, ImVec2(c0.x + 3 * s, c1.y), color::kAccent);
    char num[8];
    std::snprintf(num, sizeof(num), "%02d", entry + 1);
    const float ns = picture ? std::min(ph * 0.30f, 56 * s) : ph * 0.42f, nw = ui::TrackedWidth(f.display, ns, num, 2 * s);
    const float nb = picture ? 12 * s : 18 * s;
    ui::DrawTracked(dl, f.display, ns, ImVec2(c0.x + 26 * s, c1.y - ns - nb), ui::WithAlpha(color::kText, 0.9f), num,
                    2 * s);
    const std::string of = Upper(F("of {0}", {std::to_string(play::kChapterCount)}));
    ui::DrawTracked(dl, f.display, 13 * s, ImVec2(c0.x + 38 * s + nw, c1.y - 13 * s - nb - 8 * s),
                    picture ? ui::WithAlpha(color::kText, 0.75f) : color::kTextDim, of, 2.2f * s);

    float y = c1.y + 22 * s;
    const std::string name = T(ChapterName(entry));
    dl->AddText(f.display, 25 * s, ImVec2(p0.x, y), color::kText, name.c_str(), nullptr, w);
    y += ui::TextSize(f.display, 25 * s, name, w).y + 12 * s;
    const int slot = ChapterSlot();
    const play::SaveSlot* sv = SlotUsed(slot) ? &saves_[size_t(slot)] : nullptr;
    const int best = BestScore(sv, entry);
    const float bs = 15.5f * s;
    const std::string score = best > 0 ? F("Best score: {0}", {std::to_string(best)}) : T("No score yet.");
    dl->AddText(f.text, bs, ImVec2(p0.x, y), best > 0 ? color::kText : color::kTextDim, score.c_str(), nullptr, w);
    y += ui::TextSize(f.text, bs, score, w).y + 10 * s;
    const std::string with =
        sv ? F("Starts with the save {0}.", {SaveName(*sv)})
           : F("No save yet: a new one is made in slot {0} first.", {std::to_string(std::max(0, FirstEmptySlot()) + 1)});
    dl->AddText(f.text, bs, ImVec2(p0.x, y), color::kTextDim, with.c_str(), nullptr, w);
  }

  void ConfirmDelete(int slot) {
    const std::string name = SaveName(saves_[size_t(slot)]);
    OpenModal(F("Delete {0}?", {name}),
              "A copy of your saves is kept in the save-backups folder first. Then this save is deleted.",
              {"Delete", "Cancel"}, 1,
              [this, slot, name](int i) {
                if (i != 0) return;
                if (play::DeleteSave(paths_.user_dir, slot))
                  Status(F("{0} is deleted. A copy is in save-backups.", {name}));
                else
                  Status("Could not delete the save. Nothing was changed.");
                ReadSaves();
                if (UsedSlots() == 0) play_ = PlayView::kMenu;
              },
              1);
  }

  void ConfirmOverwrite(int slot) {
    const std::string name = SaveName(saves_[size_t(slot)]);
    OpenModal(F("Overwrite {0}?", {name}),
              "Your saves are backed up first. Then a new game starts in this slot, in place of this save.",
              {"Overwrite", "Cancel"}, 1,
              [this, slot](int i) {
                if (i == 0) PlayWith({play::StartKind::kNewGame, slot, -1, new_name_});
              },
              1);
  }

  // New game's name: typed with the keyboard while its pop-up is open (TickNameEditor).
  void OpenNameEditor() {
    name_before_ = new_name_;
    editing_name_ = true;
    OpenModal("Name", "Type a name for the new save. Enter keeps it, Esc cancels.", {"Done", "The game's"}, 0,
              [this](int i) {
                if (i == 1) new_name_.clear();
                editing_name_ = false;
              });
    modal_.live = [this] {
      const bool blink = std::fmod(ImGui::GetTime(), 1.0) < 0.55;
      return std::string(T("Name:")) + " " + new_name_ + (blink ? "_" : "");
    };
  }

  void TickNameEditor() {
    if (!editing_name_) return;
    if (!modal_.open) {
      editing_name_ = false;
      return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    for (const ImWchar c : io.InputQueueCharacters)
      if (c >= 32 && c < 127 && new_name_.size() < kNameMax && !(c == ' ' && new_name_.empty())) new_name_ += char(c);
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && !new_name_.empty()) new_name_.pop_back();
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (cancel || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      if (cancel) new_name_ = name_before_;
      while (!new_name_.empty() && new_name_.back() == ' ') new_name_.pop_back();
      modal_.open = false;
      editing_name_ = false;
      input_.ConsumeAll();
      ui::Cue(cancel ? ui::Sound::kBack : ui::Sound::kSelect);
    }
  }

  std::vector<ui::Prompt> PlayPrompts() const {
    switch (play_) {
      case PlayView::kLoad: {
        std::vector<ui::Prompt> p;
        if (focused_ && focused_->on_activate) {
          p.push_back({Action::kAccept, "Continue"});
          p.push_back({Action::kResetPage, "Chapter select"});
          p.push_back({Action::kDefault, "Delete"});
        }
        p.push_back({Action::kBack, "Back"});
        return p;
      }
      case PlayView::kChapters: {
        std::vector<ui::Prompt> p = {{Action::kAccept, "Play"}};
        if (UsedSlots() > 1) p.push_back({Action::kResetPage, "Change save"});
        p.push_back({Action::kBack, "Back"});
        return p;
      }
      case PlayView::kExtras: {  // the save row changes with Left and Right; a bonus row plays it
        std::vector<ui::Prompt> p;
        if (focused_ && focused_->type == ItemType::kChoice && !focused_->disabled) p.push_back({Action::kLeft, "Change"});
        if (focused_ && focused_->type == ItemType::kAction && focused_->on_activate) p.push_back({Action::kAccept, "Select"});
        p.push_back({Action::kBack, "Back"});
        return p;
      }
      default:
        return {{Action::kAccept, "Select"}, {Action::kBack, "Back"}};
    }
  }

  void HandlePlayInput() {
    ui::Input& in = input_;
    for (const Action a : {Action::kPrevTab, Action::kNextTab, Action::kPlay}) in.Consume(a);
    if (in.Pressed(Action::kBack)) {
      in.Consume(Action::kBack);
      ui::Cue(ui::Sound::kBack);
      play_ = play_ == PlayView::kMenu ? PlayView::kNone : PlayView::kMenu;
    }
  }

  // Home's logo (or the title in letters), placed so the menu below stays clear of the status row;
  // returns where the menu starts.
  float DrawLogo(ImDrawList* dl, float x, size_t entries, float a) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    // With more entries the title and the menu move up, then the logo gets smaller.
    const float bottom = o_.y + h_ - 150 * s - 42 * s * float(entries);
    float y = o_.y + std::max(100 * s, h_ * 0.14f);
    if (logo_) {
      // The game's own logo.
      float lw = std::min(360 * s, (h_ * 0.34f) * logo_aspect_), lh = lw / logo_aspect_;
      y = std::max(o_.y + 88 * s, std::min(y, bottom - 34 * s - lh));
      lh = std::max(100 * s, std::min(lh, bottom - 34 * s - y));
      lw = lh * logo_aspect_;
      dl->AddImage(ui::Tex(logo_), ImVec2(x - 8 * s, y), ImVec2(x - 8 * s + lw, y + lh), ImVec2(0, 0), ImVec2(1, 1),
                   ui::WithAlpha(IM_COL32_WHITE, a));
      y += lh + 34 * s;
    } else {
      y = std::max(o_.y + 88 * s, std::min(y, bottom - 188 * s));
      ui::DrawTracked(dl, f.display, 17 * s, ImVec2(x + 3 * s, y), ui::WithAlpha(color::kAccent, a), "PETER JACKSON'S",
                      7 * s);
      y += 22 * s;
      ui::DrawTracked(dl, f.display, 100 * s, ImVec2(x - 2 * s, y), ui::WithAlpha(color::kText, a), "KING KONG", 3 * s);
      y += 100 * s;
      ui::DrawTracked(dl, f.display, 14 * s, ImVec2(x + 3 * s, y), ui::WithAlpha(color::kTextDim, a),
                      "THE OFFICIAL GAME OF THE MOVIE", 5.2f * s);
      y += 66 * s;
    }
    return y;
  }

  void DrawHome(ImDrawList* dl) {
    const float s = S();
    const float x = o_.x + Margin();
    const float a = home_mix_;  // fades in with the clear backdrop
    const std::vector<HomeEntry> entries = HomeEntries();
    const float y = DrawLogo(dl, x, entries.size(), a);
    // In a small window (the scale stops shrinking at 0.7) the entries close
    // up rather than run into the status row.
    const float room = o_.y + h_ - 150 * s - y;
    const float item_h = std::clamp(room / float(std::max<size_t>(entries.size(), 1)), 28 * s, 42 * s);
    DrawSidePanels(dl, a, true, entries, home_focus_, o_.y + h_ - 120 * s);
    if (side_focus_ >= 0) status_focus_ = false;
    HomeStatusInput(int(entries.size()));
    menu_to_status_ = ShaderStatusSelectable();
    if (const int i = DrawMenu(dl, entries, home_focus_, x, y, a, item_h); i >= 0) entries[size_t(i)].action();
    menu_to_status_ = false;  // only Home's menu
    DrawHomeStatus(dl, x, o_.y + h_ - 120 * s, a);
  }

  // The SHADERS status on Home (the native renderer's pack) can be selected to build it again.
  bool ShaderStatusSelectable() const { return files_ok_ && !installing_ && NativeShaderPack::Wanted(); }

  // Controller, keyboard and mouse on the shaders' status: Up goes back to the menu's last entry and
  // Down to its first (the menu wraps through it), A, Enter or a click asks to recompile, B goes back
  // (HandleGlobalInput). Its place is last frame's (status_rect_).
  void HomeStatusInput(int entries) {
    if (!ShaderStatusSelectable()) {
      status_focus_ = false;
      return;
    }
    if (modal_.open || side_focus_ >= 0) return;
    const bool hovered = status_rect_[1].x > status_rect_[0].x &&
                         ImGui::IsMouseHoveringRect(status_rect_[0], status_rect_[1], false);
    if (input_.mouse_moved()) status_focus_ = hovered;
    if (hovered && ImGui::IsMouseClicked(0)) {
      status_focus_ = true;
      OpenRecompileShaders();
      return;
    }
    if (!status_focus_) return;
    if (input_.Pressed(Action::kUp) || input_.Pressed(Action::kDown)) {
      home_focus_ = input_.Pressed(Action::kUp) ? std::max(0, entries - 1) : 0;
      input_.Consume(Action::kUp);
      input_.Consume(Action::kDown);
      status_focus_ = false;
      ui::Cue(ui::Sound::kMove);
    } else if (input_.Pressed(Action::kAccept)) {
      input_.Consume(Action::kAccept);
      OpenRecompileShaders();
    }
  }

  // "Recompile shaders": the native renderer's pack built again (force), with the status line's
  // progress bar; Play waits for it as for any build.
  // Home's shaders status: the same as Graphics > Advanced > Rebuild shaders (asked first, then the shader
  // screen with its one bar; task 144: it used to say "about a minute" and build behind Home).
  void OpenRecompileShaders() {
    ui::Cue(native_pack_.Busy() ? ui::Sound::kDeny : ui::Sound::kSelect);
    OpenRebuildShaders();
  }

  // ------------------------------------------------------------- setup ---
  // Before the game is installed, the launcher is a setup screen of its own:
  // it asks for the player's copy of the game (the launcher's artwork comes
  // from it too), then shows the install and the artwork being prepared.
  bool InSetup() const {
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) return true;
#endif
    return !files_ok_ || installing_ || setup_art_ || setup_shaders_ || setup_style_ || installing_update_ ||
           update_shaders_;
  }

  // A version this launcher has not run before, on an installation that has run one (What's new uses
  // the same test; a fresh install has no settings file). Not in test runs (KK_AUTOPLAY), which
  // don't record the version either.
  bool UpdatedSinceLastRun() const {
    if (std::getenv("KK_AUTOPLAY")) return false;
    const std::string last = Get("kk_last_version");
    std::error_code ec;
    return last != KK_VERSION && (!last.empty() || std::filesystem::exists(paths_.config_path, ec));
  }

  // The native renderer's shaders being prepared, as a screen of its own: the setup's step (after the
  // artwork, before the look) and after every update.
  bool ShaderScreen() const {
    if (installing_update_ || installing_ || setup_art_) return false;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) return dev_setup_ == 4;
#endif
    return setup_shaders_ || update_shaders_ || rebuild_shaders_screen_;
  }
  // The setup's step and the plain setup screens draw the night sky; the shader screen the stills.
  bool PlainSetup() const { return InSetup() && !ShaderScreen(); }

  // Home's look (the stills, blurred; the game's logo; the music), with a centred panel: what is
  // happening and why, the PC being slower meanwhile, the progress, and Continue in the background
  // (A, Enter or a click). The build goes on behind Home after that, and Play waits for it.
  void DrawShaderScreen(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    bool after_update = !setup_shaders_ && update_shaders_;
    const bool rebuild = !setup_shaders_ && !update_shaders_ && rebuild_shaders_screen_;
    // ONE bar for the whole job, every API's shaders and pipelines, with one label (owner, 16 Oct: no steps).
    float fraction = native_pack_.DrawnFraction(ImGui::GetTime());
    std::string label = native_pack_.ProgressLabel();
#if defined(KK_DEV_TOOLS)
    {  // Developer: the bar as drawn, once a second (task 144: one bar, smooth, never back).
      static double logged = -10;
      if (std::getenv("KK_DEV_LAUNCHER_PERF") && ImGui::GetTime() - logged >= 1.0) {
        logged = ImGui::GetTime();
        REXLOG_INFO("KK dev: shader bar {:.4f} job {:.4f} label '{}'", fraction, native_pack_.Fraction(), label);
      }
    }
#endif
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ == 4) {
      after_update = false;
      if (!native_pack_.Busy()) {  // screenshots
        fraction = 0.45f;
        label = T(std::string("Preparing shaders"));
      }
    }
#endif
    const std::string caption =
        Upper(T("PC port")) + "  " KK_DOT "  " + Upper(T(after_update ? "Update" : rebuild ? "Settings" : "Setup"));
    const std::string title = after_update ? F("Updated to v{0}: preparing the shaders", {KK_VERSION})
                              : rebuild    ? T(std::string("Rebuilding the shaders"))
                                           : T(std::string("Preparing the shaders"));
    const std::string why = T(
        "The game's shaders are translated for your graphics card from your copy of the game, once, so effects "
        "never stutter the first time they appear.");
    const std::string slow = T(KK_SLOW_PC);
    const std::string button = T("Continue in the background");
    const float pw = std::min(760 * s, w_ - Margin() * 2), pad = 34 * s, r = 12 * s, wrap = pw - pad * 2;
    const float why_h = ui::TextSize(f.text, 16 * s, why, wrap).y, slow_h = ui::TextSize(f.text, 14.5f * s, slow, wrap).y;
    const float bh = 48 * s;
    const float ph = pad + 16 * s + 14 * s + 36 * s + 18 * s + why_h + 10 * s + slow_h + 30 * s + 3 * s + 14 * s +
                     20 * s + 30 * s + bh + pad;
    float lw = 0, lh = 0;
    if (logo_) {
      lh = std::clamp(h_ - ph - 150 * s, 0.0f, 150 * s);
      lw = lh * logo_aspect_;
    }
    const float total_h = (lh > 40 * s ? lh + 30 * s : 0) + ph;
    float y = o_.y + std::max(40 * s, (h_ - 70 * s - total_h) * 0.5f);
    const float cx = o_.x + w_ * 0.5f, x0 = cx - pw * 0.5f, x1 = cx + pw * 0.5f;
    if (lh > 40 * s) {
      dl->AddImage(ui::Tex(logo_), ImVec2(cx - lw * 0.5f, y), ImVec2(cx + lw * 0.5f, y + lh));
      y += lh + 30 * s;
    }
    // The panel, like the pop-ups': dark, a hairline, a brass line on top.
    const ImVec2 p0(x0, y), p1(x1, y + ph);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 6 * s), ImVec2(p1.x, p1.y + 6 * s), IM_COL32(0, 0, 0, 60), r);
    dl->AddRectFilled(p0, p1, ui::WithAlpha(IM_COL32(14, 16, 20, 255), 0.86f), r);
    dl->AddRect(p0, p1, ui::WithAlpha(color::kText, 0.10f), r, 0, 1.0f * s);
    dl->AddRectFilled(ImVec2(p0.x + r, p0.y), ImVec2(p1.x - r, p0.y + 2 * s), color::kAccent);
    float ty = y + pad;
    const float tx = x0 + pad;
    ui::DrawTracked(dl, f.display, 12 * s, ImVec2(tx, ty), color::kAccent, caption, 2.6f * s);
    ty += 16 * s + 14 * s;
    dl->AddText(f.display, 30 * s, ImVec2(tx, ty), color::kText, title.c_str());
    ty += 36 * s + 18 * s;
    dl->AddText(f.text, 16 * s, ImVec2(tx, ty), color::kTextDim, why.c_str(), nullptr, wrap);
    ty += why_h + 10 * s;
    dl->AddText(f.text, 14.5f * s, ImVec2(tx, ty), color::kTextFaint, slow.c_str(), nullptr, wrap);
    ty += slow_h + 30 * s;
    // The progress.
    const float by = std::round(ty);
    dl->AddRectFilled(ImVec2(tx, by), ImVec2(tx + wrap, by + 3 * s), ui::WithAlpha(color::kText, 0.12f));
    if (fraction >= 0) {
      dl->AddRectFilled(ImVec2(tx, by), ImVec2(tx + wrap * std::clamp(fraction, 0.0f, 1.0f), by + 3 * s), color::kAccent);
    } else {
      const float t = float(std::fmod(ImGui::GetTime() * 0.6, 1.0));
      const float a0 = tx + wrap * std::max(0.0f, t * 1.3f - 0.3f), a1 = tx + wrap * std::min(1.0f, t * 1.3f);
      dl->AddRectFilled(ImVec2(a0, by), ImVec2(a1, by + 3 * s), color::kAccent);
    }
    ty += 3 * s + 14 * s;
    const std::string left = fraction >= 0 ? label : T(std::string("Checking"));
    const std::string right = fraction >= 0 ? std::to_string(int(fraction * 100)) + "%" : "";
    dl->AddText(f.text, 15 * s, ImVec2(tx, ty), color::kTextDim, left.c_str());
    dl->AddText(f.text, 15 * s, ImVec2(tx + wrap - ui::TextSize(f.text, 15 * s, right).x, ty), color::kTextDim,
                right.c_str());
    ty += 20 * s + 30 * s;
    // Continue in the background: the panel's one button, with the focus.
    const float bw = std::max(300 * s, ui::TextSize(f.semibold, 16 * s, button).x + 60 * s);
    const ImVec2 b0(x1 - pad - bw, ty), b1(x1 - pad, ty + bh);
    const bool hovered = !modal_.open && ImGui::IsMouseHoveringRect(b0, b1, false);
    dl->AddRectFilled(b0, b1, hovered ? IM_COL32_WHITE : color::kFocus, 4 * s);
    const ImVec2 ts = ui::TextSize(f.semibold, 16 * s, button);
    dl->AddText(f.semibold, 16 * s, ImVec2(b0.x + (bw - ts.x) * 0.5f, b0.y + (bh - ts.y) * 0.5f), color::kOnFocus,
                button.c_str());
    if (!modal_.open && (input_.Pressed(Action::kAccept) || (hovered && ImGui::IsMouseClicked(0)))) {
      input_.Consume(Action::kAccept);
      ContinueShadersInBackground();
    }
  }

  void ContinueShadersInBackground() {
    ui::Cue(ui::Sound::kSelect);
    const bool rebuild = rebuild_shaders_screen_ && !setup_shaders_ && !update_shaders_;
    setup_shaders_ = false;  // the setup goes on to the look step
    update_shaders_ = false;
    rebuild_shaders_screen_ = false;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ == 4) dev_setup_ = 5;
#endif
    if (rebuild) return;  // back to the Graphics page it was asked for on
    page_ = kHome;
    home_focus_ = 0;
  }

  // 0: asking for the disc image, 1: installing, 2: taking the artwork,
  // 3: updating the port, 4: preparing the native renderer's shaders, 5: the look.
  int SetupStage() const {
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) return dev_setup_;
#endif
    return installing_update_ ? 3 : installing_ ? 1 : setup_art_ ? 2 : setup_shaders_ ? 4 : setup_style_ ? 5 : 0;
  }

  void DrawSetup(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float x = o_.x + Margin();
    float y = o_.y + std::max(84 * s, h_ * 0.12f);
    const int stage = SetupStage();
    float copied = 0, copy_total = 0;
    if (installing_) {
      copied = float(progress_.bytes_done.load());
      copy_total = float(progress_.bytes_total.load());
    }
    float art_fraction = art_progress_.fraction.load();
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) {  // screenshots of each stage
      copy_total = 6.3e9f;
      copied = copy_total * 0.37f;
      art_fraction = 0.6f;
    }
#endif

    ui::DrawTracked(dl, f.display, 15 * s, ImVec2(x + 2 * s, y), color::kAccent, "PETER JACKSON'S", 6 * s);
    y += 20 * s;
    ui::DrawTracked(dl, f.display, 66 * s, ImVec2(x - 1 * s, y), color::kText, "KING KONG", 2 * s);
    y += 70 * s;
    ui::DrawTracked(dl, f.display, 13 * s, ImVec2(x + 2 * s, y), color::kTextFaint,
                    Upper(T("PC port")) + "  " KK_DOT "  " + Upper(T(stage == 3 ? "Update" : "Setup")), 4 * s);
    y += 62 * s;

    // The steps (of the first install).
    static const char* const kSteps[] = {"Your copy", "Install", "Play"};
    if (stage != 3) {
    const int step = stage == 0 ? 0 : stage == 5 ? 2 : 1;
    float sx = x;
    for (int i = 0; i < 3; ++i) {
      const ImU32 num = i < step ? color::kTextDim : i == step ? color::kAccent : color::kTextFaint;
      const ImU32 lab = i < step ? color::kTextDim : i == step ? color::kText : color::kTextFaint;
      const std::string n = "0" + std::to_string(i + 1);
      ui::DrawTracked(dl, f.display, 13 * s, ImVec2(sx, y), num, n, 1.5f * s);
      sx += ui::TrackedWidth(f.display, 13 * s, n, 1.5f * s) + 10 * s;
      const std::string step_name = Upper(T(kSteps[i]));
      ui::DrawTracked(dl, f.display, 13 * s, ImVec2(sx, y), lab, step_name, 2.2f * s);
      sx += ui::TrackedWidth(f.display, 13 * s, step_name, 2.2f * s) + 18 * s;
      if (i < 2) {
        dl->AddLine(ImVec2(sx, std::round(y + 7 * s)), ImVec2(sx + 36 * s, std::round(y + 7 * s)),
                    i < step ? WithAlphaText(0.4f) : color::kHairline, 1.0f);
        sx += 36 * s + 18 * s;
      }
    }
    }
    y += stage == 3 ? 8 * s : 48 * s;

    const float col_w = std::min(620 * s, w_ * 0.52f);
    auto heading = [&](const char* english) {
      dl->AddText(f.display, 30 * s, ImVec2(x, y), color::kText, T(english));
      y += 46 * s;
    };
    auto body = [&](const std::string& english) {
      const std::string text = T(english);
      dl->AddText(f.text, 16 * s, ImVec2(x, y), color::kTextDim, text.c_str(), nullptr, col_w);
      y += ui::TextSize(f.text, 16 * s, text, col_w).y + 18 * s;
    };
    auto bar = [&](float fraction) {
      const float by = std::round(y);
      dl->AddRectFilled(ImVec2(x, by), ImVec2(x + col_w, by + 3 * s), ui::WithAlpha(color::kText, 0.12f));
      if (fraction >= 0) {
        dl->AddRectFilled(ImVec2(x, by), ImVec2(x + col_w * std::clamp(fraction, 0.0f, 1.0f), by + 3 * s), color::kAccent);
      } else {
        const float t = float(std::fmod(ImGui::GetTime() * 0.6, 1.0));
        const float a0 = x + col_w * std::max(0.0f, t * 1.3f - 0.3f), a1 = x + col_w * std::min(1.0f, t * 1.3f);
        dl->AddRectFilled(ImVec2(a0, by), ImVec2(a1, by + 3 * s), color::kAccent);
      }
      y += 16 * s;
    };
    auto caption_pair = [&](const std::string& left, const std::string& right) {
      dl->AddText(f.text, 15 * s, ImVec2(x, y), color::kText, left.c_str());
      const float rw = ui::TextSize(f.text, 15 * s, right).x;
      dl->AddText(f.text, 15 * s, ImVec2(x + col_w - rw, y), color::kTextDim, right.c_str());
      y += 46 * s;
    };
    auto gb = [](float bytes) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.1f", bytes / 1e9f);
      return std::string(buf);
    };

    std::vector<HomeEntry> entries;
    if (stage == 0) {
      heading("Your copy of the game");
      body("This port plays the original Xbox 360 game, so it needs a disc image (.iso) that you made from your "
           "own disc. The game's files are copied from it, and so is the artwork this launcher shows. Nothing from "
           "the game comes with the port.");
      DrawSetupCards(dl);
      y += 8 * s;
      entries.push_back({"Select disc image", true, [this] { StartInstall(); }});
      entries.push_back(LanguageEntry());
      {
        HomeEntry e;
        e.label = "Online players";
        e.value = T(GetBool("kk_online_count") ? "On" : "Off");
        e.change = [this](int) { SetOnlineCount(!GetBool("kk_online_count")); };
        entries.push_back(std::move(e));
      }
      entries.push_back({"Quit", true, [this] { Quit(); }});
    } else if (stage == 1) {
      heading("Installing");
      body("Copying the game's files from your disc image. This takes a few minutes.");
      bar(copy_total > 0 ? copied / copy_total : -1.0f);
      caption_pair(copy_total > 0 ? F("{0} of {1} GB", {gb(copied), gb(copy_total)}) : T("Reading the disc image"),
                   copy_total > 0 ? std::to_string(int(copied / copy_total * 100)) + "%" : "");
      entries.push_back({"Cancel", true, [this] { progress_.cancel = true; }});
    } else if (stage == 2) {
      heading("Preparing the launcher");
      body("Taking the game's logo and stills from the movie from your copy of the game.");
      bar(art_fraction);
      y += 30 * s;
    } else if (stage == 5) {
      heading("Choose a look");
      body("Pick how King Kong looks. You can change it at any time with Look on the Graphics page.");
      DrawStyleCards(dl, y);
    } else {
      float got = 0, size = 0;
      std::string version;
      if (update_) {
        got = float(update_->bytes.load());
        size = float(update_->total.load());
        if (update_->found) version = update_->found->version;
      }
      bool finishing = update_ && !update_->busy && update_->done;
#if defined(KK_DEV_TOOLS)
      if (dev_setup_ >= 0) version = "9.9.9", size = 61.4e6f, got = size * 0.42f, finishing = false;
#endif
      heading(finishing ? "Restarting" : "Updating");
      body(F("Downloading version {0} of the port. The launcher restarts by itself when it's done; your installed "
             "game, saves and settings stay as they are.",
             {version}));
      bar(finishing ? 1.0f : size > 0 ? got / size : -1.0f);
      auto mb = [](float bytes) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", bytes / 1e6f);
        return std::string(buf);
      };
      caption_pair(size > 0 ? F("{0} of {1} MB", {mb(got), mb(size)}) : T(std::string("Connecting")),
                   size > 0 ? std::to_string(int(got / size * 100)) + "%" : "");
    }
    if (stage == 0) DrawSidePanels(dl, 1.0f, false, entries, setup_focus_, o_.y + h_ - 120 * s);
    else side_focus_ = -1;
    if (const int i = DrawMenu(dl, entries, setup_focus_, x, y, 1.0f); i >= 0) entries[size_t(i)].action();
    if (!modal_.open && input_.Pressed(Action::kBack)) {
      input_.Consume(Action::kBack);
      if (stage <= 1) ui::Cue(ui::Sound::kBack);
      if (stage == 0) OpenQuit();
      else if (stage == 1) progress_.cancel = true;
    }
  }

  static ImU32 WithAlphaText(float a) { return ui::WithAlpha(color::kText, a); }

  // The setup's last step: two cards, Original and Remastered, side by side under the heading.
  // Left and Right (or Up and Down) move between them, A or Enter or a click picks one.
  void DrawStyleCards(ImDrawList* dl, float top) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float x0 = o_.x + Margin(), right = o_.x + w_ - Margin();
    const float gap = 28 * s, pad = 18 * s, r = 10 * s;
    const float cw = std::min((right - x0 - gap) * 0.5f, 520 * s);
    const float bottom = o_.y + h_ - 92 * s;  // above the prompts
    static const GpuInfo gpu = DetectGpu();
    struct Card {
      const char* caption;
      const char* title;
      const char* text;
      std::string fact;
      rex::ui::ImmediateTexture* still;
    };
    const Card cards[2] = {
        {"Original", "The Xbox 360 as it was",
         "The console's own picture: 720p at 30 FPS, its anti-aliasing and texture filtering, the 69" KK_DEG " field "
         "of view, motion blur, screen blur and distance fog. No ambient occlusion.",
         T("720p  " KK_DOT "  30 FPS  " KK_DOT "  Xbox 360 settings"), still_low_},
        {"Remastered", "Sharper, with soft shadows",
         "Drawn for your screen's resolution with sharper textures, and the new GTAO ambient occlusion: soft "
         "shading in corners, in creases and under rocks, grass and people.",
         F("Recommended for your {0}  " KK_DOT "  60 FPS", {gpu.name.empty() ? T(std::string("graphics card")) : gpu.name}),
         slides_.empty() ? nullptr : slides_[0].sharp},
    };
    // The text under each still, measured; the stills take what's left (cropped to fit).
    const float wrap = cw - pad * 2;
    float text_h = 0;
    for (const Card& c : cards)
      text_h = std::max(text_h, 22 * s + 36 * s + ui::TextSize(f.text, 14.5f * s, T(std::string(c.text)), wrap).y + 10 * s +
                                    ui::TextSize(f.text, 12.5f * s, c.fact, wrap).y);
    const float still_h = std::clamp(bottom - top - text_h - pad * 2, 60 * s, cw * 9.0f / 16.0f);
    const float ch = still_h + pad + text_h + pad;

    auto choose = [this](int i) {
      ui::Cue(ui::Sound::kSelect);
      if (i == 0) ApplyOriginal();
      else ApplyRemastered();
      REXLOG_INFO("KK: setup: look {}", i == 0 ? "Original" : "Remastered");
      setup_style_ = false;
#if defined(KK_DEV_TOOLS)
      dev_setup_ = -1;
#endif
      Status(InstalledMessage());
    };
    const int before = style_focus_;
    int chosen = -1;
    if (!modal_.open) {
      if (input_.Pressed(Action::kLeft) || input_.Pressed(Action::kUp)) style_focus_ = 0;
      if (input_.Pressed(Action::kRight) || input_.Pressed(Action::kDown)) style_focus_ = 1;
      if (input_.Pressed(Action::kAccept)) chosen = style_focus_;
    }
    int hovered = -1;
    for (int i = 0; i < 2; ++i) {
      const float cx = x0 + float(i) * (cw + gap);
      if (!modal_.open && ImGui::IsMouseHoveringRect(ImVec2(cx, top), ImVec2(cx + cw, top + ch), false)) hovered = i;
    }
    if (hovered >= 0 && input_.mouse_moved()) style_focus_ = hovered;
    if (hovered >= 0 && ImGui::IsMouseClicked(0)) chosen = style_focus_ = hovered;
    if (style_focus_ != before) ui::Cue(ui::Sound::kMove);

    for (int i = 0; i < 2; ++i) {
      const Card& c = cards[i];
      const float cx = x0 + float(i) * (cw + gap);
      const bool focused = style_focus_ == i, on = focused || hovered == i;
      const ImVec2 p0(cx, top), p1(cx + cw, top + ch);
      dl->AddRectFilled(ImVec2(p0.x, p0.y + 5 * s), ImVec2(p1.x, p1.y + 5 * s), IM_COL32(0, 0, 0, 40), r);
      dl->AddRectFilled(p0, p1, ui::WithAlpha(on ? IM_COL32(30, 33, 40, 255) : IM_COL32(16, 18, 22, 255), on ? 0.80f : 0.62f), r);
      // The still, cropped to the room it has.
      const ImVec2 s0(p0.x, p0.y), s1(p1.x, p0.y + still_h);
      if (c.still) {
        const float want = cw / still_h, aspect = 16.0f / 9.0f;
        float v0 = 0, v1 = 1;
        if (want > aspect) {
          const float v = aspect / want;
          v0 = (1 - v) * 0.5f;
          v1 = v0 + v;
        }
        dl->AddImageRounded(ui::Tex(c.still), s0, s1, ImVec2(0, v0), ImVec2(1, v1),
                            ui::WithAlpha(IM_COL32_WHITE, on ? 1.0f : 0.78f), r, ImDrawFlags_RoundCornersTop);
      } else {
        dl->AddRectFilledMultiColor(s0, s1, IM_COL32(22, 28, 36, 255), IM_COL32(26, 33, 42, 255), color::kBase,
                                    color::kBase);
      }
      dl->AddRect(p0, p1, ui::WithAlpha(color::kText, on ? 0.30f : 0.09f), r, 0, 1.0f * s);
      if (focused)  // the focus ring, as on the other focused things
        dl->AddRect(ImVec2(p0.x - 3 * s, p0.y - 3 * s), ImVec2(p1.x + 3 * s, p1.y + 3 * s),
                    ui::WithAlpha(color::kFocus, 0.85f), r + 3 * s, 0, 2.0f * s);
      float ty = s1.y + pad;
      ui::DrawTracked(dl, f.display, 12 * s, ImVec2(cx + pad, ty), color::kAccent, Upper(T(c.caption)), 2.6f * s);
      ty += 22 * s;
      dl->AddText(f.display, 24 * s, ImVec2(cx + pad, ty), color::kText, T(c.title));
      ty += 36 * s;
      const std::string text = T(std::string(c.text));
      dl->AddText(f.text, 14.5f * s, ImVec2(cx + pad, ty), on ? color::kText : color::kTextDim, text.c_str(), nullptr, wrap);
      ty += ui::TextSize(f.text, 14.5f * s, text, wrap).y + 10 * s;
      dl->AddText(f.text, 12.5f * s, ImVec2(cx + pad, ty), color::kTextFaint, c.fact.c_str(), nullptr, wrap);
    }
    if (chosen >= 0) {
      input_.Consume(Action::kAccept);
      choose(chosen);
    }
  }

  // The status line once the setup is over.
  const char* InstalledMessage() {
    return NativeShaderPack::Wanted() && native_pack_.Finished() && !native_pack_.Ok()
               ? "Installed. The shaders were not prepared: the game compiles them as it starts."
               : "Installed. Your copy of King Kong is ready to play.";
  }

  // The setup screen's right side: which disc works, and Online players.
  void DrawSetupCards(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float right = o_.x + w_ - Margin();
    const float x0 = std::max(o_.x + Margin() + std::min(620 * s, w_ * 0.52f) + 48 * s, right - 470 * s);
    const float w = right - x0, pad = 18 * s, r = 10 * s;
    const float label_w = 118 * s, value_w = w - pad * 2 - label_w;
    float y = o_.y + std::max(84 * s, h_ * 0.12f) + 4 * s;

    auto card = [&](float top, float bottom) {
      dl->AddRectFilled(ImVec2(x0, top + 5 * s), ImVec2(right, bottom + 5 * s), IM_COL32(0, 0, 0, 40), r);
      dl->AddRectFilled(ImVec2(x0, top), ImVec2(right, bottom), ui::WithAlpha(IM_COL32(16, 18, 22, 255), 0.62f), r);
      dl->AddRect(ImVec2(x0, top), ImVec2(right, bottom), ui::WithAlpha(color::kText, 0.09f), r, 0, 1.0f * s);
    };
    auto caption = [&](float cy, const char* english) {
      ui::DrawTracked(dl, f.display, 12 * s, ImVec2(x0 + pad, cy), color::kAccent, Upper(T(english)), 2.6f * s);
    };

    // Which disc works.
    struct Row {
      const char* label;
      std::string value;
      bool mono;
    };
    const Row rows[] = {
        {"Game", T("Peter Jackson's King Kong: The Official Game of the Movie"), false},
        {"Console", "Xbox 360", false},
        {"Region", T("USA and Europe"), false},
        {"Title ID", "555307D3", true},
        {"Version", "0.0.0.1", true},
        {"Disc image", T(".iso, 7,834,892,288 bytes"), false},
        {"SHA-1", "075F43709E9C9A095E099AB5056CC04B768288D1", true},
        {"Installed", T("About 6.3 GB"), false},
    };
    const float fs = 13.5f * s, line = 7 * s;
    float h = pad + 22 * s;
    for (const Row& row : rows) h += ui::TextSize(f.text, row.mono ? 12.5f * s : fs, row.value, value_w).y + line;
    const std::string note = T("Make the image from your own disc. Other versions and regions won't work.");
    h += 6 * s + ui::TextSize(f.text, 12.5f * s, note, w - pad * 2).y + pad;
    card(y, y + h);
    caption(y + pad, "Supported disc");
    float ry = y + pad + 26 * s;
    for (const Row& row : rows) {
      const float vfs = row.mono ? 12.5f * s : fs;
      dl->AddText(f.text, 13 * s, ImVec2(x0 + pad, ry + (vfs - 13 * s) * 0.5f), color::kTextDim, T(row.label));
      dl->AddText(row.mono ? f.semibold : f.text, vfs, ImVec2(x0 + pad + label_w, ry), color::kText, row.value.c_str(),
                  nullptr, value_w);
      ry += ui::TextSize(f.text, vfs, row.value, value_w).y + line;
    }
    dl->AddLine(ImVec2(x0 + pad, ry + 2 * s), ImVec2(right - pad, ry + 2 * s), color::kHairline, 1.0f);
    dl->AddText(f.text, 12.5f * s, ImVec2(x0 + pad, ry + 8 * s), color::kTextDim, note.c_str(), nullptr, w - pad * 2);
    y += h + 14 * s;

    // Online players.
    const bool on = GetBool("kk_online_count");
    const std::string text = T(
        "Shows how many players are online, at the top right of the launcher. It only sends a tiny anonymous check-in "
        "every 5 minutes: a random number, the port's version, Windows or Linux, and whether you're playing. Nothing "
        "that identifies you or your PC is kept. Switch it off with Online players on the left, or on the About page, "
        "at any time.");
    const float th = ui::TextSize(f.text, 12.5f * s, text, w - pad * 2).y;
    const float oh = pad + 22 * s + th + pad;
    card(y, y + oh);
    caption(y + pad, "Online players");
    const std::string state = Upper(T(on ? "On" : "Off"));
    const float sw = ui::TrackedWidth(f.display, 12 * s, state, 2.6f * s);
    ui::DrawTracked(dl, f.display, 12 * s, ImVec2(right - pad - sw, y + pad), on ? color::kGood : color::kTextFaint,
                    state, 2.6f * s);
    dl->AddText(f.text, 12.5f * s, ImVec2(x0 + pad, y + pad + 24 * s), color::kTextDim, text.c_str(), nullptr,
                w - pad * 2);
  }

  // The game's language (and the launcher's), on the setup screen.
  HomeEntry LanguageEntry() {
    HomeEntry e;
    e.label = "Language";
    const int cur = GetInt("user_language", 1);
    int index = 0;
    for (size_t i = 0; i < std::size(kLanguages); ++i)
      if (kLanguages[i].first == cur) index = int(i);
    e.value = kLanguages[size_t(index)].second;
    e.change = [index](int dir) {
      const int n = int(std::size(kLanguages));
      SetInt("user_language", kLanguages[size_t((index + dir + n) % n)].first);
    };
    return e;
  }

  // Game, shader pack and version, in three columns.
  void DrawHomeStatus(ImDrawList* dl, float x, float y, float alpha) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    struct Column {
      std::string caption;
      std::string value;
      ImU32 color;
      float progress;  // < 0: none
    };
    Column cols[3];
    const bool selectable = ShaderStatusSelectable();
    if (installing_) {
      const double total = std::max<double>(1.0, double(progress_.bytes_total.load()));
      const float p = float(double(progress_.bytes_done.load()) / total);
      cols[0] = {Upper(T("Game")), F("Installing {0}%", {std::to_string(int(p * 100))}), color::kText, p};
    } else {
      cols[0] = {Upper(T("Game")), T(files_ok_ ? "Ready to play" : "Not installed"),
                 files_ok_ ? color::kText : color::kWarn, -1};
    }
    cols[1] = {Upper(T("Shader pack")), PackStatusText(), pack_.failed ? color::kWarn : color::kText, PackProgress()};
    if (NativeShaderPack::Wanted())  // the native renderer's own shaders instead (native_shaders.h)
      cols[1] = {Upper(T("Shaders")), native_pack_.StatusText(),
                 native_pack_.Finished() && !native_pack_.Ok() ? color::kWarn : color::kText,
                 native_pack_.DrawnFraction(ImGui::GetTime())};
    if (update_ && update_->found)
      cols[2] = {Upper(T("Version")), F("v{0}   " KK_DOT "   v{1} is out", {KK_VERSION, update_->found->version}),
                 color::kAccent, -1};
    else
      cols[2] = {Upper(T("Version")), "v" KK_VERSION, color::kText, -1};
    float cx = x;
    for (int i = 0; i < 3; ++i) {
      const Column& c = cols[i];
      const float col_w = std::max(210 * s, ui::TextSize(f.text, 15.5f * s, c.value).x + 48 * s);
      if (i == 1 && selectable) {
        // The shaders' status is a button (Recompile shaders): a tile when it has the focus or the mouse.
        const ImVec2 p0(cx - 12 * s, y - 9 * s), p1(cx + col_w - 22 * s, y + 67 * s);
        status_rect_[0] = p0;
        status_rect_[1] = p1;
        const bool hovered = !modal_.open && ImGui::IsMouseHoveringRect(p0, p1, false);
        if (status_focus_ || hovered) {
          dl->AddRectFilled(p0, p1, ui::WithAlpha(IM_COL32(30, 33, 40, 255), 0.6f * alpha), 10 * s);
          dl->AddRect(p0, p1, ui::WithAlpha(color::kText, 0.22f * alpha), 10 * s, 0, 1.0f * s);
        }
        if (status_focus_)
          dl->AddRect(ImVec2(p0.x - 3 * s, p0.y - 3 * s), ImVec2(p1.x + 3 * s, p1.y + 3 * s),
                      ui::WithAlpha(color::kFocus, 0.85f * alpha), 13 * s, 0, 2.0f * s);
      } else if (i == 1) {
        status_rect_[0] = status_rect_[1] = ImVec2(0, 0);
      }
      ui::DrawTracked(dl, f.display, 12 * s, ImVec2(cx, y), ui::WithAlpha(color::kTextFaint, alpha), c.caption, 2.2f * s);
      dl->AddText(f.text, 15.5f * s, ImVec2(cx, y + 20 * s), ui::WithAlpha(c.color, alpha), c.value.c_str());
      if (c.progress >= 0) {
        const float bw = 150 * s, by = y + 46 * s;
        dl->AddRectFilled(ImVec2(cx, by), ImVec2(cx + bw, by + 2 * s), ui::WithAlpha(color::kText, 0.12f * alpha));
        dl->AddRectFilled(ImVec2(cx, by), ImVec2(cx + bw * std::clamp(c.progress, 0.0f, 1.0f), by + 2 * s),
                          ui::WithAlpha(color::kAccent, alpha));
        // The native renderer's shaders being prepared: the PC is busier until it's done.
        if (i == 1 && NativeShaderPack::Wanted())
          dl->AddText(f.text, 11.5f * s, ImVec2(cx, by + 6 * s), ui::WithAlpha(color::kTextFaint, alpha),
                      T("Your PC may run slower until this finishes."));
      }
      cx += col_w;
    }
  }

  std::vector<Item> AdvancedPageItems() {
    auto items = AdvancedItems();
    items.push_back(ActionItem("Copy diagnostics",
        "Copies version, hardware, settings, fallbacks and recent errors. Names and paths are redacted. Nothing is uploaded.",
        "Copy", [this] {
          const auto report = DiagnosticsReport();
          ImGui::SetClipboardText(report.c_str());
          Status("Diagnostics copied to the clipboard.");
        }));
    if (Cached(crash_reports_cache_, 2.0, [this] { return HasCrashReports(paths_.user_dir); })) {
      items.push_back(ActionItem("Saved crash reports",
          "A previous crash saved local diagnostics and a minidump. Dumps may contain private process memory. Review before sharing. Nothing is uploaded.",
          "Open folder", [this] { OpenInExplorer(paths_.user_dir / "crashes"); }));
    }
    return items;
  }

  // ----------------------------------------------------- settings pages ---
  std::string MenuText(uint32_t id) {
    static constexpr const char* codes[] = {"en", "de", "es", "fr", "it"};
    const std::string lang = codes[int(text::Current())];
    if (extras_lang_ != lang) {
      extras_texts_ = art::ReadMenuTexts(paths_.game_dir, lang.c_str());
      extras_lang_ = lang;
    }
    auto it = extras_texts_.find(id);
    return it == extras_texts_.end() ? std::string() : it->second;
  }

  static void ReplaceText(std::string& text, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size()) text.replace(at, from.size(), to);
  }

  std::string ExtraHint(const play::ExtraInfo& e, const play::ExtraProgress& progress) {
    std::string hint;
    if (e.progress > 100) hint = MenuText(0x4902EF32);
    else if (e.score >= 0) hint = MenuText(progress.percent < 100 ? 0x4902EF2D : 0x4902EF2B);
    else hint = MenuText(0x4902EF2A);
    ReplaceText(hint, "\\m1\\", e.progress > 100 ? MenuText(0x4902EC04 + e.id) : std::to_string(progress.percent));
    ReplaceText(hint, "\\m2\\", std::to_string(e.progress));
    ReplaceText(hint, "\\m3\\", std::to_string(progress.score));
    ReplaceText(hint, "\\m4\\", std::to_string(e.score));
    ReplaceText(hint, "\\cFF7FFF\\", "");
    ReplaceText(hint, "\\cFFFFFF\\", "");
    return hint;
  }

  std::vector<Item> ExtrasItems() {
    std::vector<Item> items;
    const int slot = ChapterSlot();
    Item save;
    save.type = ItemType::kChoice;
    save.label = "Save";
    save.description = "Bonuses use this save's progress and scores.";
    save.index = -1;
    std::vector<int> slots;
    for (const auto& sv : saves_) if (sv.exists) {
      if (sv.index == slot) save.index = int(slots.size());
      slots.push_back(sv.index);
      save.options.push_back(SaveName(sv));
    }
    save.value_text = "No save yet";
    save.on_choice = [this, slots](int i) { chapter_slot_ = slots[size_t(i)]; };
    save.disabled = slots.empty();
    items.push_back(std::move(save));
    items.push_back(Section("Extras"));
    auto progress = ExtraProgressFor(slot);
    progress.museum |= GetBool("kk_cheats") && GetBool("kk_cheat_bonus");
    for (const auto& extra : play::kExtras) {
      const int id = extra.id;
      Item it;
      it.type = ItemType::kAction;
      it.label = MenuText(0x4902EC04 + id);
      if (it.label.empty()) it.label = F("Bonus {0}", {std::to_string(id + 1)});
      const bool unlocked = play::ExtraUnlocked(extra, progress);
      it.value_text = unlocked ? "Unlocked" : "Locked";
      it.description = unlocked ? T("Play this bonus, then return to Extras.") : ExtraHint(extra, progress);
      it.value_color = unlocked ? color::kAccent : color::kTextFaint;
      // Locked rows stay selectable so a controller can read their unlock hints.
      if (unlocked && extra.type == play::ExtraType::kMode) {
        it.description = MenuText(0x4902EF30);
        ReplaceText(it.description, "\\m1\\", MenuText(id == 1 ? 0x4902D4FA : 0x4902D4EE));
        // Opens its row on the Graphics page; B comes back to Extras on the same row (task 144: the
        // Extras page kept a stale focus after a visit to Graphics).
        it.on_activate = [this, slot, row = std::string(id == 1 ? "Old movie filter" : "Horizontal flip")] {
          profile_slot_ = slot;
          profile_read_at_ = -1;
          const int extras_focus = play_lists_[int(PlayView::kExtras)].focus;
          GoTo(kGraphics);
          ui::ListState& st = lists_[kGraphics * 4 + kNoSubPage];
          st.focus = -1;
          st.focus_label = row;
          extras_return_focus_ = extras_focus;
        };
      } else if (unlocked) it.on_activate = [this, slot, id] { PlayWith({play::StartKind::kExtra, slot, id, ""}); };
      else it.on_activate = [] { ui::Cue(ui::Sound::kDeny); };
      items.push_back(std::move(it));
    }
    return items;
  }

  void DrawSettings(ImDrawList* dl) {
    const float s = S();
    std::vector<Item> items;
    switch (page_) {
      case kDisplay: items = DisplayItems(); break;
      case kGraphics: items = GraphicsItems(); break;
      case kGameplay: items = GameplayItems(); break;
      case kAudio: items = AudioItems(); break;
      case kControls:
        items = sub_ == kRemapPage ? RemapItems() : sub_ == kKeysPage ? KeyItems() : ControlsItems();
        break;
      case kAdvanced: items = AdvancedPageItems(); break;
      case kAchievements: items = AchievementItems(); break;
      case kAbout: items = AboutItems(); break;
      default: break;
    }
#if defined(KK_DEV_TOOLS)
    if (IsSettingsPage(page_) && sub_ == kNoSubPage) DevCheckLayout(page_ - kFirstSettingsPage, items, "launcher");
#endif
    const float top = o_.y + 104 * s, bottom = o_.y + h_ - 96 * s;
    const float x0 = o_.x + Margin();
    const float avail = w_ - Margin() * 2;
    const float list_w = std::round(avail * 0.56f);
    ui::ListState& st = lists_[page_ * 4 + sub_];
    const Item* focused = ui::DrawList(dl, items, st, ImVec2(x0, top), ImVec2(x0 + list_w, bottom), m_, input_,
                                       !modal_.open);
    const float dx = x0 + list_w + 76 * s;
    ui::DrawDescription(dl, focused, ImVec2(dx, top + 18 * s), ImVec2(o_.x + w_ - Margin(), bottom), m_);
    // Kept for the prompts (the items live until the next frame's rebuild).
    focused_items_ = std::move(items);
    focused_ = st.focus >= 0 && st.focus < int(focused_items_.size()) ? &focused_items_[size_t(st.focus)] : nullptr;
  }

#if defined(KK_DEV_TOOLS)
  // Developer: every settings page as it is built now, checked against settings_layout.h and logged.
  void LogLayoutCheck() {
    const SubPage sub = sub_;
    sub_ = kNoSubPage;
    for (int p = kFirstSettingsPage; p <= kLastSettingsPage; ++p) {
      std::vector<Item> items;
      switch (Page(p)) {
        case kDisplay: items = DisplayItems(); break;
        case kGraphics: items = GraphicsItems(); break;
        case kAudio: items = AudioItems(); break;
        case kControls: items = ControlsItems(); break;
        case kGameplay: items = GameplayItems(); break;
        case kAdvanced: items = AdvancedPageItems(); break;
        default: break;
      }
      for (const Item& it : items)
        REXLOG_INFO("KK dev: launcher page {} {} {}", kPageNames[p], it.type == ItemType::kSection ? "section" : "row",
                    it.label);
      DevCheckLayout(p - kFirstSettingsPage, items, "launcher");
    }
    sub_ = sub;
  }
#endif

  // Graphics > Advanced > Rebuild shaders: asked first (A on Rebuild, B or Cancel to keep them), then the
  // shader screen with its one bar, as at setup.
  void OpenRebuildShaders() {
    if (native_pack_.Busy()) {
      ui::Cue(ui::Sound::kDeny);
      Status("The shaders are being prepared right now.");
      return;
    }
    OpenModal("Rebuild shaders",
              "Prepare all of the game's shaders again? This takes a few minutes. " KK_SLOW_PC, {"Rebuild", "Cancel"},
              1,
              [this](int i) {
                if (i != 0 || native_pack_.Busy()) return;
                native_pack_.Start(CacheDir(), paths_.game_dir, true);
                rebuild_shaders_screen_ = true;
                REXLOG_INFO("KK: rebuilding the native renderer's shaders (asked for)");
              },
              1);
  }

  // ------------------------------------------------------- Achievements ---
  std::vector<Item> AchievementItems() {
    std::vector<Item> items;
    items.push_back(ActionItem("Test notification", "Shows a sample notification right now.", "", [this] {
      if (!GetBool("kk_achievement_toasts")) Status("Notifications are off; switch them on to see the test.");
      const uint32_t icon = achievements_.empty() ? 0 : achievements_[test_index_++ % achievements_.size()].id;
      toast_->Show("Test achievement", 10, icon);
    }));

    items.push_back(Section("PC port"));
    for (const auto& pa : PortAchievements()) {
      const bool unlocked = Cached(port_achievement_cache_.try_emplace(pa.id, -1e9, false).first->second, 2.0,
                                   [&] { return IsPortAchievementUnlocked(paths_.user_dir, pa.id); });
      Item it;
      it.type = ItemType::kInfo;
      it.tall = true;
      it.label = pa.title;
      it.description = pa.description;
      it.icon = title_icon_;
      it.icon_dim = !unlocked;
      it.value_text = unlocked ? "Unlocked" : "Locked";
      it.value_color = unlocked ? color::kAccent : color::kTextFaint;
      it.facts.push_back(unlocked ? "Unlocked" : "Not unlocked yet");
      items.push_back(std::move(it));
    }

    uint32_t unlocked = 0, score = 0, total_score = 0;
    for (auto& a : achievements_) {
      total_score += a.gamerscore;
      if (a.unlocked) {
        ++unlocked;
        score += a.gamerscore;
      }
    }
    std::string caption = "King Kong";
    if (!achievements_.empty()) {
      caption += "   " KK_DOT "   " +
                 F("{0} of {1} unlocked", {std::to_string(unlocked), std::to_string(achievements_.size())});
      if (have_achievement_names_)
        caption += "   " KK_DOT "   " + std::to_string(score) + " / " + std::to_string(total_score) + " G";
    }
    items.push_back(Section(caption));
    if (achievements_.empty()) {
      Item it;
      it.type = ItemType::kInfo;
      it.label = files_ok_ ? "Start the game once to see them" : "Install the game to see its achievements";
      it.description = "The game's achievements, with their pictures, are read from the game the first time it runs.";
      items.push_back(std::move(it));
    }
    for (const auto& a : achievements_) {
      Item it;
      it.type = ItemType::kInfo;
      it.tall = true;
      it.label = a.label.empty() ? F("Achievement {0}", {std::to_string(a.id)}) : a.label;
      it.description = a.unlocked || a.unachieved.empty() ? a.description : a.unachieved;
      if (!have_achievement_names_) it.description = "Its name and description appear after you've played once.";
      it.icon = a.icon;
      it.icon_dim = !a.unlocked;
      if (a.gamerscore) it.value_text = std::to_string(a.gamerscore) + " G";
      it.value_color = a.unlocked ? color::kAccent : color::kTextFaint;
      it.facts.push_back(a.unlocked ? "Unlocked" : "Not unlocked yet");
      if (a.gamerscore) it.facts.push_back(F("{0} Gamerscore", {std::to_string(a.gamerscore)}));
      items.push_back(std::move(it));
    }
    return items;
  }

  // -------------------------------------------------------------- About ---
  std::vector<Item> AboutItems() {
    std::vector<Item> items;
    items.push_back(Section("King Kong PC port"));
    Item benchmark = ActionItem("Run benchmark",
        "Replays benchmark-route.kkbr beside the game, using your graphics settings and private saves. Results and route drift are saved locally.",
        "Start", [this] { OpenBenchmark(); });
    items.push_back(Disable(benchmark, !CanPlay(), "Install the game first."));
    {
      Item it;
      it.type = ItemType::kInfo;
      it.label = "Version";
      it.value_text = "v" KK_VERSION;
      it.description =
          "Peter Jackson's King Kong: The Official Game of the Movie (Ubisoft, 2005) running natively on PC: the "
          "original Xbox 360 game code statically recompiled to C++ with the ReXGlue SDK. No game code or assets are "
          "included; the game runs from your own disc image.";
      it.facts = {"ReXGlue SDK 0.10.0", F("Title {0}, v{1}", {"555307D3", "0.0.0.1"})};
      items.push_back(std::move(it));
    }
    {
      Item it;
      it.type = ItemType::kLink;
      it.label = "Version history";
      it.description = "What each version of the port added, newest first.";
      it.on_activate = [this] { OpenVersionHistory(); };
      items.push_back(std::move(it));
    }
    {
      Item it;
      it.type = ItemType::kInfo;
      it.label = "Graphics panel";
      it.value_text = "F8";
      it.description = "Press F8 while playing to try graphics settings live, and hold F7 to compare with your saved settings. Nothing changes for good until you save. Copy settings and F6 screenshots make it easy to share what you find.";
      items.push_back(std::move(it));
    }
    items.push_back(ActionItem("Project page", "The port's GitHub page: releases, issues and the source code.",
                               "GitHub", [] { OpenUrl(kProjectUrl); }));
    items.push_back(ActionItem("Discord",
                               "The official Discord server: news, upcoming features, early builds, and a place to "
                               "tell us what you think.",
                               "Join", [] { OpenUrl(kDiscordUrl); }));
    {
      // The upscalers' credits. NVIDIA's DLSS SDK licence (supplement 7.1 b) asks for the SDK to be credited in
      // the about box; its marks (logos) need NVIDIA's written approval first (7.2 c), so it is text only until
      // then (native-renderer/docs/upscalers.md). The project is not endorsed by NVIDIA, AMD or Intel.
      Item it;
      it.type = ItemType::kInfo;
      it.label = "Upscaler credits";
      it.description =
          "The native renderer's temporal upscalers use NVIDIA DLSS (the NVIDIA DLSS SDK), AMD FidelityFX Super "
          "Resolution (FSR 3.1 and FSR 4, the AMD FidelityFX SDK) and Intel XeSS (the Intel XeSS SDK). NVIDIA, "
          "GeForce RTX, RTX and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. "
          "and other countries. AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc. Intel and "
          "XeSS are trademarks of Intel Corporation. This project is not sponsored or endorsed by NVIDIA, AMD or "
          "Intel. Uses SMAA. Copyright (C) 2011 by Jorge Jimenez, Jose I. Echevarria, "
          "Tiago Sousa, Belen Masia, Fernando Navarro and Diego Gutierrez. "
          "Their licences are in THIRD-PARTY-NOTICES.txt beside the game.";
      items.push_back(std::move(it));
    }

    items.push_back(Section("Launcher"));
    items.push_back(CvarToggle("Show this launcher",
                               "Off starts the game directly. Hold Shift while starting to bring the launcher back.",
                               "kk_launcher", "Off", "At startup"));
    items.push_back(CvarToggle("Check for updates",
                               "Checks GitHub for a newer version of the port each time the launcher opens, and offers "
                               "it. The shader pack keeps itself up to date either way.",
                               "kk_check_updates", "Off", "At startup"));
    items.push_back(ActionItem("Check for updates now", "Looks for a newer version of the port right now.",
                               update_ && update_->busy ? "Checking..." : "",
                               [this] { StartUpdateCheck(true); }));
    {
      Item it = CvarToggle("Online player count", kOnlineAbout, "kk_online_count");
      it.on_choice = [this](int i) { SetOnlineCount(i == 1); };
      it.on_default = [this] { SetOnlineCount(false); };
      if (const online::Counts c = online::Get(); c.known)
        it.facts.push_back(F("Players online: {0}", {std::to_string(c.online)}));
      items.push_back(std::move(it));
    }
    if (discord::Available()) {
      Item it = CvarToggle("Discord status",
                           "Shows on your Discord profile that you're playing King Kong, in the launcher or in the "
                           "game, and for how long, while the Discord app is running on this PC. It only talks to "
                           "the Discord app; nothing is sent anywhere else.",
                           "kk_discord_status");
      it.on_choice = [](int i) {
        SetBool("kk_discord_status", i == 1);
        discord::Wake();
      };
      it.on_default = [] {
        Reset("kk_discord_status");
        discord::Wake();
      };
      items.push_back(std::move(it));
    }
    {
      Item it = ActionItem("Change version",
                           "Stable is the tested version of the port. Beta gets test builds of upcoming features "
                           "early, which can have problems. Your saves and settings are kept either way.",
                           Get("kk_update_channel") == "beta" ? "Beta" : "Stable", [this] { OpenChannelPrompt(); });
      if (!switch_to_.empty()) it.value_text = "Checking...";
      if (!switch_to_.empty() || installing_update_) it.on_activate = nullptr;
      items.push_back(std::move(it));
    }
    {
      Item it = ActionItem("Shader pack",
                           "Effects already prepared by playing through the game, so it pauses for new ones less "
                           "often. Each time the launcher opens, it downloads the newest pack from the port's GitHub "
                           "page by itself.",
                           PackStatusText(), [this] {
                             if (!pack_.busy) StartPackDownload();
                           });
      if (pack_.failed) {
        it.value_color = color::kWarn;
        it.facts.push_back(pack_.message);
        it.facts.push_back("Select to try again.");
      } else if (!pack_.busy && !pack_.done) {
        it.facts.push_back("Select to download it now.");
      }
      if (pack_.busy) it.on_activate = nullptr;
      items.push_back(std::move(it));
    }

    items.push_back(Section("Files"));
    items.push_back(ActionItem(files_ok_ ? "Reinstall game" : "Install game",
                               "Copies the game's files again from your disc image. Your saves and settings stay as "
                               "they are.",
                               "", [this] { StartInstall(); }));
    items.push_back(ActionItem("Open save folder", "Your saves, achievements and caches.", "",
                               [this] { OpenInExplorer(paths_.user_dir); }));
    items.push_back(ActionItem("Open save backups",
                               "Copies of your saves, made each time you start playing from the Play menu and before a "
                               "save is deleted or overwritten. The newest 20 are kept.",
                               "", [this] {
                                 std::error_code ec;
                                 std::filesystem::create_directories(paths_.user_dir / "save-backups", ec);
                                 OpenInExplorer(paths_.user_dir / "save-backups");
                               }));
    {
      Item it = ActionItem("Open game folder", "Where the game is installed.", "", [this] { OpenInExplorer(paths_.game_dir); });
      it.facts.push_back(paths_.game_dir.string());
      items.push_back(std::move(it));
    }
    items.push_back(ActionItem("Open settings file", "Every launcher setting, as plain text.", "", [this] {
      SaveSettings(paths_.config_path);
      OpenInExplorer(paths_.config_path);
    }));
    {
      const bool have = Cached(launcher_art_cache_, 2.0, [this] { return art::HasLauncherArt(ArtDir()); });
      Item it = ActionItem("Launcher art",
                           "The launcher's logo and backgrounds come from your copy of the game: the game's own logo, "
                           "and stills from the movie trailer and the intro on the disc.",
                           art_progress_.busy ? "Preparing..." : have ? "From your game files" : "Not taken yet",
                           [this] {
                             if (files_ok_) StartArtExtraction(false);
                           });
      if (art_progress_.failed) it.facts.push_back(art_progress_.message);
      if (files_ok_ && !art_progress_.busy) it.facts.push_back("Select to take it from the game files again.");
      if (!files_ok_ || art_progress_.busy) it.on_activate = nullptr;
      items.push_back(std::move(it));
    }

    items.push_back(Section("Reset"));
    items.push_back(ActionItem("Reset all settings", "Put every setting on every page back to its default.", "",
                               [this] { OpenResetAll(); }));
    return items;
  }

  // ------------------------------------------------------------- reset ---
  // The settings each page shows, for Reset page. "x_*" is a prefix,
  // "@category" a cvar category.
  static std::vector<std::string> PageSettings(Page page) {
    switch (page) {
      case kDisplay:
        return {"fullscreen", "kk_fullscreen_mode", "window_width", "window_height", "monitor", "kk_frame_rate",
                "d3d12_allow_variable_refresh_rate_and_tearing", "present_letterbox",
                "present_hdr", "present_hdr_peak", "present_hdr_paper_white"};
      case kGraphics:
        return {"kk_original_look", "kk_modern_settings", "kk_graphics_preset", "native_backend", "kk_renderer",
                "swap_post_effect", "anisotropic_override",
                "native_texture_pack", "native_texture_pack_folder",
                "present_effect", "kk_render_quality", "resolution_scale", "draw_resolution_scale_x",
                "draw_resolution_scale_y", "native_upscaler", "native_upscaler_quality",
                "kk_motion_blur", "kk_big_blur", "kk_fog", "ao_mode", "ao_strength", "native_ao_method",
                "native_ao_apply", "native_ao_quality", "native_ao_samples", "native_gi", "native_gi_distance", "native_shadow_quality", "native_water_reflections", "native_volumetrics", "native_effect_buffers", "native_sharpness"};
      case kGameplay:
        return {"kk_fov", "kk_camera_shake", "kk_camera_shake_strength", "kk_cheats", "kk_cheat_*",
                "kk_achievement_toasts", "kk_achievement_style",
                "kk_skip_intros", "user_language"};
      case kAudio:
        return {"#profile", "kk_launcher_music", "kk_launcher_music_volume", "kk_launcher_sounds",
                "kk_launcher_sounds_volume", "kk_achievement_sound", "kk_achievement_sound_file", "kk_achievement_volume"};
      case kControls:
        return {"mnk_mode", "kk_button_prompts", "kk_camera_sensitivity", "kk_camera_modern", "kk_mouse_sensitivity",
                "mnk_mouse", "kk_mouse_invert_y", "kk_invert_rs_x", "kk_invert_rs_y", "kk_invert_ls_x", "kk_invert_ls_y",
                "kk_toggle_aim", "kk_deadzone", "kk_vibration", "kk_vibration_strength", "kk_map_*",
                "@Input/Keybinds/Controller"};
      case kAdvanced:
        return {"kk_show_fps", "kk_ps2_cutscenes", "kk_ps2_movie_folder"};
      default:
        return {};
    }
  }

  // Puts one page's settings back to their defaults; the other pages keep theirs.
  void ResetPage(Page page) {
    if (page == kAudio) {
      ResetAudio();
    }
    if (page == kGraphics) startup::ManualChoice();  // resets the renderer and API by hand
    const auto names = PageSettings(page);
    auto on_page = [&](const rex::cvar::FlagEntry& e) {
      for (const auto& n : names) {
        if (n[0] == '@' ? e.category == n.substr(1)
                        : n.back() == '*' ? e.name.rfind(n.substr(0, n.size() - 1), 0) == 0 : e.name == n)
          return true;
      }
      return false;
    };
    for (auto& e : rex::cvar::GetRegistry()) {
      if (e.type == rex::cvar::FlagType::Command || !on_page(e)) continue;
      if (e.source == rex::cvar::Source::kCommandLine || e.source == rex::cvar::Source::kEnvironment) continue;
      rex::cvar::ResetToDefault(e.name);
    }
    ++g_changes;
    // Gameplay's Subtitles is the game's own option, in every save (its default follows the language).
    // Its HUD rows (Aiming visor, Inventory) are the game's own options too.
    if (page == kGameplay) {
      ResetSubtitles();
      ChangeProfileOption(play::ProfileOption::kAimingVisor, 0);
      ChangeProfileOption(play::ProfileOption::kInventory, 0);
    }
    if (page == kGraphics) {
      ChangeProfileOption(play::ProfileOption::kOldMovie, 0);
      ChangeProfileOption(play::ProfileOption::kHorizontalFlip, 0);
      ApplyRendererChoice();  // gpu_plugin follows kk_renderer
    }
    if (page == kDisplay && cb_.set_fullscreen) cb_.set_fullscreen(GetBool("fullscreen"));
    Status(F("{0} settings are back to their defaults.", {T(std::string(kPageNames[page]))}));
  }

  void ResetAllSettings() {
    startup::ManualChoice();
    for (auto& e : rex::cvar::GetRegistry()) {
      if (e.type == rex::cvar::FlagType::Command) continue;
      if (e.source == rex::cvar::Source::kCommandLine || e.source == rex::cvar::Source::kEnvironment) continue;
      rex::cvar::ResetToDefault(e.name);
    }
    ResetAudio();  // the game's own options, in every save
    ResetSubtitles();
    for (int i = int(play::ProfileOption::kOldMovie); i < int(play::ProfileOption::kCount); ++i)
      ChangeProfileOption(play::ProfileOption(i), 0);
    ++g_changes;
    ApplyRendererChoice();
    if (cb_.set_fullscreen) cb_.set_fullscreen(GetBool("fullscreen"));
    Status("Every setting is back to its default.");
  }

  // ------------------------------------------------------------ pop-ups ---
  void OpenModal(std::string title, std::string body, std::vector<std::string> buttons, int cancel,
                 std::function<void(int)> on_button, int focus = 0) {
    ui::Modal m;
    m.open = true;
    m.title = std::move(title);
    m.body = std::move(body);
    m.buttons = std::move(buttons);
    m.cancel = cancel;
    m.focus = focus;
    m.on_button = std::move(on_button);
    modal_ = std::move(m);
  }

  // ---------------------------------------------------------- Benchmark ---
  // Its start pop-up and results page (launcher_benchmark.h), in the launcher's own pop-up.
  void OpenBenchmark() {
    if (!benchmark::RouteAvailable()) {
      OpenModal("Benchmark", "Place a complete benchmark-route.kkbr beside the game before running the benchmark.",
                {"Back"}, 0, {});
      return;
    }
    const bool have = bench_ui::HasResults(paths_.user_dir);
    const auto page = bench_ui::StartPage(have);
    OpenModal(page.title, page.body, page.buttons, page.cancel, [this, have](int i) {
      switch (bench_ui::StartChoiceFor(i, have)) {
        case bench_ui::StartChoice::kStandard: StartBenchmark(bench_ui::kStandardPasses); break;
        case bench_ui::StartChoice::kQuick: StartBenchmark(bench_ui::kQuickPasses); break;
        case bench_ui::StartChoice::kResults: OpenBenchmarkResults(); break;
        default: break;
      }
    });
    modal_.width = page.width;
  }

  void StartBenchmark(int passes) {
    rex::cvar::SetFlagByName("kk_benchmark_passes", std::to_string(passes));  // never saved
    benchmark_pending_ = true;
    StartGame();
  }

  void OpenBenchmarkResults(int focus = 0) {
    const auto result = bench_ui::LoadNewest(paths_.user_dir);
    const auto page = bench_ui::ResultsPage(result);
    REXLOG_INFO("KK launcher: benchmark results page ({}, {} pass rows)",
                result.found ? result.dir.filename().string() : std::string("no results"), result.rows.size());
    OpenModal(page.title, page.body, page.buttons, page.cancel, [this, result](int i) {
      switch (bench_ui::ResultChoiceFor(i)) {
        case bench_ui::ResultChoice::kRunAgain: OpenBenchmark(); break;
        case bench_ui::ResultChoice::kOpenFolder:
          OpenInExplorer(result.dir);
          OpenBenchmarkResults(i);
          break;
        case bench_ui::ResultChoice::kCopy:
          ImGui::SetClipboardText(result.report.c_str());
          Status("Benchmark results copied to the clipboard.");
          OpenBenchmarkResults(i);
          break;
        default: break;
      }
    }, focus);
    modal_.reader = page.reader;
    modal_.width = page.width;
  }

  void OpenQuit() {
    OpenModal("Quit", "Close the launcher without starting the game?", {"Quit", "Cancel"}, 1, [this](int i) {
      if (i == 0) Quit();
    });
  }

  void OpenResetPage() {
    const Page page = page_;
    const std::string name = T(std::string(kPageNames[page]));
    OpenModal(F("Reset {0}", {name}),
              F("Put the {0} settings back to their defaults? Settings on the other pages stay as they are.", {name}),
              {"Reset", "Cancel"}, 1, [this, page](int i) {
                if (i == 0) ResetPage(page);
              }, 1);
  }

  void OpenResetAll() {
    OpenModal("Reset all settings", "Put every setting on every page back to its default?", {"Reset", "Cancel"}, 1,
              [this](int i) {
                if (i == 0) ResetAllSettings();
              }, 1);
  }

  static std::string ChangelogText(const ChangelogEntry& e) {
    return "## v" + e.version + "|" + e.date + "\n" + e.body + "\n";
  }

  void OpenVersionHistory() {
    std::string text;
    for (const auto& e : Changelog()) text += ChangelogText(e);
    OpenModal("Version history", "", {"Close"}, 0, nullptr);
    modal_.reader = text;
    modal_.width = 780;
  }

  // Shown once after an update: the notes of every version since the last one run.
  void OpenWhatsNew() {
    std::string text;
    for (const auto& e : Changelog()) {
      if (CompareVersions(e.version, KK_VERSION) > 0) continue;
      if (!whats_new_from_.empty() && CompareVersions(e.version, whats_new_from_) <= 0) break;
      text += ChangelogText(e);
      if (whats_new_from_.empty()) break;  // only this version when we don't know the last one
    }
    REXLOG_INFO("KK: What's new opened (from v{})", whats_new_from_);
    OpenModal(F("What's new in v{0}", {KK_VERSION}), "", {"Close", "Every version"}, 0, [this](int i) {
      if (i == 1) OpenVersionHistory();
    });
    modal_.reader = text;
    modal_.width = 780;
  }

  // Above 30 FPS some animations are wrong (game logic stepped per frame), so
  // say so before starting and offer 30 or 60 instead.
  void OpenFrameRateWarning() {
    const int fps = GetInt("kk_frame_rate", 30);
    const std::string current = fps <= 0 ? T(std::string("an unlimited frame rate")) : std::to_string(fps) + " FPS";
    std::vector<std::string> buttons = {"Play at 30 FPS"};
    std::vector<int> rates = {30};
    if (fps != 60) {
      buttons.push_back("Play at 60 FPS");
      rates.push_back(60);
      buttons.push_back(fps <= 0 ? std::string("Keep unlimited") : F("Keep {0} FPS", {std::to_string(fps)}));
      rates.push_back(fps);
    } else {
      buttons.push_back("Keep 60 FPS");
      rates.push_back(60);
    }
    buttons.push_back("Back");
    OpenModal("Frame rate above 30 FPS",
              F("You have chosen {0}. King Kong was made to run at 30 FPS, and above that some character animations "
                "can look wrong, such as the crew rowing the boat at the start.\n\nWe recommend 30 FPS. 60 FPS also "
                "works well: the issues are still there, but much less noticeable. This will be fixed in a future "
                "update.",
                {current}),
              buttons, int(buttons.size()) - 1, [this, rates](int i) {
                if (i >= int(rates.size())) return;
                SetInt("kk_frame_rate", rates[size_t(i)]);
                StartGame();
              });
    modal_.width = 640;
  }

  // ------------------------------------------------------------- updates ---
  // The check runs on its own thread with its own status, so pressing Play
  // never waits for the network (the launcher may be gone when it finishes).
  // The shader pack updates itself separately (StartPackDownload).
  void StartUpdateCheck(bool manual, const std::string& switch_to = "") {
    if (update_ && update_->busy) return;
    update_ = std::make_shared<UpdateStatus>();
    update_manual_ = manual;
    update_prompted_ = !switch_to.empty();  // a switch installs without asking again
    switch_to_ = switch_to;
    const std::string channel = switch_to.empty() ? Get("kk_update_channel") : switch_to;
    std::thread([s = update_, channel, switching = !switch_to.empty()] { CheckForUpdate(*s, channel, switching); })
        .detach();
  }

  // Change version (About): stable <-> beta. Only once the player has accepted
  // the warning does it look for that channel's newest version and install it,
  // older or newer; the saves and settings stay as they are.
  void OpenChannelPrompt() {
    if (Get("kk_update_channel") == "beta") {
      OpenModal("Switch back to stable?",
                "This installs the newest stable version of the port and restarts the launcher. If your beta is "
                "newer than it, this goes back a version.\n\nStable has no native renderer, so the beta's own "
                "settings (the renderer, its ambient occlusion method and the fullscreen resolution) go back to "
                "their defaults. Your installed game, saves and other settings stay as they are.",
                {"Switch to stable", "Cancel"}, 1, [this](int i) {
                  if (i == 0) StartChannelSwitch("stable");
                });
    } else {
      OpenModal("Switch to the beta?",
                "Beta versions are test builds of what's coming next. They can have bugs, some things may not work, "
                "and support for them is limited.\n\nChanging version replaces the port's own files with the newest "
                "beta and restarts the launcher. Your installed game, saves and settings stay as they are, and you "
                "can switch back to stable here at any time.\n\nTell us how the beta runs on the Discord.",
                {"I understand, switch to beta", "Join Discord", "Cancel"}, 2,
                [this](int i) {
                  if (i == 0) StartChannelSwitch("beta");
                  if (i == 1) {
                    OpenUrl(kDiscordUrl);
                    OpenChannelPrompt();
                  }
                },
                2);
    }
  }

  void StartChannelSwitch(const std::string& channel) {
    if (update_ && update_->busy) {
      Status("Still checking for updates. Try again in a moment.");
      return;
    }
    Status(channel == "beta" ? "Looking for the newest beta..." : "Looking for the newest stable version...");
    StartUpdateCheck(false, channel);
  }

  // Once a channel switch's check is back: keep the new channel, then install
  // its version (or say there's nothing to install).
  void FinishChannelSwitch() {
    const std::string channel = switch_to_;
    switch_to_.clear();
    if (update_->failed) {
      OpenModal("Couldn't change version", T(update_->message) + "\n\n" + T("Nothing was changed."), {"Close"}, 0,
                nullptr);
      return;
    }
    Set("kk_update_channel", channel);
    // Stable has no native renderer: it must find only settings it knows (ResetBetaOnlySettings).
    if (channel == "stable" && update_->found) ResetBetaOnlySettings();
    SaveSettings(paths_.config_path);
    if (update_->found) {
      BeginUpdateInstall(*update_->found);
    } else if (channel == "beta") {
      OpenModal("Beta", F("There's no beta build right now, so you stay on v{0}. You're on the beta channel now, so "
                          "the launcher offers the next beta as soon as it's out.",
                          {KK_VERSION}),
                {"Close"}, 0, nullptr);
    } else {
      Status(F("You're on stable, and v{0} is the newest stable version.", {KK_VERSION}));
    }
  }

  // The online player count: on makes a new random ID and checks in now; off
  // deletes the ID, so nothing can link a later check-in to this one.
  void SetOnlineCount(bool on) {
    SetBool("kk_online_asked", true);
    if (on && Get("kk_install_id").empty()) Set("kk_install_id", online::NewInstallId());
    if (!on) Set("kk_install_id", "");
    SetBool("kk_online_count", on);
    SaveSettings(paths_.config_path);
    online::Wake();
  }

  // No reminder about the online player count ever opens by itself (owner, 16 Oct): it is explained and
  // switched in the first-time setup, and on the About page, and nowhere else.

  // The update screen shows the download (SetupStage 3).
  void BeginUpdateInstall(const UpdateInfo& info) {
    installing_update_ = true;
    update_->done = update_->failed = false;
    update_->busy = true;
    update_->bytes = update_->total = 0;
    std::thread([s = update_, info] { InstallUpdate(info, *s); }).detach();
  }

  void OpenUpdatePrompt() {
    if (!update_ || !update_->found) return;
    const UpdateInfo info = *update_->found;
    OpenModal("Update available",
              F("Version {0} of the King Kong PC port is out. You have version {1}.\n\nUpdating replaces the port's "
                "own files and restarts the launcher. Your installed game, saves and settings stay as they are.",
                {info.version, KK_VERSION}),
              {"Update now", "What's new", "Later"}, 2, [this, info](int i) {
                if (i == 0) {
                  BeginUpdateInstall(info);
                } else if (i == 1) {
                  OpenUrl(info.page_url);
                  OpenUpdatePrompt();
                }
              });
  }

  void TickUpdate() {
    if (!update_) return;
    if (!switch_to_.empty()) {
      if (!update_->busy && (update_->done || update_->failed)) FinishChannelSwitch();
      return;
    }
    if (update_manual_ && !update_->busy && (update_->done || update_->failed) && !update_->found) {
      Status(update_->failed ? update_->message : F("You have the newest version (v{0}).", {KK_VERSION}));
      update_manual_ = false;
    }
    if (update_->found) online::SetAvailableVersion(update_->found->version);
    if (update_->found && !update_prompted_ && !modal_.open && !installing_update_) {
      OpenUpdatePrompt();
      update_prompted_ = true;
    }
    if (!installing_update_ || update_->busy) return;
    if (update_->failed) {
      installing_update_ = false;
      OpenModal("Update failed", update_->message, {"Close"}, 0, nullptr);
    } else if (update_->done && !relaunched_) {  // start the new version, then close this one
      relaunched_ = true;
      RelaunchSelf(L"");
      if (cb_.quit) cb_.quit();
    }
  }

  // ------------------------------------------------------ add to Steam ---
  // Home offers Add to Steam while Steam is installed and this copy isn't in
  // its library yet (steam_shortcut.h). Steam reads its non-Steam games only
  // when it starts, so an open Steam closes first and opens again afterwards.
  void StartSteamCheck() {
    steam_ = std::make_shared<SteamState>();
    std::thread([s = steam_] {
      if (steam::StartedFromSteam()) return;
      if (const auto where = steam::Locate()) {
        s->in_library = steam::HasShortcut(*where);
        s->available = true;
      }
    }).detach();
  }

  void OpenSteamPrompt() {
    if (!steam_ || steam_->busy) return;
    const bool running = steam::SteamRunning();
    OpenModal("Add to Steam",
              running ? "Adds King Kong to your Steam library as a non-Steam game, with artwork from SteamGridDB, so "
                        "you can start it from Steam, Big Picture or a Steam Deck.\n\nSteam reads its list of games "
                        "only when it starts, so it closes for a moment and opens again afterwards."
                      : "Adds King Kong to your Steam library as a non-Steam game, with artwork from SteamGridDB, so "
                        "you can start it from Steam, Big Picture or a Steam Deck.",
              {running ? "Close Steam and add" : "Add to Steam", "Cancel"}, 1, [this](int i) {
                if (i == 0) StartAddToSteam();
              });
  }

  void StartAddToSteam() {
    auto s = steam_;
    s->busy = true;
    s->done = false;
    s->phase = 0;
    OpenModal("Adding to Steam", "", {}, -1, nullptr);
    modal_.progress = [] { return -1.0f; };
    modal_.live = [s] { return T(kSteamPhases[std::clamp(s->phase.load(), 0, 3)]); };
    std::thread([s] {
      auto finish = [&](steam::Result r) {
        s->result = std::move(r);
        s->busy = false;
        s->done = true;
      };
      const auto where = steam::Locate();
      if (!where) return finish({false, false, 0, "Steam wasn't found on this computer, or nobody has signed in to it yet."});
      const auto art = steam::DownloadArt();
      const bool running = steam::SteamRunning();
      if (running) {
        s->phase = 1;
        steam::CloseSteam();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (steam::SteamRunning()) {
          if (std::chrono::steady_clock::now() > deadline)
            return finish({false, false, 0, "Steam didn't close. Close Steam yourself, then choose Add to Steam again."});
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // Steam's last writes
      }
      s->phase = 2;
      steam::Result r = steam::AddShortcut(*where, art);
      if (running) {
        s->phase = 3;
        steam::OpenSteam();
        s->reopened = true;
      }
      finish(std::move(r));
    }).detach();
  }

  void TickSteam() {
    if (!steam_ || !steam_->done) return;
    steam_->done = false;
    const steam::Result& r = steam_->result;
    if (!r.ok) {
      OpenModal("Add to Steam", r.error, {"Close"}, 0, nullptr);
      return;
    }
    steam_->in_library = true;
    std::string body = r.art == 5   ? T("King Kong is in your Steam library, with its artwork.")
                       : r.art > 0 ? F("King Kong is in your Steam library, with {0} of its 5 artwork pictures; the "
                                       "others didn't download.",
                                       {std::to_string(r.art)})
                                   : T("King Kong is in your Steam library, but its artwork didn't download, so Steam "
                                       "shows a plain tile.");
    if (steam_->reopened) body += std::string("\n\n") + T("Steam is opening again.");
    OpenModal("Added to Steam", body, {"OK"}, 0, nullptr);
  }

  // ------------------------------------------------------- shader pack ---
  std::filesystem::path CacheDir() const {
    const std::string root = Get("cache_root");
    return root.empty() ? paths_.user_dir / "cache" : std::filesystem::path(root);
  }

  // Collects a finished download and reads which pack is installed.
  void RefreshPack() {
    if (!pack_.busy && pack_thread_.joinable()) {
      pack_thread_.join();
      pack_installed_ = -1;
    }
    if (pack_installed_ < 0) pack_installed_ = InstalledShaderPackVersion(CacheDir());
  }

  void StartPackDownload() {
    if (pack_.busy) return;
    if (pack_thread_.joinable()) pack_thread_.join();
    pack_.done = pack_.failed = false;
    pack_.busy = true;
    pack_thread_ = std::thread([this, dir = CacheDir()] { DownloadAndInstallShaderPack(dir, pack_); });
  }

  float PackProgress() const {
    if (!pack_.busy) return -1;
    const float total = float(pack_.total.load());
    return total > 0 ? float(pack_.bytes.load()) / total : -1.0f;
  }

  std::string PackStatusText() const {
    if (pack_.busy) {
      const float p = PackProgress();
      return p >= 0 ? F("Downloading {0}%", {std::to_string(int(p * 100))}) : T(std::string("Checking for a newer pack"));
    }
    if (pack_.failed) return T(std::string("Download failed"));
    if (pack_installed_ > 0) return F("Pack {0}", {std::to_string(pack_installed_)});
    return T(std::string("Not downloaded"));
  }

  // ------------------------------------------------------------ install ---
  void StartInstall() {
    const auto pkg = BrowseForDiscImage();
    if (pkg.empty()) return;
    const uint32_t title = iso::ReadTitleId(pkg);
    if (title == 0) {
      OpenModal("Can't install", "That file is not an Xbox 360 disc image.", {"OK"}, 0, nullptr);
      return;
    }
    if (title != kTitleId) {
      char ids[2][16];
      std::snprintf(ids[0], sizeof(ids[0]), "%08X", title);
      std::snprintf(ids[1], sizeof(ids[1]), "%08X", kTitleId);
      OpenModal("Can't install", F("That disc is title {0}, not King Kong ({1}).", {ids[0], ids[1]}), {"OK"}, 0, nullptr);
      return;
    }
    progress_.cancel = false;
    installing_ = true;
    install_done_ = false;
    install_thread_ = std::thread([this, pkg] {
      install_result_ = iso::Extract(pkg, paths_.game_dir, &progress_);
      install_done_ = true;
    });
  }

  void FinishInstallIfDone() {
    if (!installing_ || !install_done_) return;
    install_thread_.join();
    installing_ = false;
    files_ok_ = GameFilesPresent(paths_.game_dir);
    if (!install_result_.empty() && progress_.cancel) {
      Status("The install was cancelled.");
    } else if (!install_result_.empty()) {
      OpenModal("Install failed", install_result_, {"OK"}, 0, nullptr);
    } else if (!files_ok_) {
      OpenModal("Install failed", "Extraction finished but default.xex is missing.", {"OK"}, 0, nullptr);
    } else {
      ReloadArt();               // the title icon and achievements from the new files
      StartArtExtraction(true);  // then the logo and stills (the setup screen's last step)
    }
  }

  // --------------------------------------------------------------- play ---
  bool CanPlay() const { return files_ok_ && !installing_ && !played_; }

  void Play() {
    if (!CanPlay()) return;
    benchmark_pending_ = false;
    const int fps = GetInt("kk_frame_rate", 30);
    if (fps > 0 && fps <= 60) return StartGame();  // 60: Remastered's rate, no warning each time
    OpenFrameRateWarning();
  }

  void Quit() {
    SaveIfChanged();
    input_.Release();
    if (cb_.quit) cb_.quit();
  }

  void StartGame() {
    if (played_) return;
    // The pack is merged into the shader cache, which the game opens as it
    // starts: a download in progress finishes first.
    if (pack_.busy) {
      waiting_for_pack_ = true;
      OpenModal("Shader pack", "The newest shader pack is still downloading. The game starts as soon as it's done.",
                {"Back"}, 0, [this](int) { waiting_for_pack_ = benchmark_pending_ = false; });
      modal_.progress = [this] { return PackProgress(); };
      return;
    }
    // The native renderer's shaders for the settings the game starts with (they may have changed
    // here): a build in progress, or one these settings need, finishes first (Tick).
    // Checked (and prepared) when the launcher opened, or at setup, for these settings: nothing to do. A
    // job still running finishes first; settings that change the shaders or pipelines check again.
    const bool checked = std::exchange(native_pack_checked_, false);
    if (NativeShaderPack::Wanted() && !checked && native_pack_.OkForCurrentSettings() && !native_pack_.Busy())
      REXLOG_INFO("KK: Play: shaders already checked and prepared for these settings; nothing to do");
    if (NativeShaderPack::Wanted() && !checked && !(native_pack_.OkForCurrentSettings() && !native_pack_.Busy())) {
      if (!native_pack_.Busy()) native_pack_.Start(CacheDir(), paths_.game_dir);
      waiting_for_native_ = true;
      native_wait_since_ = ImGui::GetTime();
      return;
    }
    played_ = true;
    SaveSettings(paths_.config_path);
    bool needs_restart = false;
    for (size_t i = 0; i < std::size(kRestartCvars); ++i) {
      if (Get(kRestartCvars[i]) == restart_baseline_[i]) continue;
      // native_upscaler: only an upscaler whose Vulkan extensions the device was not made with needs a new
      // device (DLSS's or XeSS's; FSR needs none, and "auto", the plugin's own default before the settings
      // are applied, asks for the card's vendor's).
      if (std::string_view(kRestartCvars[i]) == "native_upscaler" &&
          !UpscalerNeedsNewDevice(restart_baseline_[i], Get("native_upscaler")))
        continue;
      needs_restart = true;
    }
    FlushProfileOptions();
    auto action = benchmark_pending_ ? cb_.benchmark : needs_restart ? cb_.restart_and_play : cb_.play;
    // The pause menu for the game about to start (the app makes it too when the launcher is skipped).
    if (!needs_restart && !benchmark_pending_) CreatePauseMenu(imgui_drawer(), immediate_, paths_.config_path, paths_.user_dir, paths_.game_dir);
    input_.Release();  // the game opens the controllers itself
    if (needs_restart || benchmark_pending_) {
      // The game starts as a new process: this window keeps showing the launcher's backdrop (no menus,
      // no input, the music fading) until the game's window has drawn, then this process ends (handoff.h).
      leaving_ = true;
      modal_.open = false;
    } else {
      Close();  // deletes this dialog after the current draw
    }
    if (action) action();
  }

  // --------------------------------------------------------------- tick ---
  void SaveIfChanged() {
    if (saved_changes_ == g_changes) return;
    SaveSettings(paths_.config_path);
    saved_changes_ = seen_changes_ = g_changes;
  }

  // Bookkeeping each frame: downloads, the install, pop-ups that open by
  // themselves, and saving settings a moment after they change.
  void Tick() {
    const double now = ImGui::GetTime();
    FlushProfileOptions(false);
    FinishInstallIfDone();
    FinishArtIfDone();
    RefreshPack();
    TickUpdate();
    TickSteam();
    CaptureKey();
    TickNameEditor();
    if (offer_crash_reports_ && !modal_.open && !InSetup() && !update_shaders_ && !native_pack_.Busy()) {
      offer_crash_reports_ = false;
      if (HasCrashReports(paths_.user_dir)) {
        REXLOG_INFO("KK: offering saved local crash reports in the launcher");
        OpenModal("Saved crash reports",
            "A previous crash saved diagnostics and a minidump locally. Nothing was uploaded. Dumps may contain private process memory; review before sharing.",
            {"Open folder", "Later"}, 1, [this](int i) {
              if (i == 0) OpenInExplorer(paths_.user_dir / "crashes");
            });
      }
    }
    // What's new only once the game is playable (owner, 16 Oct): never during the first-time setup (the
    // game files asked for, installed, the artwork, the shaders, the look), nor while shaders are prepared
    // (that loads every processor core and the launcher answers slowly meanwhile). launcher_policy.h.
    if (open_whats_new_ &&
        ShowWhatsNewNow({open_whats_new_, modal_.open, files_ok_, InSetup(), ShaderScreen(), native_pack_.Busy(),
                         NativeShaderPack::Wanted(), native_pack_.Finished()})) {
      open_whats_new_ = false;
      OpenWhatsNew();
    }
    TickNativeShaders();
    TickUpscalerQuery();
    if (waiting_for_pack_ && !pack_.busy) {
      waiting_for_pack_ = false;
      modal_.open = false;
      StartGame();
    }
    PreviewAudio();
    if (g_changes != seen_changes_) {
      seen_changes_ = g_changes;
      changed_time_ = now;
    }
    if (seen_changes_ != saved_changes_ && now - changed_time_ > 0.8 && !ImGui::IsMouseDown(0)) {
      if (SaveSettings(paths_.config_path)) saved_time_ = now;
      saved_changes_ = seen_changes_;
    }
  }

  // The temporal upscalers this PC runs (asked when the launcher opened): once the answer is in, it is
  // kept in the settings (kk_upscalers_offered) and the default upscaler follows it (SetOfferedUpscalers).
  void TickUpscalerQuery() {
    if (!HasTemporalSetting() || !UsingNativeRenderer()) return;
    const std::string backend = Get("native_backend");
    if (upscaler_answer_taken_ && upscaler_answer_backend_ == backend) return;
    const nr::kk::UpscalerChoices c = nr::kk::QueryUpscalers();
    if (c.state != 2 && c.state != 3) return;  // still asking, or not possible here
    upscaler_answer_taken_ = true;
    upscaler_answer_backend_ = backend;
    std::string offered;
    for (const auto& [on, name] : {std::pair{c.dlss, "dlss"}, std::pair{c.fsr4, "fsr4"}, std::pair{c.fsr3, "fsr3"},
                                   std::pair{c.xess, "xess"}})
      if (on) offered += (offered.empty() ? "" : ",") + std::string(name);
    if (offered.empty()) offered = "none";
    const std::string before = Get("native_upscaler");
    SetOfferedUpscalers(offered);
    if (Get("native_upscaler") != before)
      REXLOG_INFO("KK: default upscaler now {} (was {})", Get("native_upscaler"), before);
    ++g_changes;  // kk_upscalers_offered is saved with the settings
  }

  // The native renderer's shaders: the end of the setup step, and Play waiting for them (the
  // pop-up only when the check takes more than a moment, i.e. there is something to build).
  void TickNativeShaders() {
    if (setup_shaders_ && !native_pack_.Busy()) {
      setup_shaders_ = false;
      if (!setup_style_) Status(InstalledMessage());  // else once the look is chosen
    }
    if (rebuild_shaders_screen_ && !native_pack_.Busy()) {
      rebuild_shaders_screen_ = false;
      Status(native_pack_.Ok() ? "The shaders are ready." : "The shaders were not prepared: the game compiles them as it starts.");
    }
    if (update_shaders_ && !native_pack_.Busy()) {
      update_shaders_ = false;
      Status(native_pack_.Ok() ? F("Updated to v{0}. The shaders are ready.", {KK_VERSION})
                               : T(std::string("The shaders were not prepared: the game compiles them as it starts.")));
    }
    if (!waiting_for_native_) return;
    if (!native_pack_.Busy()) {
      waiting_for_native_ = false;
      if (native_modal_) modal_.open = false;
      native_modal_ = false;
      native_pack_checked_ = true;
      StartGame();
    } else if (!native_modal_ && !modal_.open &&
               (native_pack_.Working() || ImGui::GetTime() - native_wait_since_ > 3.0)) {
      // Only while something is actually prepared (or a check takes unusually long): a quick check at
      // Play shows nothing.
      native_modal_ = true;
      OpenModal("Preparing shaders",
                "The native renderer's shaders are still being prepared from your copy of the game. " KK_SLOW_PC " The game starts "
                "as soon as they're done.",
                {"Back"}, 0, [this](int) { waiting_for_native_ = native_modal_ = benchmark_pending_ = false; });
      modal_.progress = [this] { return native_pack_.DrawnFraction(ImGui::GetTime()); };
    }
  }

#if defined(KK_DEV_TOOLS)
  // Developer-only (screenshots): KK_DEV_LAUNCHER_TOUR=<seconds> shows each
  // page in turn for that long, then the What's new pop-up, logging
  // "KK dev: tour <name>" as each appears (tools/launcher_shots.ps1 captures them).
  void DevTour() {
    static const double each = [] {
      const char* v = std::getenv("KK_DEV_LAUNCHER_TOUR");
      return v && *v ? std::atof(v) : 0.0;
    }();
    if (each <= 0) return;
    static const auto start = std::chrono::steady_clock::now();
    static int shown = -1;
    const int step = int(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / each);
    if (step == shown) return;
    if (shown < 0) open_whats_new_ = false;  // no start-up pop-up over the pages
    shown = step;
    struct Step {
      Page page;
      SubPage sub;
      const char* name;
    };
    static const Step kSteps[] = {
        {kHome, kNoSubPage, "play"},         {kDisplay, kNoSubPage, "display"},
        {kGraphics, kNoSubPage, "graphics"}, {kGameplay, kNoSubPage, "gameplay"},
        {kControls, kNoSubPage, "controls"}, {kControls, kRemapPage, "remap"},
        {kAdvanced, kNoSubPage, "advanced"}, {kAchievements, kNoSubPage, "achievements"},
        {kAbout, kNoSubPage, "about"}};
    constexpr int kCount = int(std::size(kSteps));
    if (step < kCount) {
      page_ = kSteps[step].page;
      sub_ = kSteps[step].sub;
      modal_.open = false;
      REXLOG_INFO("KK dev: tour {}", kSteps[step].name);
    } else if (step == kCount) {
      page_ = kHome;
      whats_new_from_ = "";  // this version's notes, as after an update from an unknown version
      OpenWhatsNew();
      REXLOG_INFO("KK dev: tour whats_new");
    } else if (step == kCount + 1) {
      modal_.open = false;
      REXLOG_INFO("KK dev: tour done");
    }
  }

  // Developer-only: KK_DEV_LAUNCHER_INPUT="pad:down,pad:a,kb:right,shot:name,..."
  // ('|' separates launcher sessions: each bonus started from Extras hands the next
  // session to the launcher that comes back, see dev_launcher_chain.h)
  // ("setup:N" shows a stage of the setup screen) presses buttons (a, b, x, y,
  // lb, rb, start, up, down, left, right, lt, rt)
  // one at a time as a controller ("pad:", "ps:") or the keyboard ("kb:", the
  // default), logging "KK dev: input <step>"; "shot:<name>" logs
  // "KK dev: shot <name>" and holds the page for a capture. The mouse, at 1280 x 720
  // layout positions: "move:X:Y", "click:X:Y", "down:X:Y", "up", "wheel:N".
  // "setup:5" is the look step; "reset-beta" does to the settings what switching to
  // stable does (and saves them); "type:<text>" types text (New game's name); "whats-new" opens What's new as after 1.9.11; "update-test"
  // logs the update choice cases (RunUpdatePickTests); "updated" acts as the first start after an
  // update (the full shader rebuild and its screen); "setup:4" shows the setup's shader screen.
  void DevInput() {
    ImGuiIO& io = ImGui::GetIO();
    // Scripted runs wait for the setup's work (only captures go on); the look step (stage 5) takes input
    // like the pages.
    const bool waiting =
        std::getenv("KK_DEV_LAUNCHER_INPUT_FILE") && (InSetup() || ShaderScreen()) && SetupStage() != 5;
    static bool release = false;
    if (release) {
      io.AddMouseButtonEvent(0, false);
      release = false;
    }
    static std::vector<std::string> steps = [] {
      std::vector<std::string> out;
      const char* v = std::getenv("KK_DEV_LAUNCHER_INPUT");
      // Only the first '|' session: the rest is for the runs that come back to Extras (dev_launcher_chain.h).
      std::stringstream in(std::string(kk::DevLauncherSession(v ? v : "")));
      for (std::string t; std::getline(in, t, ',');)
        if (!t.empty()) out.push_back(t);
      return out;
    }();
    static size_t next = 0;
    // Test commands may be appended inside a private run without focusing the window.
    if (const char* path = std::getenv("KK_DEV_LAUNCHER_INPUT_FILE")) {
      static size_t loaded = 0;
      std::ifstream file(path);
      size_t line = 0;
      for (std::string command; std::getline(file, command); ++line)
        if (line >= loaded && !command.empty()) steps.push_back(command);
      loaded = line;
    }
    static double at = 2.5;
    const double now = ImGui::GetTime();
    if (next >= steps.size() || now < at) return;
    if (waiting && steps[next].rfind("shot:", 0) != 0) return;
    std::string t = steps[next++];
    at = now + 0.45;
    REXLOG_INFO("KK dev: input {}", t);
    if (t.rfind("shot:", 0) == 0) {
      REXLOG_INFO("KK dev: shot {}", t.substr(5));
      rex::FlushLogging();
      at = now + 4.0;
      return;
    }
    // Private Extras tests choose a save, then navigate/activate through normal UI actions.
    if (t.rfind("extras-save:", 0) == 0) {  // Play > Extras with that save
      chapter_slot_ = std::atoi(t.c_str() + 12);
      OpenPlayMenu();
      OpenPlayView(PlayView::kExtras);
      return;
    }
    if (t.rfind("page:", 0) == 0) {
      if (text::Upper(t.substr(5)) == "EXTRAS") {
        OpenPlayMenu();
        OpenPlayView(PlayView::kExtras);
      }
      for (int p = 0; p < kPageCount; ++p)
        if (text::Upper(t.substr(5)) == text::Upper(kPageNames[p])) GoTo(Page(p));
      return;
    }
    if (t.rfind("focus:", 0) == 0) {
      for (size_t i = 0; i < focused_items_.size(); ++i)
        if (focused_items_[i].label == t.substr(6) && focused_items_[i].type != ItemType::kSection) {
        auto& state = play_ != PlayView::kNone ? play_lists_[int(play_)] : lists_[page_ * 4 + sub_];
        state.focus = int(i);
        state.focus_label = focused_items_[i].label;
        REXLOG_INFO("KK dev: launcher focused {} value {} help {}", state.focus_label, focused_items_[i].value_text,
                    text::T(focused_items_[i].description));
        return;
      }
      REXLOG_INFO("KK dev: launcher focus FAIL {}", t.substr(6));
      return;
    }
    if (t == "rows") {
      // With the shown value (a choice's option, else its value text), so a test can read settings back.
      for (const auto& row : focused_items_) {
        const bool choice = row.index >= 0 && row.index < int(row.options.size());
        REXLOG_INFO("KK dev: launcher page {} row {} = {}",
                    play_ != PlayView::kNone ? PlayViewName(play_) : kPageNames[page_], row.label,
                    choice ? row.options[size_t(row.index)] : row.value_text);
      }
      return;
    }
    if (t.rfind("look:", 0) == 0) {  // the setup's look step, when it is up: look:original / look:remastered
      if (SetupStage() == 5) {
        if (t == "look:original") ApplyOriginal();
        else ApplyRemastered();
        REXLOG_INFO("KK: setup: look {}", t == "look:original" ? "Original" : "Remastered");
        setup_style_ = false;
        Status(InstalledMessage());
      }
      return;
    }
    if (t == "layout-check") {  // every settings page against settings_layout.h
      LogLayoutCheck();
      return;
    }
    if (t.rfind("language:", 0) == 0) { SetInt("user_language", std::atoi(t.c_str() + 9)); return; }
    auto at_point = [&](const std::string& xy) {
      const size_t colon = xy.find(':');
      const float x = std::stof(xy.substr(0, colon)), y = std::stof(xy.substr(colon + 1));
      io.AddMousePosEvent(o_.x + x * m_.s, o_.y + y * m_.s);
    };
    if (t.rfind("setup:", 0) == 0) {  // show a setup stage: 0 your copy, 1 install, 2 artwork; -1 back
      dev_setup_ = std::atoi(t.c_str() + 6);
      return;
    }
    if (t == "reset-beta") {  // what switching to stable does to the settings (FinishChannelSwitch)
      ResetBetaOnlySettings();
      SaveSettings(paths_.config_path);
      return;
    }
    if (t == "updated") {  // as on the first start after an update: the full rebuild and its screen
      update_shaders_ = true;
      if (!native_pack_.Busy()) native_pack_.Start(CacheDir(), paths_.game_dir, true);
      return;
    }
    if (t == "update-test") {  // PickUpdate's cases (update.h), logged
      RunUpdatePickTests();
      return;
    }
    if (t == "whats-new") {  // the What's new pop-up as after an update from the version before
      whats_new_from_ = "1.9.11";
      return OpenWhatsNew();
    }
    if (t.rfind("type:", 0) == 0) return io.AddInputCharactersUTF8(t.substr(5).c_str());  // typed text
    if (t.rfind("move:", 0) == 0) return at_point(t.substr(5));
    if (t.rfind("click:", 0) == 0 || t.rfind("down:", 0) == 0) {
      at_point(t.substr(t.find(':') + 1));
      io.AddMouseButtonEvent(0, true);
      release = t[0] == 'c';
      return;
    }
    if (t == "up") return io.AddMouseButtonEvent(0, false);
    if (t.rfind("wheel:", 0) == 0) return io.AddMouseWheelEvent(0, std::stof(t.substr(6)));
    ui::Device d = ui::Device::kKeyboard;
    if (t.rfind("pad:", 0) == 0) d = ui::Device::kXbox, t = t.substr(4);
    else if (t.rfind("ps:", 0) == 0) d = ui::Device::kPlayStation, t = t.substr(3);
    else if (t.rfind("kb:", 0) == 0) t = t.substr(3);
    static const std::map<std::string, Action> kNames = {
        {"up", Action::kUp},          {"down", Action::kDown},       {"left", Action::kLeft},
        {"right", Action::kRight},    {"lt", Action::kPageUp},       {"rt", Action::kPageDown},
        {"a", Action::kAccept},       {"b", Action::kBack},          {"x", Action::kDefault},
        {"y", Action::kResetPage},    {"lb", Action::kPrevTab},      {"rb", Action::kNextTab},
        {"start", Action::kPlay}};
    if (auto it = kNames.find(t); it != kNames.end()) input_.Inject(it->second, d);
  }
#endif

  rex::ui::ImmediateDrawer* immediate_;
  LauncherPaths paths_;
  ui::Metrics m_;
  ImVec2 o_{0, 0};
  float w_ = 1280, h_ = 720;
  Page page_ = kHome;
  bool recovery_shown_ = false;
  std::map<int, ui::ListState> lists_;
  std::vector<Item> focused_items_;
  const Item* focused_ = nullptr;
  Action pending_click_ = Action::kCount;
  int home_focus_ = 0;
  PlayView play_ = PlayView::kNone;      // the Play menu and its pages, over Home
  int play_focus_ = 0;
  std::map<int, ui::ListState> play_lists_;
  std::vector<play::SaveSlot> saves_;    // read as the Play menu opens and after a delete
  int chapter_slot_ = -1;               // the save Chapter select starts with (Load game's Y); -1 the last played
  std::vector<int> row_data_;           // the Play pages' rows: the slot or chapter entry each stands for
  std::string new_name_, name_before_;  // New game's name ("" the game's own)
  bool editing_name_ = false;
  std::string upscaler_answer_backend_;
  bool upscaler_answer_taken_ = false;  // TickUpscalerQuery has the plugin's answer
  bool menu_value_ = false;    // the menu's focused entry has a value (Left and Right change it)
  int side_focus_ = -1;      // the side panel (Home, setup) with the focus, or -1
  bool status_focus_ = false;   // Home: the shaders' status has the focus (Recompile shaders)
  bool menu_to_status_ = false; // Home's menu moves down to it (DrawMenu)
  ImVec2 status_rect_[2] = {};  // where it was drawn last frame
  double side_since_ = -1;   // when the side panels were first drawn (their fade-in)
  std::unique_ptr<rex::ui::ImmediateTexture> discord_icon_, steam_icon_;
  std::vector<float> home_offset_;
  float home_mix_ = 1.0f;
  float tab_x_ = -1, tab_w_ = 0;

  std::vector<std::string> restart_baseline_;
  bool files_ok_ = false;
  bool played_ = false;
  bool leaving_ = false;  // the game starts in a new process; this one closes once it is shown
  int extras_return_focus_ = -1;  // Graphics was opened from this Extras row: B goes back to it
  double last_activity_ = 0;  // NoteActivity: when something last moved (ImGui time)
  int last_view_ = -1;
  bool last_status_focus_ = false;
  std::chrono::steady_clock::time_point next_frame_{};
  bool benchmark_pending_ = false;
  bool open_benchmark_results_ = false;  // --kk_launcher_open=benchmark, once the launcher draws
  std::shared_ptr<UpdateStatus> update_;
  std::shared_ptr<SteamState> steam_;
  bool update_manual_ = false, update_prompted_ = false, installing_update_ = false, relaunched_ = false;
  std::string switch_to_;  // the channel Change version is switching to, while it checks
  bool open_whats_new_ = false;
  bool offer_crash_reports_ = true;
  std::string whats_new_from_;  // version the player had before this one
  ShaderPackStatus pack_;
  std::thread pack_thread_;
  int pack_installed_ = -1;        // -1: not read yet
  bool waiting_for_pack_ = false;  // Play was pressed during the download
  NativeShaderPack native_pack_;      // the native renderer's shaders (native_shaders.h)
  bool setup_shaders_ = false;        // being prepared as the install's last step
  bool update_shaders_ = false;       // built again in full after an update (DrawShaderScreen)
  bool rebuild_shaders_screen_ = false;  // Graphics > Rebuild shaders: the shader screen until it's done
  Page settings_page_ = kDisplay;     // the settings category Home's Settings opens (the last one shown)
  std::vector<std::string> tab_names_;  // the settings tabs, upper case, in tab_names_lang_
  int tab_names_lang_ = -1;
  bool waiting_for_native_ = false;   // Play was pressed: they are checked (and built) first
  bool native_modal_ = false;         // the pop-up of that wait is open
  bool native_pack_checked_ = false;  // checked for this Play: StartGame goes on
  double native_wait_since_ = 0;
  int autoplay_frames_ = 0;
  double saved_time_ = -100;
  int seen_changes_ = 0, saved_changes_ = 0;
  double changed_time_ = 0;

  std::vector<std::unique_ptr<rex::ui::ImmediateTexture>> textures_;
  struct Slide {
    rex::ui::ImmediateTexture* sharp;
    rex::ui::ImmediateTexture* soft;
    float aspect;
  };
  std::vector<Slide> slides_;
  double slide_clock_ = 0;
  rex::ui::ImmediateTexture* logo_ = nullptr;
  rex::ui::ImmediateTexture* glow_ = nullptr;
  float logo_aspect_ = 1.6f;
  art::ArtProgress art_progress_;
  std::thread art_thread_;
  bool setup_art_ = false;  // the art is being taken as the install's last step
  bool setup_style_ = false;  // the setup's last step: Original or Remastered (DrawStyleCards)
  int style_focus_ = 1;       // the card with the focus (Remastered first)
  rex::ui::ImmediateTexture* still_low_ = nullptr;  // the first still, smaller (the Original card)
  int setup_focus_ = 0;
#if defined(KK_DEV_TOOLS)
  int dev_setup_ = -1;  // KK_DEV_LAUNCHER_SETUP: show a setup stage (screenshots)
#endif
  rex::ui::ImmediateTexture* title_icon_ = nullptr;
  // The chapter select's pictures by file (nullptr: none or unreadable), made when first shown.
  std::map<std::filesystem::path, rex::ui::ImmediateTexture*> chapter_pictures_;
  std::map<std::filesystem::path, float> chapter_aspects_;
  std::map<uint32_t, std::string> extras_texts_;
  std::string extras_lang_;
  std::vector<Achievement> achievements_;
  bool have_achievement_names_ = false;
  std::unique_ptr<AchievementToast> toast_;
  std::unique_ptr<LauncherMusic> music_;  // once the game is installed
  std::unique_ptr<LauncherSounds> ui_sounds_;
  size_t test_index_ = 0;

  iso::Progress progress_;
  std::thread install_thread_;
  std::atomic<bool> install_done_{false};
  bool installing_ = false;
  std::string install_result_;
};

// The pause menu's Settings (ingame_settings.h): the launcher's pages, live rows only.
class InGameSettingsImpl final : public InGameSettings, private SettingsPages {
 public:
  InGameSettingsImpl(rex::ui::ImmediateDrawer* immediate, std::filesystem::path config_path,
                     const std::filesystem::path& user_dir)
      : SettingsPages(immediate, LauncherCallbacks{}, /*in_game=*/true, user_dir), config_path_(std::move(config_path)) {
    glyphs_.AddSet("xbox360", art::LauncherArtDir(user_dir) / "glyphs" / "xbox360");
    seen_changes_ = saved_changes_ = g_changes;
  }
  ~InGameSettingsImpl() override {
    FlushProfileOptions();
    SaveNow();
  }

  std::vector<Item> Items(Page page) override {
    std::vector<Item> items;
    switch (page) {
      case kDisplay: items = DisplayItems(); break;
      case kGraphics: items = GraphicsItems(); break;
      case kGameplay: items = GameplayItems(); break;
      case kAudio: items = AudioItems(); break;
      case kControls:
        items = sub_ == kRemapPage ? RemapItems() : sub_ == kKeysPage ? KeyItems() : ControlsItems();
        break;
      case kAdvanced: items = AdvancedItems(); break;
      default: break;
    }
    items = LiveOnly(std::move(items));
#if defined(KK_DEV_TOOLS)
    if (sub_ == kNoSubPage) DevCheckLayout(int(page), items, "pause");
#endif
    return items;
  }
  int SubPage() const override { return int(sub_); }
  bool CanResetPage(Page page) const override { return page == kAudio; }
  void ResetPage(Page page) override {
    if (page == kAudio) ResetAudio();
  }
  void LeaveSubPage() override { sub_ = kNoSubPage; }
  void Tick() override {
    CaptureKey();
    PreviewAudio();
    FlushProfileOptions(false);
    const double now = ImGui::GetTime();
    if (g_changes != seen_changes_) {
      seen_changes_ = g_changes;
      changed_time_ = now;
    }
    if (seen_changes_ != saved_changes_ && now - changed_time_ > 0.8 && !ImGui::IsMouseDown(0)) SaveNow();
  }
  void SaveNow() override {
    FlushProfileOptions();  // audio changes also belong to a quick press followed by Resume
    if (saved_changes_ == g_changes) return;
    SaveSettings(config_path_);
    saved_changes_ = seen_changes_ = g_changes;
  }
  bool Capturing() const override { return !capturing_.empty(); }
  std::string StatusLine() const override {
    return !status_.empty() && ImGui::GetTime() - status_time_ < 6.0 ? status_ : std::string();
  }
  ui::Input& input() override { return input_; }
  ui::Glyphs& glyphs() override { return glyphs_; }
  ui::Modal& modal() override { return modal_; }
  std::string PromptSet() const override { return SettingsPages::PromptSet(); }

 private:
  std::filesystem::path config_path_;
  int seen_changes_ = 0, saved_changes_ = 0;
  double changed_time_ = 0;
};

}  // namespace

const char* InGameSettings::PageName(Page page) {
  static const char* const kNames[kPageCount] = {"Display", "Graphics", "Audio", "Controls", "Gameplay", "Advanced"};
  return page >= 0 && page < kPageCount ? kNames[page] : "";
}

std::unique_ptr<InGameSettings> CreateInGameSettings(rex::ui::ImmediateDrawer* immediate,
                                                     std::filesystem::path config_path,
                                                     std::filesystem::path user_dir) {
  return std::make_unique<InGameSettingsImpl>(immediate, std::move(config_path), user_dir);
}

bool GameFilesPresent(const std::filesystem::path& game_dir) {
  std::error_code ec;
  return !game_dir.empty() && std::filesystem::exists(game_dir / "default.xex", ec) &&
         std::filesystem::exists(game_dir / "KKMaps.bf", ec) && std::filesystem::exists(game_dir / "KKTextures.bf", ec);
}

namespace {

// Loads rexgpu-<name> (any build config) from beside the program; true if one loaded.
bool PreloadGpuPluginNamed(const char* name) {
  const auto dir = rex::filesystem::GetExecutableFolder();
  for (const char* postfix : {"rd", "", "d"}) {
#if defined(_WIN32)
    const std::string file = std::string("rexgpu-") + name + postfix + ".dll";
    if (std::filesystem::exists(dir / file) && LoadLibraryW((dir / file).c_str())) return true;
#else
    const std::string file = std::string("librexgpu-") + name + postfix + ".so";
    for (const auto& path : {dir / file, dir / ".." / "lib" / file})
      if (std::filesystem::exists(path) && dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL)) return true;
#endif
  }
  return false;
}

}  // namespace

namespace {
bool g_native_renderer = false;  // PreloadGpuPlugin loaded rexgpu-native
}  // namespace

void PreloadGpuPlugin() {
  g_native_renderer = PreloadGpuPluginNamed("native");
  if (!g_native_renderer) REXLOG_ERROR("KK: native graphics plugin could not be loaded");
}

bool NativeRendererAvailable() { return g_native_renderer; }

std::string ApplyRendererChoice() {
  NormalizeRendererSettings();
  SetCvarDefault("ao_strength", "3");
  ApplyUpscalerDefaults();
  return startup::ApplyChoice(rex::cvar::GetFlagByName("gpu_plugin"));
}

void ShowLauncher(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, LauncherPaths paths,
                  LauncherCallbacks callbacks) {
  new Launcher(drawer, immediate, std::move(paths), std::move(callbacks));
}

}  // namespace kk
