#include "launcher.h"
#include "handoff.h"
#include "startup_recovery.h"
#include "ingame_settings.h"
#include "pause_menu.h"
#include "photo_mode.h"
#include "play_session.h"
#include "benchmark.h"
#include "launcher_benchmark.h"
#include "launcher_art.h"
#include "setup_compare.h"
#include "workshop_art.h"
#include "launcher_music.h"
#include "launcher_sounds.h"
#include "launcher_text.h"
#include "launcher_ui.h"
#include "social_panel.h"
#include "input_diag.h"
#include "coop_runtime.h"
#include "coop_resume.h"
#include "coop_handover.h"
#include "easter_egg.h"
#include "easter_egg_logic.h"
#include "countdown.h"
#include "menu_scene.h"
#include "loading_loop.h"
#include "startup_movies.h"
#include "startup_movies_logic.h"
#include "setup_again.h"
#include "setup_progress.h"
#include "hardware_recommendation.h"
#include "notify_logic.h"
#include "system_notify.h"
#include "ps2_movies.h"
#include "shader_pack.h"
#include "native_shaders.h"
#include "display_modes.h"
#include "steam_shortcut.h"
#include "update.h"
#include "update_channel.h"
#include "glyphs.h"
#include "key_glyphs.h"
#include "discord_presence.h"
#include "online.h"
#include "diagnostics.h"
#include "exit_evidence.h"
#include "exit_evidence_logic.h"
#include "dev_launcher_chain.h"
#include "hooks/kk_native_ui.h"
#include "backend/startup_steps.h"  // the temporal upscalers this PC runs
#include "debug_overlay_logic.h"  // the upscalers the running game offers (as F8 lists them)
#include "ultrawide_logic.h"  // the Ultrawide mod's picture shape (task 311)

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
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
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>

#include "art.h"
#include "changelog.h"
#include "content.h"
#include "cheat_rules.h"
#include "cheats.h"
#include "platform.h"
#include "settings.h"
#include "settings_help.h"
#include "settings_layout.h"
#include "launcher_policy.h"
#include "graphics_presets.h"
#include "iso.h"
#include "toast.h"
#include "workshop.h"
#include "workshop_net.h"
#include "artbook.h"
#include "artbook_files.h"
#include "artbook_logic.h"

REXCVAR_DEFINE_STRING(kk_launcher_open, "", "KK",
                      "Internal: the screen the launcher opens on, set by the game when it goes back to the "
                      "launcher: play (the Play menu) or chapters (Chapter select). Never saved.");
REXCVAR_DEFINE_BOOL(kk_launcher_saved, false, "KK",
                    "Internal (task 331): set by the game when a story run quit from the pause menu and the save was "
                    "written; the Returning to the main menu loop then says so. Never saved.");

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
// Import ZIP / "I already have a texture pack": the file the OS dialog returned (its callback can run on another thread).
struct TexturePackZipChoice {
  std::mutex mutex;
  bool busy = false;
  std::string selected;
  kk::content::ImportTarget target = kk::content::ImportTarget::kTexturePack;  // task 209: which import button
} texture_pack_zip_choice;
constexpr const char* kProjectUrl = "https://github.com/TekRantGaming/king-kong-recompiled";
constexpr const char* kDiscordUrl = "https://discord.gg/8eakYSvBZS";  // the owner's invite (23 Oct)
#include "discord_icon.inc"
#include "steam_icon.inc"
constexpr const char* kOnlineAbout =
    "Shows how many players are online, at the top right of the launcher. Help reduce stutter: share graphics "
    "pipeline data. When you quit, the game sends its list of the graphics pipelines it needed, so later "
    "versions can prepare them in advance. It also sends an anonymous performance summary when you quit (frame "
    "rate per chapter, graphics card, driver and main graphics settings) and, after a crash, the crash report "
    "with your user name and personal folders removed. To count you, the launcher sends a tiny anonymous "
    "check in every 5 minutes: a random number that isn't linked to you, the port's version (stable or beta), "
    "Windows or Linux, and whether you're in the game. No name, account, IP address or personal files are "
    "kept, and switching it off stops all of this and deletes the random number.";
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
// most others) or kdialog (KDE). Without the AppImage's library path, as the PS2 movie folder picker
// (system programs load their own libraries). What it gave is logged: players' reports need it.
std::filesystem::path BrowseForDiscImage() {
  const char* commands[] = {
      "env -u LD_LIBRARY_PATH zenity --file-selection --title='Select your King Kong Xbox 360 disc image' "
      "--file-filter='Xbox 360 disc image (*.iso) | *.iso *.ISO' --file-filter='All files | *' 2>/dev/null",
      "env -u LD_LIBRARY_PATH kdialog --title 'Select your King Kong Xbox 360 disc image' --getopenfilename "
      "\"$HOME\" '*.iso *.ISO|Xbox 360 disc image' 2>/dev/null",
  };
  for (const char* cmd : commands) {
    FILE* pipe = popen(cmd, "r");
    if (!pipe) continue;
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    const int status = pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    REXLOG_INFO("KK: disc picker {}: exit {}, gave '{}'", std::strstr(cmd, "zenity") ? "zenity" : "kdialog", code,
                out);
    if (!out.empty()) return std::filesystem::path(out);
    // 127: the tool isn't installed, so try the next one; anything else means
    // the player cancelled.
    if (code != 127) return {};
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
constexpr const char* kRestartCvars[] = {"present_effect", "window_width", "window_height", "kk_picture_size", "monitor", "gpu_plugin",
                                         "native_upscaler", "native_backend",
                                         "d3d12_allow_variable_refresh_rate_and_tearing"};

// The launcher's pages (owner, 16 Oct: a tidy top level). Home is the main menu: Play (its own pages,
// PlayView, with Extras among them), Settings, Workshop, Achievements and Benchmark. Settings has the seven
// categories of settings_layout.h as tabs, then About as its last tab (owner, 16 Oct evening: Display, Graphics,
// Audio, Controls, Gameplay, Cheats, Advanced, About). About is only a Settings tab: Home's About entry became
// Benchmark (owner, 18 Oct). Achievements and the Workshop are pages of their own.
enum Page { kHome, kDisplay, kGraphics, kAudio, kControls, kGameplay, kCheats, kAdvanced, kAbout, kAchievements,
            kWorkshop, kPageCount };
constexpr const char* kPageNames[kPageCount] = {"Home",     "Display", "Graphics", "Audio", "Controls",    "Gameplay",
                                               "Cheats",   "Advanced", "About",   "Achievements", "Workshop"};
// The pages with settings rows (settings_layout.h, same order).
constexpr Page kFirstSettingsPage = kDisplay, kLastSettingsPage = kAdvanced;
constexpr bool IsSettingsPage(Page page) { return page >= kFirstSettingsPage && page <= kLastSettingsPage; }
static_assert(kLastSettingsPage - kFirstSettingsPage + 1 == settings_layout::kPageCount);
// Settings' tabs: those pages, then About.
constexpr Page kLastSettingsTab = kAbout;
constexpr int kSettingsTabCount = kLastSettingsTab - kFirstSettingsPage + 1;
static_assert(kSettingsTabCount == 8);
// Sub-pages of Controls.
enum SubPage { kNoSubPage, kRemapPage, kKeysPage };
// Play on Home: the Play menu and its pages (docs/menus.md).
// kCoop (C81): the Play Online Co-op pages (launcher_coop_pages.inc), only with co-op enabled.
enum class PlayView { kNone, kMenu, kLoad, kNew, kChapters, kExtras, kArtbook, kManual, kCoop };

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
#if !defined(_WIN32)
  // Linux builds have no temporal upscaler (native-renderer/docs/linux.md, "Upscalers on Linux"): the
  // renderer answers "none here" (0) or "failed" (3) at once. That is the final answer, not "still asking",
  // so the setup's cards and Display > Upscaling must not say "Checking" for ever.
  o.known = o.known || c.state == 0 || c.state == 3;
#endif
  o.fsr3 = c.fsr3;
  o.fsr4 = c.fsr4 && Fsr4AllowedOnThisGpu();  // Radeon RX 9000 cards only
  o.dlss = c.dlss;
  o.xess = c.xess;
  return o;
}

// While the game runs (the pause menu): the upscalers the game's own device runs, as the graphics panel (F8)
// lists them (debug_overlay_logic.h): never a new question, which would make a second device in the game.
OfferedUpscalers OfferedWhilePlaying() {
  OfferedUpscalers o;
  if (!HasTemporalSetting() || !UsingNativeRenderer()) return o;
  const NrEffectStats s = nr::kk::EffectTimes();
  const nr::kk::UpscalerChoices c = nr::kk::PeekUpscalers();
#if defined(_WIN32)
  constexpr bool windows = true;
#else
  constexpr bool windows = false;
#endif
  o.known = true;
  for (int i : panel::OfferedUpscalers(s.size ? s.api : 0, s.size ? s.vendor : 0,
                                       {c.state == 2, c.dlss, c.fsr4 && Fsr4AllowedOnThisGpu(), c.fsr3, c.xess}, windows)) {
    const std::string_view v = panel::kUpscalers[i].value;
    o.dlss = o.dlss || v == "dlss", o.fsr4 = o.fsr4 || v == "fsr4", o.fsr3 = o.fsr3 || v == "fsr3";
    o.xess = o.xess || v == "xess";
  }
  return o;
}

// ------------------------------------------------------------ presets ---
// Graphics presets set the upscaler and every quality setting of the Graphics page (graphics_presets.h: render
// quality, anti-aliasing, sharpness, texture filtering, ambient occlusion, GI, shadows, water reflections, light
// shafts, effect resolution); motion blur, screen blur and fog are left to taste.
// The preset shown is the one the settings match; anything else is Custom.

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
// Remastered: the Modern look (named Remastered on the Graphics page), the hardware recommendation's preset for
// this graphics card (with that preset's global illumination), the native renderer's GTAO at Strong and 60 FPS.
// What every new install starts with (StartFullSetup, job J6) and what "Use recommended for my PC" on graphics
// page 4 gives. Keeps the native renderer. native-renderer/docs/beta.md.
void ApplyRemastered(bool first_setup = false) {
  SetOriginalLook(false);
  const auto output = PrimaryScreenSize();
  // First setup keeps the tier already chosen before the launcher opened.
  // Later explicit Remastered actions continue to detect as before.
  ApplyHardwareRecommendation(!first_setup, output.first, output.second);
  SetInt("ao_mode", 1);
  SetInt("ao_strength", 3);  // Strong: GTAO's default since Beta 3 (the owner's choice)
  SetInt("native_ao_normal_smoothing", 2);  // High (owner 17 Oct): smooths the faceted AO on low-poly models
  SetInt("kk_frame_rate", 60);  // Remastered plays at 60 FPS (the owner's choice); Original keeps 30
  ++g_changes;
  ApplyRendererChoice();
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
                      e.name == "kk_fullscreen_mode" || e.name == "kk_picture_size" || e.name == "gpu_plugin";
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
  std::atomic<bool> checked{false};     // the check below has finished
  std::atomic<bool> from_steam{false};  // this copy was started from Steam (it is in the library)
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
        in_game_(in_game), profile_user_dir_(std::move(user_dir)) {
    if (PadPromptsFirst()) input_.StartOnPad();
  }

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

  // Graphics > Extras: the game's bonus horizontal flip. Every bonus is unlocked (owner, 18 Oct), so no save
  // is read to decide. The game's Old movie filter is not offered (owner, 18 Oct, task 213: filters belong to
  // photo mode only; the game never applies it, play_session.cpp ApplyPendingVideoModes).
  void OriginalVideoItems(std::vector<Item>& items) {
    const auto profile = ShownProfileOptions();
    items.push_back(Section("Extras"));
    items.push_back(ProfileToggle("Horizontal flip", "An original bonus video mode, unlocked in Extras.",
                                  play::ProfileOption::kHorizontalFlip, profile.horizontal_flip));
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
                                 "kk_launcher_music"));  // task 330: with the live scene, the scene's own sound
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
    items.push_back(CvarToggle("Sound", "The sound that plays with each popup.", "kk_achievement_sound"));
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
          "The built in chime or a sound you added. Put .wav files in the sounds folder to see them here.";
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
  // Owner, 17 Oct: shadows, water reflections and light shafts are shown on the setup's graphics pages (and so
  // in Settings > Graphics too, where the setup says they can be changed), still off by default.
  static void LightingItems(std::vector<Item>& items, bool original) {
    if (!NativeShaderPack::Wanted()) return;
    {
      Item it = CvarChoice("Shadows",
                           "How sharp and detailed the game's shadows are.",
                           "native_shadow_quality", {{"Original", "original"}, {"High", "high"}, {"Ultra", "ultra"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarChoice("Water reflections",
                           "How the water reflects its surroundings.",
                           "native_water_reflections", {{"Original", "original"}, {"Screen space", "ssr"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarChoice("Light shafts",
                           "Sunlight shines through the mist and fog in visible shafts.",
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
                         "How many pictures the game draws a second. Your screen's refresh rate is separate.",
                         "kk_frame_rate", opts);
    return std::move(Disable(it, GetBool("kk_original_look"), kSetByOriginal));
  }

  // Ultrawide (task 311; built in since task 360, Beta 17): the shape of the picture the game draws for
  // out_w x out_h (16:9 on a 16:9 or narrower screen, so every size below is as before). In game: the shape the
  // running game draws (UltrawideFactor, set at the start). In the main menu: the shape the next start gives.
  double PictureAspect(int out_w, int out_h) const {
    if (in_game_) return ultrawide::kGameAspect * double(UltrawideFactor());
    const double aspect = PictureAspectFor(out_w, out_h);
    return ultrawide::WidthFactor(aspect) == 1.0 ? ultrawide::kGameAspect : aspect;
  }

  // Display > Upscaling (Beta 2: from the Graphics page): the upscaler and how many pixels the game
  // draws for the screen, which the Resolution above decides.
  void UpscalingItems(std::vector<Item>& items) {
    const auto [out_w, out_h] = cb_.output_size ? cb_.output_size() : std::pair<int, int>{1280, 720};
    const double aspect = PictureAspect(out_w, out_h);  // 16:9 unless the Ultrawide mod is on
    const bool original = GetBool("kk_original_look");
    const char* draws = ChosenPictureSize()    ? "Draws {0} for a {1} picture."  // task 363: a chosen ultrawide size
                        : GetBool("fullscreen") ? "Draws {0} for your {1} screen."
                                                : "Draws {0} for your {1} window.";
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
      const OfferedUpscalers offered = in_game_ ? OfferedWhilePlaying() : OfferedTemporalUpscalers();
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
      // In game: the renderer swaps temporal upscalers (and Off) while it runs; FSR 1 and NIS are the
      // presenter's, chosen at the start, with a render quality below Native that is too.
      const bool spatial_now = !temporal.empty() ? false
                                                 : Get("present_effect") != "bilinear" || Get("kk_render_quality") != "native";
      if (in_game_ && !spatial_now)
        std::erase_if(choices, [](const Choice& c) { return c.present != "bilinear"; });
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Upscaler";
      // Translated sentence by sentence (task 222 game check: the whole text had no translation). In game the
      // pause menu leaves out what does not apply there (FSR 1 / NIS are not listed, nothing restarts) so the
      // strip fits at 1280 x 720.
      std::string desc = T("Scales the picture up to your screen and sharpens it, for a clearer image when the game "
                           "draws fewer pixels than your screen has.");
      if (!in_game_ || spatial_now) desc += std::string(" ") + T(std::string(which));
      desc += std::string(" ") + T("Off uses plain smooth scaling.");
      if (any_temporal || !temporal.empty()) {
        desc += std::string(" ") + T("DLSS, FSR 4, FSR 3.1 and XeSS rebuild a sharp picture from several frames and smooth jagged "
                        "edges too; only those your graphics card supports are listed.");
        if (!in_game_)
          desc += std::string(" ") + T("The default is the best one it runs: DLSS on NVIDIA cards, FSR 4 or FSR 3.1 on AMD cards, "
                          "XeSS on Intel cards, FSR 3.1 on others, and FSR 1 where none of them runs. Choosing one of "
                          "them restarts the game when you press Play.");
      }
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
      if (in_game_ && !spatial_now)
        it.facts.push_back("While you play, the change is made at once. FSR 1 and NIS are chosen from the main menu.");
      if (in_game_ && spatial_now) Disable(it, true, kInLauncherOnly);
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
      // and rebuilds the output size (the chosen resolution's 16:9 picture, never above it: settings.h).
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Upscaler mode";
      it.description = "How many pixels the game draws before the upscaler rebuilds the picture at your screen's size.";
      const std::string cur = Get("native_upscaler_quality");
      it.index = -1;
      const auto [ow, oh] = UpscalerOutputFor(out_w, out_h, aspect);
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
      it.description = "How many pixels the game draws before the upscaler scales them to your screen.";
      it.help_key = "Upscaler mode (spatial)";
      it.options.assign(std::begin(kUpscalerModeNames), std::end(kUpscalerModeNames));
      const int mode = UpscalerMode();
      it.index = mode;
      if (mode >= 0) {
        const int scale = RenderScaleFor(kUpscalerModes[mode], out_h);
        const auto [fw, fh] = NoUpscalerFrameSize(out_w, out_h, 1280 * scale, 720 * scale, aspect);
        it.facts.push_back(F(draws, {Size(fw, fh), Size(out_w, out_h)}));
        std::string same;
        for (int m = 0; m < 4; ++m) {
          const int ms = RenderScaleFor(kUpscalerModes[m], out_h);
          if (m == mode || NoUpscalerFrameSize(out_w, out_h, 1280 * ms, 720 * ms, aspect) != std::pair{fw, fh}) continue;
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
      Disable(it, in_game_, kInLauncherOnly);  // FSR 1 / NIS draw size: set at the start
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    } else {
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Render quality";
      it.description = "How sharply the game is drawn compared with your screen.";
      const std::string cur = Get("kk_render_quality");
      std::vector<std::string> values;
      it.index = -1;
      for (const auto& p : RenderPresets()) {
        const int scale = RenderScaleFor(p.id, out_h);
        if (cur == p.id) {
          it.index = int(values.size());
          const auto [fw, fh] = NoUpscalerFrameSize(out_w, out_h, 1280 * scale, 720 * scale, aspect);
          it.facts.push_back(F(draws, {Size(fw, fh), Size(out_w, out_h)}));
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
      {
        // Never above the chosen resolution without an upscaler (settings.h, NoUpscalerFrameSize).
        const int s = std::clamp(std::atoi(Get("resolution_scale").c_str()), 1, 8);
        const auto [fw, fh] = NoUpscalerFrameSize(out_w, out_h, 1280 * s, 720 * s, aspect);
        it.facts.push_back(F(draws, {Size(fw, fh), Size(out_w, out_h)}));
      }
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
  }

  std::vector<Item> DisplayItems() {
    // The order of settings_layout.h (kDisplayRows).
    const bool original = GetBool("kk_original_look");
    std::vector<Item> items;
    items.push_back(Section("Screen"));
    items.push_back(WindowModeItem());
    items.push_back(MonitorItem());
    // Each mode's own size: the window's, or the screen's in exclusive fullscreen (Borderless has none).
    if (!GetBool("fullscreen")) items.push_back(WindowSizeItem());
    if (ExclusiveFullscreen()) {
      items.push_back(ResolutionItem());
      items.push_back(RefreshRateItem());
    } else if (GetBool("fullscreen")) {
      items.push_back(BorderlessResolutionItem());  // task 363
    }
    {
      // A screen or window wider than 16:9 (task 311, built in since task 360): the picture is already wider (its
      // shape, up to 32:9), so the row says so instead of "Letterbox 16:9"; the setting still decides what a
      // screen wider still shows.
      const auto [out_w, out_h] = cb_.output_size ? cb_.output_size() : std::pair<int, int>{1280, 720};
      const double aspect = PictureAspect(out_w, out_h);
      if (aspect == ultrawide::kGameAspect) {
        items.push_back(CvarChoice("Aspect ratio",
                                   "What the game's 16:9 picture does on a screen of another shape.",
                                   "present_letterbox", {{"Letterbox 16:9", "true"}, {"Stretch", "false"}}));
      } else {
        Item it = CvarChoice("Aspect ratio",
                             "What the game's wider picture does on a screen wider still.",
                             "present_letterbox", {{"Letterbox", "true"}, {"Stretch", "false"}});
        const auto [pw, ph] = ultrawide::LargestInside(out_w, out_h, aspect);
        it.facts.push_back(F("Ultrawide: the game fills your wide screen with a {0} picture and shows more at the sides.",
                             {Size(pw, ph)}));
        items.push_back(std::move(it));
      }
    }
    PictureItems(items);
    items.push_back(Section("Frame rate"));
    items.push_back(CvarToggle("VSync",
                               "Makes each frame wait for your screen's next refresh.",
                               "d3d12_allow_variable_refresh_rate_and_tearing", "Off", "On", /*invert=*/true));
    items.push_back(FrameRateItem());
    // HDR (owner, 21 Oct, Beta 14): experimental, off by default, only in the F8 graphics panel
    // (debug_overlay.cpp); no longer a row here, in the setup or in the pause menu.
    // Camera (owner, 18 Oct, after task 222): Field of view, Motion blur and Camera shake moved here from Gameplay
    // and Graphics; Performance metrics went to Gameplay. The pause menu's Display page follows.
    items.push_back(Section("Camera"));
    {
      Item it = CvarSlider("Field of view",
                           "How wide the camera sees. 80" KK_DEG " is recommended and 69" KK_DEG " is the original for "
                           "Jack; Kong, cutscene and other cameras widen by the same amount. Jack's gun keeps its usual "
                           "size.",
                           "kk_fov", 69, 110, 1, [](float v) {
                             // Task 365 (owner, 23 Oct): 80 is the default and marked recommended, 69 the original.
                             const int d = int(std::lround(v));
                             return d <= 69   ? F("{0}" KK_DEG " (original)", {std::to_string(d)})
                                    : d == 80 ? F("{0}" KK_DEG " (recommended)", {std::to_string(d)})
                                              : std::to_string(d) + KK_DEG;
                           });
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarToggle("Motion blur",
                           "The trail the game blends over fast moments, mostly in Kong's sequences and some "
                           "transitions.",
                           "kk_motion_blur");
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
    return items;
  }

  // Display > Picture (owner, 18 Oct; GitHub issue #44): brightness and gamma of the game's picture, applied
  // by the renderer as it shows each frame (native-renderer/backend/picture_adjust.h), so they change at once
  // in the launcher and in the pause menu. Only the game's picture changes: the launcher, the menus and the
  // overlays stay as they are. The defaults are the game's own picture (with HDR, white stays at Paper white).
  void PictureItems(std::vector<Item>& items) {
    items.push_back(Section("Picture"));
    {
      Item it = CvarSlider("Brightness",
                           "Raises or lowers the darkest parts of the game's picture. Higher shows more in dark "
                           "places, lower gives deeper blacks. White stays the same. The menus do not "
                           "change. 0 is the game's own picture.",
                           "kk_brightness", -50, 50, 1, [](float v) {
                             const int n = int(std::lround(v));
                             return n > 0 ? "+" + std::to_string(n) : std::to_string(n);
                           });
      items.push_back(std::move(it));
    }
    {
      Item it = CvarSlider("Gamma",
                           "Brightens or darkens the middle tones of the game's picture while black and white stay "
                           "the same. Above 1.00 is brighter, below is darker. 1.00 is the game's own picture.",
                           "kk_gamma", 0.5f, 1.5f, 0.05f, [](float v) {
                             char b[16];
                             std::snprintf(b, sizeof(b), "%.2f", std::round(v * 20.0f) / 20.0f);
                             return std::string(b);
                           });
      // Saved as 0.05 steps ("1.05"), not the slider's float text.
      it.on_value = [](float v) {
        char b[16];
        std::snprintf(b, sizeof(b), "%.2f", std::clamp(std::round(v * 20.0f) / 20.0f, 0.5f, 1.5f));
        Set("kk_gamma", b);
      };
      items.push_back(std::move(it));
    }
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
    const auto picture = ChosenPictureSize();
    Item it;
    it.type = ItemType::kChoice;
    it.label = "Window size";
    it.description = "The size of the game's window.";
    it.index = -1;
    it.value_text = picture ? Size(picture->first, picture->second) : Size(w, h);
    it.default_text = "Default";
    std::vector<std::pair<int, int>> sizes;
    std::vector<bool> wide;  // an ultrawide picture size (task 363), not a window size
    for (auto [sw, sh] : kSizes) {
      if (sw >= screen_w || sh >= screen_h) continue;  // must fit with its frame
      if (!picture && close_to(sw, w) && close_to(sh, h)) it.index = int(sizes.size());
      it.options.push_back(sw == 0 ? std::string("Default") : Size(sw, sh));
      sizes.emplace_back(sw, sh);
      wide.push_back(false);
    }
    // Task 363: the ultrawide sizes too, even wider than the screen: the window then takes the largest size of that
    // shape that fits (FitWindow) and the game still draws the chosen size.
    WideOptions(screen_h, picture, it, sizes, wide);
    it.on_choice = [sizes, wide, scale, fit_w = screen_w, fit_h = screen_h](int i) {
      auto [sw, sh] = sizes[size_t(i)];
      if (wide[size_t(i)]) {
        Set("kk_picture_size", std::to_string(sw) + "x" + std::to_string(sh));
        const auto window = ultrawide::FitWindow(sw, sh, fit_w, fit_h);
        sw = window.first;
        sh = window.second;
      } else {
        Reset("kk_picture_size");
      }
      if (sw == 0) {
        Reset("window_width");
        Reset("window_height");
        return;
      }
      SetInt("window_width", int(sw / scale + 0.5));
      SetInt("window_height", int(sh / scale + 0.5));
    };
    it.on_default = [] {
      Reset("kk_picture_size");
      Reset("window_width");
      Reset("window_height");
    };
    if (picture)
      it.facts.push_back(F("The game draws a {0} picture and fits it into your {1} window.",
                           {Size(picture->first, picture->second), Size(w, h)}));
    return it;
  }

  // Task 363 (Beta 17): the ultrawide sizes (ultrawide_logic.h kWideSizes) a resolution list offers for a screen
  // screen_h tall, as "2560 x 1080  .  21:9", after the list's own; the chosen one (kk_picture_size) is always there
  // and selected. Skips sizes the list already has as its own (a real ultrawide monitor's modes).
  static void WideOptions(int screen_h, const std::optional<std::pair<int, int>>& picture, Item& it,
                          std::vector<std::pair<int, int>>& sizes, std::vector<bool>& wide) {
    const size_t own = sizes.size();
    auto listed = [&](int w, int h) {
      for (size_t i = 0; i < own; ++i)
        if (sizes[i] == std::pair<int, int>{w, h}) return true;
      return false;
    };
    auto add = [&](int w, int h, const char* shape) {
      if (picture && *picture == std::pair<int, int>{w, h}) it.index = int(sizes.size());
      it.options.push_back(Size(w, h) + "  " KK_DOT "  " + shape);
      sizes.emplace_back(w, h);
      wide.push_back(true);
    };
    bool chosen_added = false;
    for (const auto& s : ultrawide::kWideSizes) {
      const bool chosen = picture && *picture == std::pair<int, int>{s.w, s.h};
      if (!chosen && (!ultrawide::WideSizeOffered(s.h, screen_h) || listed(s.w, s.h))) continue;
      add(s.w, s.h, s.shape);
      chosen_added = chosen_added || chosen;
    }
    if (picture && !chosen_added)  // from the settings file, not one of the list's
      add(picture->first, picture->second, double(picture->first) / picture->second > 3.0 ? "32:9" : "21:9");
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
    it.description = "How the game fills your screen: in a window, borderless or fullscreen.";
    it.options = {"Windowed", "Borderless", "Fullscreen"};
    it.index = !GetBool("fullscreen") ? 0 : ExclusiveFullscreen() ? 2 : 1;
    // Windowed, except in Game Mode (gamescope, ApplyPortDefaults): the screen's own size, as Borderless.
    const auto* mode_flag = FindFlag("fullscreen");
    it.default_text = mode_flag && mode_flag->default_value == "true" ? "Borderless" : "Windowed";
    it.on_choice = [this](int i) {
      if (i == 1) Set("kk_fullscreen_mode", "desktop");
      if (i == 2 && !ParseDisplayMode(Get("kk_fullscreen_mode"))) Set("kk_fullscreen_mode", DesktopModeValue());
      // Task 363: an ultrawide picture size stays chosen; Windowed gets a window of its shape that fits.
      if (const auto picture = ChosenPictureSize(); i == 0 && picture) {
        const double scale = cb_.dpi_scale ? cb_.dpi_scale() : 1.0;
        const auto [screen_w, screen_h] =
            cb_.screen_size ? cb_.screen_size() : std::pair<int, int>{picture->first * 2, picture->second * 2};
        const auto [ww, wh] = ultrawide::FitWindow(picture->first, picture->second, screen_w, screen_h);
        SetInt("window_width", int(ww / scale + 0.5));
        SetInt("window_height", int(wh / scale + 0.5));
      }
      SetFullscreen(i != 0);
    };
    it.on_default = [this] {
      Reset("kk_picture_size");
      Reset("kk_fullscreen_mode");
      Reset("fullscreen");
      if (cb_.set_fullscreen) cb_.set_fullscreen(GetBool("fullscreen"));
    };
    if (it.index == 2 && !in_game_)
      it.facts.push_back("The launcher stays borderless; the screen changes when the game starts.");
    return it;
  }

  // Borderless only (task 363): the desktop's own size, or an ultrawide picture size, drawn at that size and fitted
  // into the screen (black bars above and below on a 16:9 screen; the Aspect ratio row can stretch it instead).
  Item BorderlessResolutionItem() {
    const auto picture = ChosenPictureSize();
    const auto desktop = ParseDisplayMode(DesktopModeValue());
    const auto [screen_w, screen_h] = desktop ? std::pair<int, int>{desktop->width, desktop->height}
                                      : cb_.screen_size ? cb_.screen_size()
                                                        : std::pair<int, int>{1920, 1080};
    Item it;
    it.type = ItemType::kChoice;
    it.label = "Resolution";
    it.description = "The size of the game's picture: your desktop's, or a wider one shown with black bars above and "
                     "below.";
    it.options.push_back("Desktop");
    it.index = 0;
    it.default_text = "Desktop";
    std::vector<std::pair<int, int>> sizes = {{0, 0}};
    std::vector<bool> wide = {false};
    WideOptions(screen_h, picture, it, sizes, wide);
    it.value_text = picture ? Size(picture->first, picture->second) : Size(screen_w, screen_h);
    it.on_choice = [sizes](int i) {
      const auto [w, h] = sizes[size_t(i)];
      if (w == 0) Reset("kk_picture_size");
      else Set("kk_picture_size", std::to_string(w) + "x" + std::to_string(h));
    };
    it.on_default = [] { Reset("kk_picture_size"); };
    it.facts.push_back(picture ? F("The game draws a {0} picture and fits it into your {1} screen.",
                                   {Size(picture->first, picture->second), Size(screen_w, screen_h)})
                               : F("The game will be shown at {0}.", {Size(screen_w, screen_h)}));
    return it;
  }

  // Fullscreen only: the screen's size while the game runs.
  Item ResolutionItem() {
    LoadDisplayModes();
    const auto cur = ParseDisplayMode(Get("kk_fullscreen_mode"));
    const auto picture = ChosenPictureSize();
    Item it;
    it.type = ItemType::kChoice;
    it.label = "Resolution";
    it.description = "The size your screen switches to while the game runs.";
    std::vector<std::pair<int, int>> sizes;
    for (const DisplayMode& m : display_modes_) {
      if (!sizes.empty() && sizes.back() == std::pair<int, int>{m.width, m.height}) continue;
      sizes.emplace_back(m.width, m.height);
      it.options.push_back(Size(m.width, m.height));
    }
    it.index = -1;
    if (!picture)
      for (size_t i = 0; i < sizes.size(); ++i)
        if (sizes[i] == std::pair<int, int>{cur->width, cur->height}) it.index = int(i);
    if (it.index < 0 && !picture) {  // a size this display doesn't have (another monitor's, or the settings file's)
      it.index = int(sizes.size());
      sizes.emplace_back(cur->width, cur->height);
      it.options.push_back(F("{0} (not on this display)", {Size(cur->width, cur->height)}));
    }
    // Task 363: the ultrawide sizes this display has no mode for: the screen stays at the desktop's mode and the game
    // draws the chosen size, fitted into it.
    std::vector<bool> wide(sizes.size(), false);
    const auto desktop = ParseDisplayMode(DesktopModeValue());
    WideOptions(desktop ? desktop->height : cur->height, picture, it, sizes, wide);
    it.facts.push_back(picture ? F("The game draws a {0} picture and fits it into your {1} screen.",
                                   {Size(picture->first, picture->second), Size(cur->width, cur->height)})
                               : F("The game will be shown at {0}.", {Size(cur->width, cur->height)}));
    if (desktop) it.default_text = Size(desktop->width, desktop->height);
    it.on_choice = [this, sizes, wide, cur](int i) {
      if (wide[size_t(i)]) {
        const auto [w, h] = sizes[size_t(i)];
        Set("kk_picture_size", std::to_string(w) + "x" + std::to_string(h));
        Set("kk_fullscreen_mode", DesktopModeValue());
        return;
      }
      Reset("kk_picture_size");
      const auto [w, h] = sizes[size_t(i)];
      const std::vector<float> rates = RatesAt(w, h);
      // The rate chosen before when this size has it, else this size's highest.
      float rate = rates.empty() ? (cur ? cur->refresh : 0.0f) : rates.front();
      for (const float r : rates)
        if (cur && std::fabs(r - cur->refresh) < 0.01f) rate = r;
      Set("kk_fullscreen_mode", DisplayMode{w, h, rate}.Value());
    };
    it.on_default = [this] {
      Reset("kk_picture_size");
      Set("kk_fullscreen_mode", DesktopModeValue());
    };
    return it;
  }

  // Fullscreen only: the display's rates at the Resolution's size.
  Item RefreshRateItem() {
    const auto cur = ParseDisplayMode(Get("kk_fullscreen_mode"));
    Item it;
    it.type = ItemType::kChoice;
    it.label = "Refresh rate";
    it.description = "How many times a second your screen refreshes while the game runs.";
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
    return CvarChoice("Monitor", "Which display the game opens on.", "monitor",
                      std::move(opts));
  }

  // ----------------------------------------------------------- Graphics ---
  std::vector<Item> GraphicsItems() {
    const bool original = GetBool("kk_original_look");
    std::vector<Item> items;
    items.push_back(Section("Preset"));
    {
      Item it = CvarChoice("Look",
                           "Original keeps the Xbox 360 look. Remastered unlocks the graphics settings below.",
                           "kk_original_look", {{"Original Xbox 360", "true"}, {"Remastered", "false"}});
      it.on_choice = [](int i) { SetOriginalLook(i == 0); };
      it.on_default = [] { SetOriginalLook(false); };
      items.push_back(std::move(it));
    }
    {
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Preset";
      it.description = "Sets every quality setting on this page at once. Changing one of them shows Custom.";
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

    {
      Item it;
      it.label = "Recommended";
      it.description = "Detect your hardware and recommend render resolution, upscaling, ambient occlusion and global illumination. "
                       "Settings you changed are kept. The frame rate cap, graphics API and other lighting effects are kept.";
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
      Item it = CvarChoice("Antialiasing", "Smooths jagged edges after the frame is drawn.", "swap_post_effect",
                           {{"Off", "none"}, {"FXAA", "fxaa"}, {"SMAA", "smaa"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarChoice("Sharpness", "Makes fine detail crisper.",
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
                           "Soft shading where surfaces meet, in corners and under rocks and grass.",
                           "ao_mode", {{"Off", "0"}, {"On", "1"}});
      if (!native) it.help_key = "Ambient occlusion (legacy)";
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
      if (GetInt("ao_mode", 0) != 0) {
        // GTAO: the shading's darkness (XeGTAO's final power 1.6, 2.2, 3.0) and how far it reaches from
        // each crease (6, 8, 12 near-plane distances). See docs/ao.md, "In the game".
        Item st = CvarChoice("AO strength",
                             native ? "How dark the shading gets and how far it spreads from each corner."
                                    : "How dark the shading gets. Light is recommended.",
                             "ao_strength",
                             native ? std::vector<Option>{{"Light", "1"}, {"Medium", "2"}, {"Strong (recommended)", "3"}}
                                    : std::vector<Option>{{"Light (recommended)", "1"}, {"Medium", "2"}, {"Strong", "3"}});
        items.push_back(std::move(Disable(st, original, kSetByOriginal)));
        if (native) {
          Item quality = CvarChoice("AO quality",
              "How many samples the shading uses.",
              "native_ao_samples", {{"Low", "1"}, {"Medium", "2"}, {"High", "3"}});
          items.push_back(std::move(Disable(quality, original, kSetByOriginal)));
        }
      }
    }
    if (FindFlag("native_gi") && rex::cvar::GetFlagByName("gpu_plugin") == "native") {
      // Global illumination lives in the native renderer (native_gi, native-renderer/docs/gi.md). A build
      // without it has no such setting and ignores the line in the settings file.
      Item it = CvarChoice("Global illumination",
                           "Sunlight bounces off surfaces and softly lights the shade beside them.",
                           "native_gi", {{"Off", "off"}, {"Low", "low"}, {"Medium", "medium"}, {"High", "high"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
      Item distance = CvarChoice("GI distance",
          "How far from the camera bounced light is worked out.",
          "native_gi_distance", {{"Near", "near"}, {"Medium", "medium"}, {"Far", "far"}});  // Full: F8 only (owner, Beta 8)
      items.push_back(std::move(Disable(distance, original || Get("native_gi") == "off",
                                       original ? kSetByOriginal : "Switch Global illumination on to set its distance.")));
    }
    LightingItems(items, original);

    items.push_back(Section("Effects"));  // Motion blur is on Display > Camera (owner, 18 Oct)
    {
      Item it = CvarToggle("Screen blur",
                           "Some levels (Necropolis, Brontosaurus) blur the whole picture slightly.",
                           "kk_big_blur");
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarToggle("Distance fog",
                           "The haze over far away scenery.",
                           "kk_fog");
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }

    OriginalVideoItems(items);
    TexturePackItems(items);
    items.push_back(Section("Advanced"));
    {
      Item it = CvarChoice("Effect resolution", "Resolution of bloom, blur and glow buffers.",
                           "native_effect_buffers", {{"Original", "false"}, {"Output", "true"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    RebuildShadersItem(items);
    return items;
  }

  // ------------------------------------------------- content downloads ---
  // Settings rows for the HD texture pack and the PS2 cutscenes (codex/state/setup-flow.md "Settings", job
  // J2). The service (content.h) runs on its own thread; the rows show its state each frame. What a row
  // starts (the "This can take a while" pop-up, the progress pop-up, the confirmations) is the launcher's
  // (content_action_); the pause menu shows the On/Off switches only.
  enum ContentVerb { kContentDownload, kContentWatch, kContentImport, kContentRemove, kContentCancel };
  std::function<void(content::Kind, int)> content_action_;
  void ContentAction(content::Kind kind, int verb) {
    if (content_action_) content_action_(kind, verb);
  }

  // Graphics > Texture pack. Task 209 (owner, 18 Oct): only one texture pack is on at a time, so the launcher's row
  // is a choice of the packs installed (the original textures first), with the same rule and the same settings as
  // the Workshop's texture pack row (texture_packs_logic.h); downloads, imports and removing are in the Workshop.
  // In the pause menu: the switch only (the renderer reads it every frame, so in game it changes at once).
  std::function<std::vector<std::pair<std::string, std::string>>()> texture_pack_choices_;
  std::function<std::string()> texture_pack_active_;
  std::function<void(const std::string&)> texture_pack_enable_;
  std::function<void()> open_workshop_;
  bool in_setup_pages_ = false;  // task 332: the rows are built for a first-time setup page (launcher_setup_pages.inc)
  static void ChooseTexturePackFolder() {
    {
      std::lock_guard lock(texture_pack_folder_choice.mutex);
      if (texture_pack_folder_choice.busy) return;
      texture_pack_folder_choice.busy = true;
    }
    const std::string current = Get("kk_texture_pack_own_folder");
    SDL_ShowOpenFolderDialog([](void*, const char* const* files, int) {
      std::lock_guard lock(texture_pack_folder_choice.mutex);
      if (files && files[0]) texture_pack_folder_choice.selected = files[0];
      texture_pack_folder_choice.busy = false;
    }, nullptr, nullptr, current.empty() ? nullptr : current.c_str(), false);
  }
  void TexturePackItems(std::vector<Item>& items) {
    items.push_back(Section("Texture pack"));
    if (in_game_ || !texture_pack_choices_) {
      const bool have = !Get("native_texture_pack_folder").empty();
      Item it = CvarToggle("Texture pack",
                           "Replaces the game's textures with the sharper ones from the installed texture pack. You "
                           "can switch it at any time, also from the pause menu, and it changes straight away.",
                           "native_texture_pack");
      items.push_back(std::move(Disable(it, !have && !GetBool("native_texture_pack"),
                                        "No texture pack is on. Choose one in the Workshop from the main menu.")));
      return;
    }
    {
      const auto choices = texture_pack_choices_();
      const std::string active = texture_pack_active_();
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Texture pack";
      it.description = "Which texture pack the game uses. Only one is on at a time: choosing one switches the other "
                       "off. Original textures are the game's own. You can switch at any time, also from the pause "
                       "menu, and it changes straight away.";
      std::vector<std::string> keys;
      for (const auto& [key, name] : choices) {
        keys.push_back(key);
        it.options.push_back(name);
        if (key == active) it.index = int(keys.size()) - 1;
      }
      if (it.index < 0) it.index = 0;
      it.on_choice = [this, keys](int i) {
        if (i >= 0 && i < int(keys.size()) && texture_pack_enable_) texture_pack_enable_(keys[size_t(i)]);
      };
      items.push_back(std::move(it));
    }
    {
      Item it = ActionItem("Texture packs in the Workshop",
                           "Download the HD and UHD texture packs, import your own from a ZIP file, see what each "
                           "one is and remove the ones you no longer want.",
                           "Open Workshop", [this] { if (open_workshop_) open_workshop_(); });
      // Task 332: the first-time setup shows this page; the Workshop opens from the main menu once it is done.
      items.push_back(std::move(Disable(it, in_setup_pages_, "The Workshop opens from the main menu once the setup is done.")));
    }
    {
      const std::string folder = Get("kk_texture_pack_own_folder");
      Item it = ActionItem("Texture pack folder",
                           "A texture pack in a folder of your own (with its pack.txt). Choosing one switches it on.",
                           folder.empty() ? T("Choose folder") : folder, [] { ChooseTexturePackFolder(); });
      { std::lock_guard lock(texture_pack_folder_choice.mutex); it.disabled = texture_pack_folder_choice.busy; }
      items.push_back(std::move(it));
    }
  }

  static std::string Gb(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", double(bytes) / 1e9);
    return buf;
  }
  static std::string ProblemText(content::Problem problem) {
    using content::Problem;
    switch (problem) {
      case Problem::kNotZip: return T("It is not a ZIP file.");
      case Problem::kUnsafeName: return T("It has file names that are not safe to unpack.");
      case Problem::kNoManifest: return T("It has no pack.txt.");
      case Problem::kManyManifests: return T("It has more than one pack.txt.");
      case Problem::kSubfolders: return T("The pack folder has folders inside it.");
      case Problem::kTooLarge: return T("A file in it is too large.");
      case Problem::kBadManifest: return T("Its pack.txt does not start with KKTP 1, or a line in it is broken.");
      case Problem::kMissingFile: return T("A file listed in its pack.txt is missing.");
      case Problem::kNoMovies: return T("It has no cutscene files.");
      // Task 209: the Workshop's imports (MODDING.md's folder structure).
      case Problem::kNoModToml: return T("It has no mod.toml. A mod is one folder with its mod.toml.");
      case Problem::kManyMods: return T("It has more than one mod.toml. Put one mod in each ZIP file.");
      case Problem::kIsTexturePack: return T("It is a texture pack, not a mod. Use Import texture pack.");
      case Problem::kIsMod: return T("It is a mod, not a texture pack. Use Import mod.");
      case Problem::kNoModTextures: return T("It is a texture pack mod without textures/pack.txt.");
      case Problem::kBadModType: return T("Its mod.toml has no type the game knows (texture-pack, settings or cutscenes).");
      case Problem::kNone: break;
    }
    return {};
  }
  static std::string ContentError(const content::Status& st, bool tex) {
    using content::Error;
    switch (st.error) {
      case Error::kConnection: return T("The connection was lost. Select to continue the download.");
      case Error::kServer: return T("The download server did not answer as expected. Try again later.");
      case Error::kSpace:
        return F("Not enough free space. It needs {0} GB and the drive has {1} GB free.",
                 {Gb(st.space_needed), Gb(st.space_free)});
      case Error::kChecksum: return T("The download did not pass its check and was deleted. Select to download it again.");
      case Error::kBadPack:
        return std::string(tex ? T("This is not a texture pack the game can use.") : T("The download is not usable.")) +
               " " + ProblemText(st.problem);
      case Error::kUnpack: return T("The files could not be unpacked. Select to try again.");
      case Error::kWrite: return T("Your user folder could not be written to. Check that the drive is not full.");
      case Error::kBusy: return T("Another download is running. Try again when it has finished.");
      case Error::kNone: break;
    }
    return {};
  }
  static std::string TimeLeft(double seconds) {
    if (seconds < 0) return {};
    if (seconds < 60) return T("Less than a minute left");
    return F("About {0} min left", {std::to_string(int(seconds / 60 + 0.5))});
  }

  static std::string Percent(const content::Status& st) {
    return st.total ? std::to_string(int(double(st.done) * 100 / double(st.total))) + "%" : std::string();
  }
  static void ProgressFacts(const content::Status& st, std::vector<std::string>& facts) {
    if (st.total) facts.push_back(F("{0} of {1} GB", {Gb(st.done), Gb(st.total)}));
    if (st.stage == content::Stage::kDownloading && st.speed > 0) {
      char speed[32];
      std::snprintf(speed, sizeof(speed), "%.1f", st.speed / 1e6);
      facts.push_back(F("{0} MB/s", {speed}));
    }
    if (st.stage == content::Stage::kDownloading)
      if (const std::string left = TimeLeft(st.seconds_left); !left.empty()) facts.push_back(left);
  }

  // Download HD texture pack / Download PS2 cutscenes: Download asks first ("This can take a while"); while
  // it runs (or is paused) the row opens the progress pop-up (Pause / Continue, Run in background, Cancel).
  Item ContentDownloadItem(content::Kind kind, const content::Status& st) {
    using namespace content;
    const bool tex = kind == Kind::kTexturePack;
    const Info& info = InfoFor(kind);
    Item it = ActionItem(tex ? "Download HD texture pack" : "Download PS2 cutscenes", "", "", nullptr);
    int verb = -1;
    switch (st.stage) {
      case Stage::kNone:
        it.value_text = "Download";
        it.description = tex ? F("Downloads the HD texture pack ({0} GB) and turns it on. It needs {1} GB of free space while it installs.",
                                 {Gb(info.zip_size), Gb(SpaceNeeded(info.zip_size, 0, info.unpacked))})
                             : F("Downloads the upscaled PS2 cutscenes that the Xbox 360 version left out ({0} GB) and turns them on. It needs {1} GB of free space while it installs.",
                                 {Gb(info.zip_size), Gb(SpaceNeeded(info.zip_size, 0, info.unpacked))});
        verb = kContentDownload;
        break;
      case Stage::kQueued:
      case Stage::kStarting:
      case Stage::kDownloading:
        it.value_text = st.stage == Stage::kDownloading ? Percent(st) : st.stage == Stage::kQueued ? "Waiting" : "Starting";
        it.description = "Downloading. Select to see the progress, pause or cancel. It continues where it stopped, also after closing the launcher.";
        ProgressFacts(st, it.facts);
        verb = kContentWatch;
        break;
      case Stage::kPaused:
        it.value_text = "Paused";
        it.description = "Paused. Select to continue or cancel the download.";
        ProgressFacts(st, it.facts);
        verb = kContentWatch;
        break;
      case Stage::kVerifying:
        it.value_text = Percent(st);
        it.description = "Checking the download.";
        verb = kContentWatch;
        break;
      case Stage::kUnpacking:
        it.value_text = Percent(st);
        it.description = "Unpacking the files. This takes a few minutes.";
        verb = kContentWatch;
        break;
      case Stage::kInstalled:
        it.value_text = "Installed";
        it.description = tex ? F("Installed: {0}. Turn it on or off with Texture pack above.", {st.installed_name})
                             : T("Installed. Turn them on or off with PS2 cutscenes above.");
        break;
      case Stage::kFailed:
        it.value_text = st.installed ? "Installed" : "Try again";
        it.description = ContentError(st, tex);
        if (!st.installed) verb = kContentDownload;
        break;
      case Stage::kRemoving:
        it.value_text = "Removing";
        it.description = "Removing.";
        break;
    }
    if (verb >= 0) it.on_activate = [this, kind, verb] { ContentAction(kind, verb); };
    return it;
  }

  // Remove texture pack / Remove PS2 cutscenes (asks first), or Cancel download while one is kept.
  Item ContentRemoveItem(content::Kind kind, const content::Status& st) {
    using namespace content;
    const bool tex = kind == Kind::kTexturePack;
    const bool partial = st.Busy() || st.stage == Stage::kPaused || (st.stage == Stage::kFailed && !st.installed);
    Item rem = ActionItem(tex ? "Remove texture pack" : "Remove PS2 cutscenes",
                          partial ? "Stops the download and deletes what was downloaded so far."
                          : tex   ? "Deletes the installed texture pack and turns Texture pack off."
                                  : "Deletes the restored cutscenes and turns PS2 cutscenes off.",
                          partial ? "Cancel download" : "Remove", [this, kind, partial] {
        ContentAction(kind, partial ? kContentCancel : kContentRemove);
      });
    if (st.stage == Stage::kUnpacking || st.stage == Stage::kRemoving || (!partial && !st.installed)) {
      rem.disabled = true;
      rem.disabled_note = st.installed || st.Busy() ? "Wait until it has finished." : "Not installed.";
    }
    return rem;
  }

  // Graphics > Renderer: the native renderer's graphics API (Vulkan or DirectX 12).
  static void GraphicsApiItem(std::vector<Item>& items) {
    if (!NativeRendererAvailable()) return;
    if (UsingNativeRenderer() && FindFlag("native_backend")) {
      items.push_back(Section("Renderer"));
      Item api;
      api.type = ItemType::kChoice;
      api.label = "Graphics API";
      const std::string backend = Get("native_backend");
#if defined(__linux__)
      api.description = "How the renderer talks to your graphics card.";
      api.options = {"Auto", "Vulkan"};
      api.index = backend == "auto" ? 0 : backend == "vulkan" ? 1 : -1;
      api.default_text = "Auto";
#else
      // Task 350a (owner, 21 Oct): DirectX 12 is the recommended API and the default on Windows; Vulkan is the
      // other choice and the fallback when DirectX 12 cannot start. "auto" (the renderer's own, Vulkan) is not
      // offered any more: a value of it from the command line shows as Vulkan, which it runs.
      api.description = "How the renderer talks to your graphics card.";
      // Task 368: on Intel integrated graphics Vulkan is the default and the recommended one.
      const bool intel = IntelIntegratedGraphics();
      api.options = intel ? std::vector<std::string>{"DirectX 12", "Vulkan (recommended)"}
                          : std::vector<std::string>{"DirectX 12 (recommended)", "Vulkan"};
      api.index = backend == "d3d12" ? 0 : backend == "vulkan" || backend == "auto" ? 1 : -1;
      api.default_text = intel ? "Vulkan (recommended)" : "DirectX 12 (recommended)";
#endif
      api.value_text = backend;
      api.on_choice = [](int i) {
        startup::ManualChoice();  // the player's choice wins over a startup fallback
#if defined(__linux__)
        Set("native_backend", i == 0 ? "auto" : "vulkan");
#else
        Set("native_backend", i == 0 ? "d3d12" : "vulkan");
#endif
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
    if (!in_game_) {
      items.push_back(Section("Downloads"));
      items.push_back(CvarToggle("Notify me when downloads finish",
                                 "Shows a system notification when the HD texture pack or the PS2 cutscenes finish "
                                 "downloading, or stop, while the launcher is minimised or behind other windows. The "
                                 "sound that plays with it follows Menu sounds.",
                                 "kk_notify_downloads"));
    }
    // The restored PS2 cutscenes' rows, where this build can play them (Windows: Media Foundation; Linux: the
    // bundled FFmpeg, task 187).
    if (movies::PlaybackAvailable()) {
      items.push_back(Section("Cutscenes"));
      items.push_back(CvarToggle("PS2 cutscenes",
          in_game_ ? "Plays the restored PS2 cutscenes in the game, between the missions where the Xbox 360 version has "
                     "none. Download them from the main menu's Settings > Advanced. You can switch them at any time."
                   : "Plays the restored PS2 cutscenes in the game, between the missions where the Xbox 360 version has "
                     "none. Download them below, or choose a folder with your own BK_xxxx.mp4 files. You can switch "
                     "them at any time.",
          "kk_ps2_cutscenes"));
      if (!in_game_) {
        auto& service = content::Service::Get();
        const content::Status st = service.Ready() ? service.Get(content::Kind::kCutscenes) : content::Status{};
        items.push_back(ContentDownloadItem(content::Kind::kCutscenes, st));
        items.push_back(ContentRemoveItem(content::Kind::kCutscenes, st));
        const auto folder = movies::Folder().u8string();
        Item it = ActionItem("PS2 movie folder", "Choose the folder containing BK_xxxx.mp4.", "", [this] {
          const auto path = movies::BrowseFolder();
          input_.Settle(0.35);  // nothing the picker's confirming press left over presses here (StartInstall)
          if (!path.empty()) {
            Set("kk_ps2_movie_folder", path);
          }
        });
        it.value_text.assign(reinterpret_cast<const char*>(folder.data()), folder.size());
        it.on_default = [] { Reset("kk_ps2_movie_folder"); };
        items.push_back(std::move(it));
      }
    }

    items.push_back(Section("Diagnostics"));
    return items;
  }

  // ----------------------------------------------------------- Gameplay ---
  std::vector<Item> GameplayItems() {
    // The order of settings_layout.h (kGameplayRows).
    std::vector<Item> items;
    items.push_back(Section("Subtitles and language"));
    items.push_back(SubtitlesItem());
    items.push_back(CvarChoice("Language", "The game's language, where the game includes it.", "user_language",
                               {{"English", "1"},
                                {"Deutsch", "3"},
                                {"Español", "5"},
                                {"Franç" "ais", "4"},
                                {"Italiano", "6"}}));
    items.push_back(Section("HUD"));
    {
      const auto profile = ShownProfileOptions();
      items.push_back(ProfileToggle("Aiming visor", "Show the game's aiming visor.", play::ProfileOption::kAimingVisor,
                                    profile.aiming_visor));
      items.push_back(ProfileToggle("Inventory", "Show the game's inventory indicators.", play::ProfileOption::kInventory,
                                    profile.inventory));
    }
    items.push_back(CvarToggle("Notifications", "A popup when you unlock an achievement in game.",
                               "kk_achievement_toasts"));
    items.push_back(CvarChoice("Popup style",
                               "Xbox 360 unfolds from a green ring at the bottom of the screen. Steam slides up in "
                               "the bottom right corner. Try them with Test notification.",
                               "kk_achievement_style", {{"Xbox 360", "xbox360"}, {"Steam", "steam"}}));
    // Performance metrics moved here from Display (owner, 18 Oct); the camera rows went to Display > Camera.
    items.push_back(Section("Overlay"));
    items.push_back(CvarChoice("Performance metrics",
                               "Readings in the top left corner while you play. F2 switches between them.",
                               "kk_performance_metrics", {{"Off", "off"}, {"FPS only", "fps"}, {"Full", "full"}}));
    items.push_back(Section("Game"));
    items.push_back(CvarToggle("Startup logos",
                               "The Ubisoft, Universal and WingNut movies before the title screen. Story movies "
                               "still play.",
                               "kk_skip_intros", "Play", "Skip"));
    return items;  // the cheats have their own tab (CheatItems)
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
                           "Which controller's buttons the game and its menus show in menus and hints. Automatic "
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
    // The mouse rows show the mouse's picture (Beta 12: moving the mouse turns the camera and aims).
    auto mouse_icon = [this](Item& it) {
      it.icon = glyphs_.Named("keyboard", kk::key_glyphs::kMouseMove, &it.icon_aspect);
      it.icon_dim = it.disabled;
    };
    {
      Item it = CvarSlider("Mouse sensitivity",
                           "How far the camera turns as you move the mouse. It turns the same however fast you move "
                           "it, as in other first person games.",
                           "kk_mouse_sensitivity", 0.1f, 5.0f, 0.05f, [](float v) {
                             char buf[32];
                             std::snprintf(buf, sizeof(buf), "%.2f" KK_TIMES, v);
                             return std::string(buf);
                           });
      mouse_icon(Disable(it, !mnk, mnk_note));
      items.push_back(std::move(it));
    }
    {
      Item it = CvarToggle("Mouse camera",
                           "Turn the camera with the mouse. Off, it turns with the keys bound to the right stick.",
                           "mnk_mouse");
      mouse_icon(Disable(it, !mnk, mnk_note));
      items.push_back(std::move(it));
    }
    {
      Item it = CvarToggle("Mouse vertical", "Which way moving the mouse up and down moves the camera.",
                           "kk_mouse_invert_y", "Normal", "Inverted");
      mouse_icon(Disable(it, !mnk || !GetBool("mnk_mouse"), mnk_note));
      items.push_back(std::move(it));
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

  // The picture of the first key bound in `binds` that has one (key_glyphs.h: the mouse buttons are the mouse's
  // pictures), for the Keyboard bindings rows; nullptr if none has.
  rex::ui::ImmediateTexture* KeyGlyph(const std::string& binds, float* aspect) {
    for (const std::string& key : kk::key_glyphs::PictureNames(binds))
      if (auto* t = glyphs_.Named("keyboard", key, aspect)) return t;
    return nullptr;
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
      it.icon = KeyGlyph(e->getter(), &it.icon_aspect);
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
  // Settings' Cheats tab (settings_layout.h kCheatsRows), in the launcher and the pause menu.
  std::vector<Item> CheatItems() {
    kk::coop::RefreshSessionCheats();
    const bool host_control = kk::CheatsSetByHost();
    const auto effective = kk::EffectiveCheats();
    std::vector<Item> items;
    items.push_back(Section("Cheats"));
    items.push_back(CvarToggle("Cheats",
                               "Switches on the cheats you pick below when a chapter starts, the same as typing their "
                               "codes on the game's Cheat screen. Changes made while playing apply at once. They "
                               "last until you quit the game.",
                               "kk_cheats"));
    const bool on = effective.Enabled();
    if (host_control) { items.back().index = on ? 1 : 0; Disable(items.back(), true, "Set by the host"); }
    items.push_back(Section("Cheat list"));
    for (const auto& cheat : kk::kCheats) {  // the list (cheats.h), not the Page
      // Every chapter is open in the Play menu's Chapter select (docs/menus.md): no "All chapters".
      if (std::string_view(cheat.id) == "chapters") continue;
      Item it = CvarToggle(cheat.label, "One of the game's own cheats.", std::string("kk_cheat_") + cheat.id);
      it.help_key = "Cheat";
      it.facts.push_back(F("Code: {0}", {cheat.code}));
      // In game, a cheat this chapter switches off (cheat_rules.h; the Hayes chapter's weapon cheats, owner
      // 19 Oct) is greyed in place and can't be switched on; its setting is kept for the other chapters.
      Disable(it, !on, "Switch Cheats on to use it.");
      if (in_game_ && kk::CheatSuppressedNow(cheat.id)) {
        Disable(it, true, kk::cheat_rules::kNotInThisChapter);
        it.index = 0;  // shows "Off": it is off here (task 252); kk_cheat_<id> itself is unchanged
      }
      if (host_control) {
        it.index = !(in_game_ && kk::CheatSuppressedNow(cheat.id)) &&
            effective.Chosen(unsigned(&cheat - kk::kCheats.data())) ? 1 : 0;
        Disable(it, true, "Set by the host");
      }
      items.push_back(std::move(it));
    }
    return items;
  }

  // Graphics > Advanced: Rebuild shaders (the launcher's only: the game uses them while it runs). The same
  // job as Home's shader status, shown on the shader screen with its one progress bar.
  std::function<void()> rebuild_shaders_;
  void RebuildShadersItem(std::vector<Item>& items) {
    if (in_game_ || !rebuild_shaders_ || !NativeShaderPack::Wanted()) return;
    items.push_back(ActionItem("Rebuild shaders",
                               "Prepares all of the game's shaders again from the start, for Vulkan and DirectX 12, "
                               "even the ones already prepared. It takes a few minutes, longer than the setup, which "
                               "keeps shaders that are already prepared. It can help after a graphics driver update, "
                               "or if the game shows graphics problems.",
                               "", rebuild_shaders_));
    items.push_back(CvarToggle("Damn it, I paid for the full CPU", "Removes the CPU limit while shaders are prepared. They finish faster, but your PC may slow down until they are done. The limit is there for a reason: turning this on may cause a crash.", "kk_shader_full_cpu"));
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

  // In game (owner, 18 Oct: the pause menu shows as many settings as it can): every row the launcher has,
  // and those that cannot change while the game runs greyed out with a note saying where to change them:
  // the window, resolution, refresh rate and monitor, VSync (Vulkan picks its present mode when the
  // swapchain is made), render quality and its internal resolution, the look and preset, Recommended, the
  // graphics API and renderer, the language and the startup logos. The upscaler is live between the
  // temporal upscalers and Off, greyed while FSR 1 or NIS is in use (UpscalingItems). Sections left empty go.
  static std::vector<Item> LiveOnly(std::vector<Item> items) {
    std::vector<Item> out;
    for (Item& it : items) {
      if (it.disabled_note == kSetByOriginal) it.disabled_note = kSetByOriginalInGame;
      if (it.type != ItemType::kSection && help::RestartOnly(it.label)) Disable(it, true, kInLauncherOnly);
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
      "Set by the Original Xbox 360 look. Change Look from the main menu to set it.";
  static constexpr const char* kInLauncherOnly =
      "This can't be changed while you play. Change it from the main menu.";

  // The description panel's structured help (settings_help.h, task 220) for each row, and when a change to it
  // takes effect: in game, a row that can't change while playing says so; in the launcher, the rows the pause
  // menu greys apply when the game starts and the others change at once (also while playing).
  void AttachHelp(std::vector<Item>& items) const {
    for (Item& it : items) {
      if (it.type == ItemType::kSection) continue;
      it.help = help::Find(it.help_key ? std::string_view(it.help_key) : std::string_view(it.label));
      it.applies = nullptr;
      if (in_game_ && it.disabled && it.disabled_note == kInLauncherOnly) {
        it.applies = help::kNotWhilePlaying;
        continue;
      }
      if (!it.help) continue;
      help::Applies a = it.help->applies;
      if (a == help::Applies::kAuto) a = help::RestartOnly(it.label) ? help::Applies::kGameStart : help::Applies::kAtOnce;
      switch (a) {
        case help::Applies::kAtOnce: it.applies = help::kChangesAtOnce; break;
        case help::Applies::kGameStart: it.applies = in_game_ ? help::kNotWhilePlaying : help::kAppliesAtStart; break;
        case help::Applies::kNextStart: it.applies = help::kNextStartLabel; break;
        default: break;
      }
    }
  }

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
    content_action_ = [this](content::Kind kind, int verb) { OnContentAction(kind, verb); };
    EnforceOriginalLook();
    // Which temporal upscalers this PC runs (Display > Upscaling): asked now, in the background.
    ApplyRendererChoice();      // the renderer decided, the default upscaler follows it
    OfferedTemporalUpscalers();
    LoadArt();
    // Installed before the launcher took its art from the game files: take it now.
    if (files_ok_ && !art::HasLauncherArt(ArtDir())) StartArtExtraction(false);
    {
      // An install that has run before (a settings file, or a version remembered): not a fresh one. Decided now,
      // before this start writes the settings file and kk_last_version (task 350a, StartFullSetup).
      std::error_code config_ec;
      existing_install_ = !Get("kk_last_version").empty() || std::filesystem::exists(paths_.config_path, config_ec);
    }
    // The native renderer's shaders (native_shaders.h): checked each time the launcher opens, built
    // in the background from the game files when missing or stale, and built again in full after
    // every update (a version this launcher hasn't run: an update, a beta, a channel switch), shown by
    // the after-update screen (DrawShaderScreen). Not on a fresh install: its setup builds them.
    if (files_ok_) {
      std::error_code config_ec;
      const bool first_start = Get("kk_last_version").empty() &&
                               !std::filesystem::exists(paths_.config_path, config_ec);
      const bool updated = UpdatedSinceLastRun();
      // Owner rule (Beta 8, 18 Oct): ONE shader preparation per version (first install or update), any other
      // only when the player asks (Rebuild). A version already prepared is never checked or rebuilt
      // here; pipelines a new setting needs are made by the game in the background (shader_prepared.h).
      switch (JobAtLauncherStart(updated, ShadersPreparedForThisVersion())) {
        case ShaderJobAtStart::kRebuild: native_pack_.Start(CacheDir(), paths_.game_dir, true); break;
        case ShaderJobAtStart::kPrepare:
          REXLOG_INFO("KK: shaders not prepared for v{} yet (kk_shaders_prepared '{}'): preparing them", KK_VERSION,
                      Get("kk_shaders_prepared"));
          native_pack_.Start(CacheDir(), paths_.game_dir);
          break;
        case ShaderJobAtStart::kNone:
          REXLOG_INFO("KK: shaders already prepared for v{} ({}); not checking them again", KK_VERSION,
                      Get("kk_shaders_prepared"));
          break;
      }
      if (first_start && NativeShaderPack::Wanted()) {
        // Existing game files with fresh user settings still need the setup's one shader screen.
        setup_shaders_ = true;
        REXLOG_INFO("KK: first start: preparing both native shader packs on the setup screen");
      }
      // ... then the texture pack and PS2 cutscene pages, the graphics pages and the rest, as after an install.
      if (first_start) StartFullSetup(false);  // saved below with kk_last_version
      // Task 350a (owner, 21 Oct): updating to Beta 14 takes every existing player through the first-time setup once
      // more, from the new defaults (setup_again.h); the game files page is skipped (they are here). The shaders this
      // update prepares (both packs: the DirectX 12 one too) show as the setup's shader step, not the after-update
      // screen (task 365: it stays until they are ready, as in a first setup).
      const bool again = !first_start && setup_again::Due(existing_install_, GetInt("kk_setup_again"),
                                                         std::getenv("KK_AUTOPLAY") != nullptr);
      if (again) {
        if (NativeShaderPack::Wanted() && native_pack_.Busy()) setup_shaders_ = true;
        REXLOG_INFO("KK: setup again (round {}, kk_setup_again {}): the first-time setup from the new defaults{}",
                    setup_again::kRound, GetInt("kk_setup_again"),
                    setup_shaders_ ? ", the shaders as its first step" : ", shaders already prepared");
        StartFullSetup(false);  // saved below with kk_last_version (or at once: SaveSettings after it)
        SaveSettings(paths_.config_path);
      }
      if (updated && NativeShaderPack::Wanted() && !again) {
        update_shaders_ = true;
        REXLOG_INFO("KK: first start of v{} (last v{}): the native renderer's shaders are built again", KK_VERSION,
                    Get("kk_last_version"));
      }
    }
    toast_ = std::make_unique<AchievementToast>(drawer, immediate_, paths_.game_dir, paths_.user_dir);
    StartContent();
    StartNotifications();
    CleanUpAfterUpdate();
    // Task 370: nothing an update installs or removes may be in the user or game data folders; then the first start
    // check (FinishUpdate): files an older updater left out are downloaded again (the update screen shows it).
    SetProtectedFolders({paths_.user_dir, paths_.game_dir});
    StartFinishCheck();
    online::Start();
    discord::Start();
    const bool testing = std::getenv("KK_AUTOPLAY") != nullptr;
    if (GetBool("kk_check_updates") && !testing) StartUpdateCheck(false);
    StartSteamCheck();
    // Task 373's Home countdown card is gone (owner, 30 Oct: the Remastered update is out); nothing fetched.
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
        REXLOG_INFO("KK: What's new pending (last run v{}): it opens on the main menu once the game files, shaders "
                    "and setup are done", last);
        // Job J6: a player who finished the setup on an older build gets the one-time New in this update screen
        // (the texture pack, the PS2 cutscenes, the new graphics pages). Remembered until it is dismissed, so
        // closing the launcher on it shows it again next time; never on a fresh install (no settings file).
        if (Get("kk_setup_version").empty()) Set("kk_setup_version", kSetupWaiting);
        artbook_want_ = "first start after an update";  // Beta 7: the bonus disc's artbook, if missing
      }
      Set("kk_last_version", KK_VERSION);
      SaveSettings(paths_.config_path);
    }
    update_new_ = files_ok_ && !testing && Get("kk_setup_version") == kSetupWaiting;
    if (update_new_) REXLOG_INFO("KK: the New in this update screen is waiting (setup steps last had: older build)");
    // Back from the game (Quit game, the end of a chapter-select run): straight to the Play menu
    // or Chapter select.
    const std::string open_on = Get("kk_launcher_open");
    rex::cvar::ResetToDefault("kk_launcher_open");  // never saved
    // Back from a benchmark run: its results, over the page it was started from (Home's Benchmark entry, or
    // Settings' About tab).
    if (files_ok_ && (open_on == "benchmark" || open_on == "benchmark-settings")) {
      about_in_settings_ = open_on == "benchmark-settings";
      page_ = about_in_settings_ ? kAbout : kHome;
      if (about_in_settings_) settings_page_ = kAbout;
      open_benchmark_results_ = true;
      REXLOG_INFO("KK launcher: back from the benchmark to {}", about_in_settings_ ? "Settings > About" : "Home");
    }
    if (files_ok_ && (open_on == "play" || open_on == "chapters")) {
      OpenPlayMenu();
      if (open_on == "chapters") OpenPlayView(PlayView::kChapters);
    }
    if (files_ok_ && open_on == "extras") {  // back from an Extras bonus: Play > Extras
      OpenPlayMenu();
      OpenPlayView(PlayView::kExtras);
    }
    // Task 330: a launcher restarted for a shader rebuild (the live menu scene held the shader pack open).
    if (files_ok_ && open_on == "rebuild" && NativeShaderPack::Wanted() && !native_pack_.Busy()) {
      native_pack_.Start(CacheDir(), paths_.game_dir, true);
      rebuild_shaders_screen_ = true;
      REXLOG_INFO("KK: rebuilding the native renderer's shaders (asked for, launcher restarted for it)");
    }
    // Task 332: opened from the desktop with the setup done, the startup movies first (unless kk_skip_intros), then
    // the save notice loop; task 331: otherwise the owner's loading loop from the first frame (decoding starts now,
    // while the scene starts).
    // Task 349c: a launcher started again for the scene's second attempt does not play them again.
    if (!menu_scene::SecondAttempt() &&
        startup_movies::AtLauncherStart(open_on, files_ok_, InSetup(), GetBool("kk_skip_intros"))) {
      StartStartupMovies("the launcher opened");
    } else {
      if (open_on.empty())
        REXLOG_INFO("KK startup movies: not at this start ({})", !files_ok_ ? "no game files"
                                                                : InSetup() ? "a setup or update screen first"
                                                                            : "kk_skip_intros is on");
      // Owner (20 Oct): a first start goes straight to the setup; the save notice comes after the setup and the
      // movies (StartMoviesAfterSetup).
      if (open_on.empty() && (!files_ok_ || InSetup()))
        REXLOG_INFO("KK loading loop: none before the setup");
      else
        StartLoadingLoop(open_on);
    }
    loop_open_on_ = open_on;  // task 349c: the second attempt opens as this one did
    MaybeStartScene(!open_on.empty());  // back from the game (or a restart): before the first frame
    saved_changes_ = seen_changes_ = g_changes;
    StartHeartbeat();
  }

  ~Launcher() override {
    StopHeartbeat();
    notify::Stop();
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

 public:
  // Task 330: over the live menu scene the game's frames already repaint the window (60 a second at most,
  // menu_scene.h); painting on top of them as well drew the launcher about 180 times a second.
  bool NeedsContinuousPaint() override { return !(menu_scene::Active() && menu_scene::Ready()); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    NoteDrawn();
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
      auto frame = idle ? std::chrono::microseconds(33333) : std::chrono::microseconds(0);
#else
      auto frame = std::chrono::microseconds(idle ? 33333 : 16667);
#endif
      // Task 330: with the live menu scene the game's frames set the pace (its frame rate cap); a wait here
      // would hold its presentation.
      if (menu_scene::Active() && menu_scene::Ready()) frame = std::chrono::microseconds(0);
      if (frame.count() > 0) {
        if (next_frame_ > clock::now()) std::this_thread::sleep_until(next_frame_);
        next_frame_ = std::max(next_frame_, clock::now() - frame) + frame;
      } else {
        next_frame_ = clock::now();
      }
    }
#if defined(KK_DEV_TOOLS)
    const auto draw_start = std::chrono::steady_clock::now();
    // Task 330: KK_DEV_FRAME_LOG=1 logs the launcher's frame intervals every 5 s (live menu scene or stills).
    if (static const bool frame_log = std::getenv("KK_DEV_FRAME_LOG") != nullptr; frame_log) {
      static std::vector<float> gaps;
      static auto last = draw_start, since = draw_start;
      gaps.push_back(std::chrono::duration<float, std::milli>(draw_start - last).count());
      last = draw_start;
      if (draw_start - since > std::chrono::seconds(5)) {
        std::sort(gaps.begin(), gaps.end());
        float sum = 0;
        for (float g : gaps) sum += g;
        REXLOG_INFO("KK dev frames: {} in {:.1f} s, mean {:.2f} ms, median {:.2f}, p95 {:.2f}, max {:.2f} (scene {} ready {} sound {:.2f})",
                    gaps.size(), std::chrono::duration<float>(draw_start - since).count(), sum / gaps.size(),
                    gaps[gaps.size() / 2], gaps[gaps.size() * 95 / 100], gaps.back(), menu_scene::Active(),
                    menu_scene::Ready(), scene_gain_);
        gaps.clear();
        since = draw_start;
      }
    }
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
    // Task 330: over the game's own frame (the live menu scene) the window has no background of its own.
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImGui::ColorConvertU32ToFloat4(menu_scene::Active() ? IM_COL32(0, 0, 0, 0) : color::kBase));
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
      FadeSceneSound(0.0f, io.DeltaTime);  // task 330: the scene's sound fades with it
      // Task 349c: starting again for the scene's second attempt: the loading loop stays until the new window shows.
      if (scene_retrying_ && DrawLoadingLoop(dl, io.DeltaTime)) {
        ImGui::End();
        return;
      }
      DrawBackdrop(dl);
      ImGui::End();
      return;
    }

    input_.set_keyboard_enabled(capturing_.empty() && !editing_name_ && !social_.editing());
    // Controllers count only while the launcher is the active window (REPORT-188: a controller used in
    // another program pressed buttons here). Ours again once the player brings it back (a click, the taskbar,
    // a notification), e.g. for a finished download's pop-up after Run in background.
    if (cb_.window_state) {
      const bool active = cb_.window_state().second;
      // Task 229: a player's log had the window lose the foreground for 33 ms every 5.5 s. Log who took it,
      // and after the first few changes one summary a minute (the log tail keeps only 200 lines).
      const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count();
      if (active != pads_active_) {
        if (!active) {
          if (focus_log_.Lost(now_ms))
            REXLOG_INFO("KK: launcher controller input ignored (window not active; in front: {})", ForegroundWindowOwner());
        } else {
          int64_t lost_ms = -1;
          if (focus_log_.Gained(now_ms, lost_ms))
            REXLOG_INFO("KK: launcher controller input on (window active{})",
                        lost_ms >= 0 ? " again after " + std::to_string(lost_ms) + " ms" : std::string());
        }
      }
      if (const std::string summary = focus_log_.Summary(now_ms); !summary.empty()) REXLOG_INFO("{}", summary);
      pads_active_ = active;
      input_.set_pads_active(active || egg_running_);  // task 357: Start over the Easter egg
    }
    input_.Update();
    input_diag::Launcher(pads_active_, input_.gamepad_count(), false, social_.editing());
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
    // Task 357: the Easter egg game is running: only its pause panel (Start) is drawn and answered.
    if (egg_running_ && DrawEgg(dl, io)) {
      ImGui::End();
      NoteActivity(io);
      return;
    }
    // Back from a benchmark: its results page opens first (owner, 19 Oct: a pop-up covered it). Every pop-up
    // that opens by itself (update offer, What's new, the startup notices below) waits for a free moment, so
    // they come after the player closes the results.
    if (open_benchmark_results_ && !modal_.open && !ShaderScreen() && !InSetup()) {
      open_benchmark_results_ = false;
      OpenBenchmarkResults();
    }
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
    // Task 228: the launcher has shown (a crash before this, or while shaders are prepared, is told
    // at the next start), and the last start that ended unexpectedly is explained once.
    if (shown_frames_ < 30) ++shown_frames_;
    if (shown_frames_ >= 30) startup::LauncherShown(native_pack_.Working());
    if (!crash_notice_shown_ && !modal_.open && shown_frames_ >= 30) {
      crash_notice_shown_ = true;
      if (const std::string phase = startup::CrashNotice(); !phase.empty()) {
        startup::CrashNoticeShown();
        std::string body = std::string(T(phase == "shaders" ? "The last start ended unexpectedly while shaders were being prepared."
                                                : "The last start ended unexpectedly before the main menu showed.")) +
                           "\n\n" +
                           T("A damaged graphics driver shader cache can cause this, most often on AMD graphics cards. "
                             "The game's own pipeline cache was reset. If it happens again, close the game, empty the "
                             "folders VkCache, DxCache and DxcCache in %LOCALAPPDATA%\\AMD, then start the game again.");
        std::vector<std::string> buttons = {"OK"};
        if (startup::AmdCacheOffer()) {
          body += std::string("\n\n") + T("Clear AMD shader cache empties those folders the next time you start the game. Other "
                             "games and apps then rebuild their shaders once.");
          buttons.push_back("Clear AMD shader cache");
        }
        OpenModal("The game closed unexpectedly", body, buttons, 0, [](int i) {
          if (i == 1) startup::RequestAmdCacheClear();
        });
      } else if (const std::string report = startup::AmdCacheReport(); !report.empty()) {
        const auto space = report.find(' ');
        std::string body = F("The AMD shader cache was emptied ({0} files).", {report.substr(0, space)});
        if (report.substr(space + 1) != "0")
          body += " " + F("{0} files in use by another program were left.", {report.substr(space + 1)});
        OpenModal("Clear AMD shader cache", body, {"OK"}, 0, {});
      }
    }
    if (files_ok_ && !music_) music_ = std::make_unique<LauncherMusic>(paths_.game_dir);
    if (music_) {
      // Silent while the native renderer's shaders are built (setup, after an update, before Play): the
      // build takes nearly every hardware thread and the music would stutter. It fades back in after.
      const bool on = GetBool("kk_launcher_music") && !native_pack_.Working();
      const float v = on ? GetInt("kk_launcher_music_volume", 50) / 100.0f : 0.0f;
      // Task 330 (owner, 20 Oct): with the live menu scene its own sound plays (the same music, with the sea),
      // at this volume (FadeSceneSound); the launcher's copy of the music then stays silent.
      // Task 332: silent under the startup movies (they have their own sound).
      music_->Update(SceneSoundPlays() || MoviesPending() ? 0.0f : v * v, io.DeltaTime);  // a gentler curve than linear
    }
    if (menu_scene::Active()) {
      const bool on = GetBool("kk_launcher_music") && !native_pack_.Working();
      const float v = on ? GetInt("kk_launcher_music_volume", 50) / 100.0f : 0.0f;
      FadeSceneSound(SceneSoundPlays() && !MoviesPending() ? v * v : 0.0f, io.DeltaTime);
    }
    if (files_ok_ && GetBool("kk_launcher_sounds") && !ui_sounds_)
      ui_sounds_ = std::make_unique<LauncherSounds>(paths_.game_dir);

    // Task 332: the startup movies over the whole window first (only their pop-up is drawn and answered over them).
    if (movies_asking_) {  // task 332 follow-up: the movies' pop-up alone, on the plain dark background
      dl->AddRectFilled(o_, ImVec2(o_.x + w_, o_.y + h_), color::kBase);
      ui::DrawModal(dl, modal_, o_, ImVec2(o_.x + w_, o_.y + h_), m_, input_);
      const Action clicked = ui::DrawPrompts(dl, glyphs_, PromptSet(), Prompts(), o_.x + w_ - Margin(),
                                             o_.y + h_ - 42 * S(), m_, true);
      if (clicked != Action::kCount) pending_click_ = clicked;
      ImGui::End();
      NoteActivity(io);
      return;
    }
    if (DrawStartupMovies(dl, io.DeltaTime)) {
      if (modal_.open) {  // the pop-up and its button prompts (no status line over the movie)
        ui::DrawModal(dl, modal_, o_, ImVec2(o_.x + w_, o_.y + h_), m_, input_);
        const Action clicked = ui::DrawPrompts(dl, glyphs_, PromptSet(), Prompts(), o_.x + w_ - Margin(),
                                               o_.y + h_ - 42 * S(), m_, true);
        if (clicked != Action::kCount) pending_click_ = clicked;
      }
      ImGui::End();
      NoteActivity(io);
      return;
    }
    // Task 331: the loading loop over the whole window: nothing else is drawn or answered until it fades.
    if (DrawLoadingLoop(dl, io.DeltaTime)) {
      ImGui::End();
      return;
    }
    CoopTick();  // C81
    DrawBackdrop(dl);
    if (ShaderScreen() || InSetup()) card_focus_ = -1;  // the Home countdown's buttons are Home's only
    if (ShaderScreen()) {
      DrawShaderScreen(dl);
    } else if (InSetup()) {
      DrawSetup(dl);
    } else {
      if (open_benchmark_results_ && !modal_.open) {
        open_benchmark_results_ = false;
        OpenBenchmarkResults();
      }
      // Task 357: the controller code, on Home only.
      if (!PageInputBlocked() && play_ == PlayView::kNone && page_ == kHome) EggCodeInput();
      else egg_code_.Reset(), egg_held_codes_.Reset(), egg_holding_ = false;
      if (!PageInputBlocked()) HandleGlobalInput();
      if (play_ != PlayView::kNone) {
        card_focus_ = -1;
        DrawPlayHeader(dl);
        if (play_ == PlayView::kMenu) DrawPlayMenu(dl);
        else DrawPlayPage(dl);
      } else {
        DrawTopBar(dl);
        if (page_ == kHome) DrawHome(dl);
        else if (page_ == kWorkshop) DrawWorkshop(dl);
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
    if (!first_frame_noted_) {
      first_frame_noted_ = true;
      nr::steps::Note("launcher: first frame drawn");  // task 353
    }
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
             installing_ || art_progress_.busy || (update_ && update_->busy) || status_focus_ != last_status_focus_ ||
             coop_gate_.open() ||  // UI1: the early access warning's countdown
             MoviesPending();  // task 332: the movies at their own frame rate
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
    compare_art_.clear();  // the setup's pictures (their textures were in textures_)
    setup_stills_.clear();
    setup_stills_loaded_ = false;
    workshop_art_.clear();  // task 221 (their textures were in textures_)
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
      // With the native renderer, the shaders from the new game files; then the rest of the setup.
      if (NativeShaderPack::Wanted()) {
        native_pack_.Start(CacheDir(), paths_.game_dir);
        setup_shaders_ = true;
      }
      StartFullSetup();
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
    const bool home = !InSetup() && page_ == kHome && (play_ == PlayView::kNone || play_ == PlayView::kMenu);
    home_mix_ = ui::Approach(home_mix_ * 100.0f, home ? 100.0f : 0.0f, 9.0f) / 100.0f;
    slide_clock_ += std::min(ImGui::GetIO().DeltaTime, 0.1f);
    const ImVec2 p1(o_.x + w_, o_.y + h_);
    // Task 330: the game's start menu scene, live, instead of the stills once it is up (menu_scene.h).
    scene_mix_ = menu_scene::StepMix(scene_mix_, menu_scene::Active() && menu_scene::Ready() && !PlainSetup(),
                                     ImGui::GetIO().DeltaTime);
    const float still = 1.0f - scene_mix_;  // the stills' share
    if (still <= 0.0f) {
      // The scene only: nothing to draw under the darkening below.
    } else if (!slides_.empty() && !PlainSetup()) {
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
                     ui::WithAlpha(IM_COL32_WHITE, alpha * still));
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
      dl->AddRectFilledMultiColor(o_, p1, ui::WithAlpha(IM_COL32(22, 28, 36, 255), still),
                                  ui::WithAlpha(IM_COL32(26, 33, 42, 255), still), ui::WithAlpha(color::kBase, still),
                                  ui::WithAlpha(color::kBase, still));
      if (!glow_) glow_ = MakeTexture(Glow());
      const ImVec2 moon(o_.x + w_ * 0.76f, o_.y + h_ * 0.28f);
      const float r = h_ * 0.62f;
      dl->AddImage(ui::Tex(glow_), ImVec2(moon.x - r, moon.y - r), ImVec2(moon.x + r, moon.y + r),
                   ImVec2(0, 0), ImVec2(1, 1), ui::WithAlpha(IM_COL32_WHITE, still));
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
  // The top of every page but the setup's: on Settings its eight tabs (LB and RB, or a click, move between
  // them) with SETTINGS at the right; on Achievements the page's name; on Home nothing but the online player
  // count. The Play pages and the Workshop have their own (DrawCrumbs).
  void DrawTopBar(ImDrawList* dl) {
    if (page_ == kWorkshop) return DrawWorkshopHeader(dl);
    const float s = S();
    const online::Counts counts = online::Get();
    const bool show_counts = counts.known && !InSetup() && page_ == kHome;
    const ui::Fonts f = ui::GetFonts();
    const float right = o_.x + w_ - Margin();
    const float ly = std::round(o_.y + 74 * s);
    if (page_ != kHome) {
      const std::string caption = OnSettingsTab() ? Upper(T("Settings")) : std::string("KING KONG");
      const float caption_fs = 15 * s, caption_tr = 4 * s;
      const float caption_width = ui::TrackedWidth(f.display, caption_fs, caption, caption_tr);
      float ux = 0, uw = 0;
      if (OnSettingsTab()) {
        if (tab_names_lang_ != int(text::Current()) || tab_names_.empty()) {  // made once per language
          tab_names_.clear();
          for (int p = kFirstSettingsPage; p <= kLastSettingsTab; ++p) tab_names_.push_back(Upper(T(kPageNames[p])));
          tab_names_lang_ = int(text::Current());
        }
        const int clicked = ui::DrawTabs(dl, glyphs_, PromptSet(), tab_names_, int(page_) - kFirstSettingsPage,
                                         ImVec2(o_.x + Margin(), o_.y + 44 * s), m_, !PageInputBlocked(), &ux, &uw,
                                         w_ - Margin() * 2 - caption_width - 32 * s);
        if (clicked >= 0 && clicked != int(page_) - kFirstSettingsPage) {
          ui::Cue(ui::Sound::kTab);
          GoTo(Page(kFirstSettingsPage + clicked), /*settings_tab=*/true);
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

  // Settings' tabs: a page with settings rows, or About when it was opened as Settings' last tab.
  bool OnSettingsTab() const { return IsSettingsPage(page_) || (page_ == kAbout && about_in_settings_); }

  // `settings_tab`: open `page` as one of Settings' tabs (Home's Settings, the tab bar). About is always
  // Settings' last tab (owner, 18 Oct: Home's About entry became Benchmark).
  void GoTo(Page page, bool settings_tab = false) {
    if (page == kDisplay && page_ != kDisplay) {
      monitors_.clear();
      display_modes_monitor_ = -1;
      LoadDisplayModes();
    }
    (void)settings_tab;
    const bool about_tab = page == kAbout;
    if (page != page_ || about_tab != about_in_settings_)
      REXLOG_INFO("KK launcher: page {}{}", kPageNames[page], about_tab || IsSettingsPage(page) ? " (Settings tab)" : "");
    about_in_settings_ = about_tab;
    if (page != page_) sub_ = kNoSubPage;
    if (IsSettingsPage(page) || about_tab) settings_page_ = page;
    extras_return_focus_ = -1;  // only the Graphics visit an Extras row opened returns there
    side_focus_ = -1;
    card_focus_ = -1;
    play_ = PlayView::kNone;
    const bool opening_workshop = page == kWorkshop && page_ != kWorkshop;
    page_ = page;
    if (page == kWorkshop) EnsureWorkshop();  // task 209: one page with three rows
    if (opening_workshop) workshop_->OnOpen();  // task 221: the projects row in a fresh random order
  }

  // ---------------------------------------------------------- bottom bar ---
  bool PageInputBlocked() const { return modal_.open; }
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
    const auto prompts = Prompts();
    const Action clicked = ui::DrawPrompts(dl, glyphs_, PromptSet(), prompts, o_.x + w_ - Margin(), cy, m_,
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
    if (ShaderScreen()) return {};  // task 365: nothing to press while the shaders are prepared
    if (InSetup()) {
      switch (SetupStage()) {
        case 0:
          if (side_focus_ >= 0) return {{Action::kAccept, SidePanelVerb(false)}, {Action::kBack, "Back"}};
          if (menu_value_) return {{Action::kLeft, "Change"}, {Action::kAccept, "Select"}, {Action::kBack, "Quit"}};
          return {{Action::kAccept, "Select"}, {Action::kBack, "Quit"}};
        case 1: return {{Action::kBack, "Cancel"}};
        case 6:
        case 7: return {{Action::kAccept, "Select"}};
        case kWorkshopStage: return EndPrompts();
        case kSettingsFirstStage:
        case kSettingsFirstStage + 1:
        case kSettingsFirstStage + 2: return SetupSettingsPrompts();
        case 8:
        case 9:
        case 10:
        case 11: return GfxPrompts();
        case kPromptsStage:
        case kAllSetStage: return EndPrompts();
        case kNewStage: return NewPrompts();
        default: return {};
      }
    }
    if (play_ != PlayView::kNone) return PlayPrompts();
    if (page_ == kWorkshop && workshop_) {  // job J8; task 209: one page with three rows
      p = workshop_->Prompts();
      if (CanPlay()) p.insert(p.end() - 1, {Action::kPlay, "Play"});
      return p;
    }
    if (page_ == kHome) {
      if (card_focus_ >= 0)  // as CardButtons: Check for updates (once it's out), then Join the Discord
        return {{Action::kAccept, LaunchIsOut() && card_focus_ == 0 ? "Select" : "Open in browser"},
                {Action::kBack, "Back"}};
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
    if ((in.Pressed(Action::kPrevTab) || in.Pressed(Action::kNextTab)) && page_ != kWorkshop) {  // Workshop: its own
      // LB and RB move between Settings' tabs, About included, wrapping round (only on Settings).
      if (OnSettingsTab()) {
        const int dir = in.Pressed(Action::kNextTab) ? 1 : -1, n = kSettingsTabCount;
        ui::Cue(ui::Sound::kTab);
        GoTo(Page(kFirstSettingsPage + (int(page_) - kFirstSettingsPage + dir + n) % n), /*settings_tab=*/true);
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
      } else if (page_ == kWorkshop && workshop_) {  // task 209: an inner page back to its card, then Home
        if (!workshop_->Back()) GoTo(kHome);
      } else if (page_ != kHome) GoTo(kHome);
      else if (status_focus_ || side_focus_ >= 0 || card_focus_ >= 0)  // back to the menu
        status_focus_ = false, side_focus_ = -1, card_focus_ = -1;
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
    std::string badge;                // UI1: a small brass note after the label (waiting co-op invites)
  };

  std::vector<HomeEntry> HomeEntries() {
    std::vector<HomeEntry> e;
    e.push_back({"Play", CanPlay(), [this] { OpenPlayMenu(); }});
    // C81: directly under Play, only with co-op enabled (one text key; Craig may change its wording).
    if (CoopEnabled()) {
      e.push_back({"Play Online Co-op", CanPlay(), [this] { OpenCoop(coop::CoopPage::Hub); }});
      // UI1: the Social drawer showed Steam invites; it is gone, so Home shows how many are waiting.
      e.back().badge = CoopInviteBadge();
    }
    // An update that was found stays one press away; checking, reinstalling and the rest are on About.
    if (update_ && update_->found && !installing_update_)
      e.push_back({F("Update to v{0}", {update_->found->version}), true, [this] { OpenUpdatePrompt(); }});
    e.push_back({"Settings", true, [this] { GoTo(settings_page_, /*settings_tab=*/true); }});
    e.push_back({"Workshop", true, [this] { GoTo(kWorkshop); }});  // job J8: the owner's other projects
    e.push_back({"Achievements", true, [this] { GoTo(kAchievements); }});
    e.push_back({"Benchmark", CanPlay(), [this] { OpenBenchmark(); }});  // owner, 18 Oct: About is a Settings tab
    e.push_back({"Quit", true, [this] { Quit(); }});
    return e;
  }

  // A column of large menu entries (Home and the setup screen): the focused
  // one moves in, with a brass mark. Returns the one chosen, or -1. `item_h`:
  // each entry's height when there's less room than usual (0: the usual).
  int DrawMenu(ImDrawList* dl, const std::vector<HomeEntry>& entries, int& focus, float x, float y, float alpha,
               float item_h = 0, bool focusable = true) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const int n = int(entries.size());
    if (n == 0) return -1;
    focus = std::clamp(focus, 0, n - 1);
    const int focus_before = focus;
    menu_value_ = bool(entries[size_t(focus)].change);
    const bool active = focusable && !PageInputBlocked() && side_focus_ < 0 && !status_focus_ && card_focus_ < 0;
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
      const float badge_fs = std::round(fs * 0.58f);
      const float badge_w = e.badge.empty() ? 0.0f : ui::TextSize(f.semibold, badge_fs, e.badge).x + 20 * s;
      if (badge_w > 0) tw += 16 * s + badge_w;
      const ImVec2 r0(x - 12 * s, iy), r1(x + 40 * s + tw, iy + item_h);
      if (active && ImGui::IsMouseHoveringRect(r0, r1, false)) {
        if (input_.mouse_moved()) focus = i;
        if (ImGui::IsMouseClicked(0)) {
          focus = i;
          chosen = i;
        }
      }
      const bool on = focusable && i == focus && side_focus_ < 0 && !status_focus_ && card_focus_ < 0;
      home_offset_[size_t(i)] = ui::Approach(home_offset_[size_t(i)], on ? 22 * s : 0.0f, 18.0f);
      const float off = home_offset_[size_t(i)];
      const float k = off / (22 * s);
      if (k > 0.02f)
        dl->AddRectFilled(ImVec2(x, iy + item_h * 0.5f - 11 * s), ImVec2(x + 3 * s, iy + item_h * 0.5f + 11 * s),
                          ui::WithAlpha(color::kAccent, k * alpha));
      const ImU32 col = !e.enabled ? color::kTextFaint : on ? color::kText : color::kTextDim;
      const float ty = iy + (item_h - fs) * 0.5f;
      ui::DrawTracked(dl, f.display, fs, ImVec2(x + off, ty), ui::WithAlpha(col, alpha), label, tr);
      if (badge_w > 0) {  // UI1: e.g. "1 invite" in a brass pill after PLAY ONLINE CO-OP
        const float bx = x + off + ui::TrackedWidth(f.display, fs, label, tr) + 16 * s, cy = iy + item_h * 0.5f;
        const float bh = badge_fs + 10 * s;
        dl->AddRectFilled(ImVec2(bx, cy - bh * 0.5f), ImVec2(bx + badge_w, cy + bh * 0.5f),
                          ui::WithAlpha(color::kAccent, alpha), bh * 0.5f);
        dl->AddText(f.semibold, badge_fs, ImVec2(bx + 10 * s, cy - badge_fs * 0.5f - 1 * s),
                    ui::WithAlpha(color::kOnFocus, alpha), e.badge.c_str());
      }
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
    if (!PageInputBlocked()) {
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
    for (int i = 0; i < n && !PageInputBlocked(); ++i)
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
    const auto lobby=coop::Social().Snapshot();
    if (lobby.lobby && lobby.members.size()==2) {
      if (request.kind!=play::StartKind::kContinue && request.kind!=play::StartKind::kChapter) {
        OpenModal("Co-op", "Choose Continue or Chapter select for co-op.", {"OK"}, 0, {});
        return;
      }
      if (!coop::SelectSessionStart(request)) {
        OpenModal("Co-op","Could not select chapter",{"OK"},0,{});
        return;
      }
      OpenModal("Co-op", "Chapter selected. Open Play Online Co-op and press Start when both players are Ready.", {"OK"}, 0, {});
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
      cols = {{"Extras", T("Every bonus is unlocked.")},
              {"Bonuses", std::to_string(std::size(play::kExtras) + 2)}};  // and the Artbook and the user manual
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
      case PlayView::kArtbook: return "Artbook";
      case PlayView::kManual: return "User manual";
      case PlayView::kCoop: return "Play Online Co-op";
      default: return "";
    }
  }

  // Instead of the tabs: PLAY (or WORKSHOP), then the page's name (`sub`, nullptr on the menu itself), with
  // the brass underline under the last. A click on the first goes back to its menu: returns true.
  // `slash`: the co-op pages join the two with " / " as in the Design 1 mockups (no dash looking line there).
  bool DrawCrumbs(ImDrawList* dl, const char* root, const char* sub, bool slash = false) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float fs = 15 * s, tr = 1.6f * s, cy = o_.y + 44 * s;
    float x = o_.x + Margin();
    const std::string first = Upper(T(root));
    const float pw = ui::TrackedWidth(f.display, fs, first, tr);
    const bool hot = sub && !PageInputBlocked() &&
                     ImGui::IsMouseHoveringRect(ImVec2(x - 10 * s, cy - 22 * s), ImVec2(x + pw + 10 * s, cy + 22 * s), false);
    const bool clicked = hot && ImGui::IsMouseClicked(0);
    if (clicked) ui::Cue(ui::Sound::kBack);
    ui::DrawTracked(dl, f.display, fs, ImVec2(x, cy - fs * 0.5f),
                    sub ? (hot ? color::kText : color::kTextDim) : color::kText, first, tr);
    float ux = x, uw = pw;
    if (sub) {
      if (slash) {
        x += pw + 18 * s;
        const float sw = ui::TrackedWidth(f.display, fs, "/", tr);
        ui::DrawTracked(dl, f.display, fs, ImVec2(x, cy - fs * 0.5f), color::kTextFaint, "/", tr);
        x += sw + 18 * s;
      } else {
        x += pw + 14 * s;
        dl->AddRectFilled(ImVec2(x, cy - 1 * s), ImVec2(x + 8 * s, cy + 1 * s), color::kTextFaint);
        x += 8 * s + 14 * s;
      }
      const std::string name = Upper(T(sub));
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
    return clicked;
  }

  // PLAY, then the page's name (a click on PLAY goes back to the menu); on Chapter select the save it starts
  // with at the right.
  void DrawPlayHeader(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float cy = o_.y + 44 * s;
    if (play_ == PlayView::kCoop) {
      if (coop_gate_.open()) return;  // UI1: the early access warning covers the window
      if (DrawCrumbs(dl, "Play Online Co-op", coop_page_ != coop::CoopPage::Hub ? CoopPageName(coop_page_) : nullptr,
                     /*slash=*/true))
        CoopGo(coop::CoopPage::Hub);
      return;
    }
    if (DrawCrumbs(dl, "Play", play_ != PlayView::kMenu ? PlayViewName(play_) : nullptr)) play_ = PlayView::kMenu;
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
    if (play_ == PlayView::kArtbook) return DrawArtbook(dl);  // Beta 7: its own page (artbook.h)
    if (play_ == PlayView::kManual) return DrawManual(dl);    // task 333: the artbook's page for the manual
    if (play_ == PlayView::kCoop) return DrawCoopPage(dl);    // C81
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
    if (!PageInputBlocked() && play_ == PlayView::kLoad && SlotUsed(row)) {
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
    if (!PageInputBlocked() && play_ == PlayView::kChapters && input_.Pressed(Action::kResetPage)) {
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
                                       !PageInputBlocked());
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
      case PlayView::kExtras: {  // a bonus row plays it (the Artbook row opens it)
        std::vector<ui::Prompt> p;
        if (focused_ && focused_->type == ItemType::kAction && focused_->on_activate) p.push_back({Action::kAccept, "Select"});
        p.push_back({Action::kBack, "Back"});
        return p;
      }
      case PlayView::kArtbook:
        if (artbook_) return artbook_->Prompts();
        return {{Action::kBack, "Back"}};
      case PlayView::kManual:
        if (manual_) return manual_->Prompts();
        return {{Action::kBack, "Back"}};
      case PlayView::kCoop:
        return CoopPrompts();
      default:
        return {{Action::kAccept, "Select"}, {Action::kBack, "Back"}};
    }
  }

  void HandlePlayInput() {
    ui::Input& in = input_;
    if (play_ == PlayView::kArtbook || play_ == PlayView::kManual) {  // LB / RB are its previous / next; B leaves the
      in.Consume(Action::kPlay);                                       // viewer, then the page
      if (in.Pressed(Action::kBack)) {
        in.Consume(Action::kBack);
        ui::Cue(ui::Sound::kBack);
        Artbook* book = play_ == PlayView::kManual ? manual_.get() : artbook_.get();
        if (!(book && book->Back())) play_ = PlayView::kExtras;  // on its Extras row again
      }
      return;
    }
    if (play_ == PlayView::kCoop) {  // C81: the text editor takes B and Start itself
      if (social_.editing() || coop_gate_.open()) return;  // UI1: the early access warning takes its own input
      in.Consume(Action::kPlay);  // LB and RB stay for the page (Join: switch between its columns)
      if (in.Pressed(Action::kBack)) {
        in.Consume(Action::kBack);
        ui::Cue(ui::Sound::kBack);
        CoopBack();
      }
      return;
    }
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

  // ----------------------------------------------- Remastered countdown ---
  // Task 373 (Beta 17, ported from stable 1.9.12): a card at the top right of Home counting down to the King Kong Remastered
  // update (countdown.h), with the Discord invite; at zero it says the update
  // is out and offers Check for updates. Right from the menu (or the mouse)
  // moves to its buttons; Left and Right between them; Down to the side
  // panels; Left from the first or B back to the menu.
  struct CardButton {
    std::string label;
    bool enabled;
    bool discord;  // shows the Discord mark
    std::function<void()> action;
  };

  bool LaunchIsOut() const { return countdown::NowUtc() >= launch_utc_; }

  std::vector<CardButton> CardButtons() {
    std::vector<CardButton> b;
    if (LaunchIsOut()) {
      if (update_ && update_->busy && !installing_update_)
        b.push_back({T("Checking for updates"), false, false, nullptr});
      else
        b.push_back({T("Check for updates"), true, false, [this] {
                       if (update_ && update_->found) OpenUpdatePrompt();
                       else StartUpdateCheck(true);  // the normal check; it offers the update when found
                     }});
    }
    b.push_back({T("Join the Discord"), true, true, [] { OpenUrl(kDiscordUrl); }});
    return b;
  }

  // The launch time: the last one the website gave (kept in the settings), else
  // the built-in one; the website's launch.json is fetched in the background.
  void StartLaunchTime(bool fetch) {
    launch_utc_ = *countdown::ParseUtc(countdown::kBuiltInUtc);
    if (const auto cached = countdown::ParseUtc(Get("kk_launch_utc"))) {
      launch_utc_ = *cached;
      launch_title_ = Get("kk_launch_title");
    }
#if defined(KK_DEV_TOOLS)
    // Developer aids: KK_DEV_LAUNCH_JSON='{"utc": "...", "title": "..."}' stands in
    // for the website's file (applied and cached like it); KK_DEV_COUNTDOWN=<seconds>
    // puts the launch that far from now (0 or less: already out), not cached.
    if (const char* j = std::getenv("KK_DEV_LAUNCH_JSON"); j && *j) {
      launch_fetch_ = std::make_shared<countdown::Fetch>();
      launch_fetch_->info = countdown::ParseLaunchJson(j);
      launch_fetch_->done = true;
      fetch = false;
    }
    if (const char* v = std::getenv("KK_DEV_COUNTDOWN"); v && *v) {
      dev_countdown_ = true;
      launch_utc_ = countdown::NowUtc() + std::atoll(v);
      fetch = false;
    }
#endif
    if (fetch) launch_fetch_ = countdown::StartFetch();
  }

  void TickLaunchTime() {
    if (!launch_fetch_ || !launch_fetch_->done) return;
    const std::optional<countdown::LaunchInfo> info = launch_fetch_->info;
    launch_fetch_.reset();
    if (!info) {
      REXLOG_INFO("KK: launch.json not available; countdown to {}", launch_utc_);
      return;
    }
    if (!dev_countdown_) {
      launch_utc_ = info->utc;
      launch_title_ = info->title;
    }
    REXLOG_INFO("KK: launch.json: {} \"{}\"", info->utc_text, info->title);
    Set("kk_launch_utc", info->utc_text);  // saved with the other settings a moment later
    Set("kk_launch_title", info->title);
  }

  // Text with every digit in a cell of the same width, so a ticking clock
  // doesn't shift sideways. Returns the width (draw = false only measures).
  float DrawSteadyDigits(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const std::string& text,
                         bool draw = true) {
    float cell = 0;
    for (char c = '0'; c <= '9'; ++c) cell = std::max(cell, ui::TextSize(font, size, std::string(1, c)).x);
    float x = pos.x;
    for (const char c : text) {
      const std::string one(1, c);
      const float w = ui::TextSize(font, size, one).x;
      const bool digit = c >= '0' && c <= '9';
      if (draw) dl->AddText(font, size, ImVec2(std::round(x + (digit ? (cell - w) * 0.5f : 0.0f)), pos.y), col, one.c_str());
      x += digit ? cell : w;
    }
    return x - pos.x;
  }

  void DrawCountdown(ImDrawList* dl, float alpha, const std::vector<HomeEntry>& entries, int focus) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const bool out = LaunchIsOut();
    const std::vector<CardButton> buttons = CardButtons();
    const int n = int(buttons.size());
    if (card_focus_ >= n) card_focus_ = n - 1;

    // Controller and keyboard.
    auto use = [&](int i) {
      const CardButton& b = buttons[size_t(i)];
      ui::Cue(b.enabled ? ui::Sound::kSelect : ui::Sound::kDeny);
      if (b.enabled && b.action) b.action();
    };
    if (!modal_.open) {
      const bool right_free =
          entries.empty() || !entries[size_t(std::clamp(focus, 0, int(entries.size()) - 1))].change;
      if (card_focus_ < 0 && side_focus_ < 0 && right_free && input_.Pressed(Action::kRight)) {
        input_.Consume(Action::kRight);
        card_focus_ = 0;
        ui::Cue(ui::Sound::kMove);
      } else if (card_focus_ >= 0) {
        if (input_.Pressed(Action::kLeft)) {
          input_.Consume(Action::kLeft);
          card_focus_ -= 1;  // from the first: back to the menu
          ui::Cue(ui::Sound::kMove);
        } else if (input_.Pressed(Action::kRight)) {
          input_.Consume(Action::kRight);
          if (card_focus_ < n - 1) {
            ++card_focus_;
            ui::Cue(ui::Sound::kMove);
          }
        } else if (input_.Pressed(Action::kDown)) {
          input_.Consume(Action::kDown);
          card_focus_ = -1;
          side_focus_ = 0;  // the side panels below
          ui::Cue(ui::Sound::kMove);
        } else if (input_.Pressed(Action::kUp)) {
          input_.Consume(Action::kUp);
        } else if (input_.Pressed(Action::kAccept)) {
          input_.Consume(Action::kAccept);
          use(card_focus_);
        }
      }
    }

    // Layout: right-aligned under the tabs, clear of the logo and menu on the left.
    const float cw = 404 * s, pad = 24 * s, inner = cw - 2 * pad;
    const float x1 = o_.x + w_ - Margin(), x0 = x1 - cw, y0 = o_.y + 104 * s;
    const std::string title = launch_title_.empty() ? countdown::kBuiltInTitle : launch_title_;
    const std::string caption = Upper(T(title));
    const float cap_fs = 13 * s, cap_tr = 2.4f * s;
    const float big_fs = 50 * s;
    const std::string out_text = title == countdown::kBuiltInTitle ? T("The King Kong Remastered update is out")
                                                                   : F("{0} is out", {T(title)});
    const float out_fs = 24 * s;
    const std::string date = countdown::DateLine(launch_utc_);
    const float date_fs = 15 * s;
    const std::string invite =
        T("Join our Discord to follow development, get behind the scenes previews and have your say on new features.");
    const float inv_fs = 14.5f * s;
    const float bh = 38 * s, b_fs = 15.5f * s, b_pad = 18 * s, icon = 18 * s, b_gap = 10 * s;

    float h = pad + cap_fs * 1.2f + 10 * s;
    const float headline_h = out ? ui::TextSize(f.semibold, out_fs, out_text, inner).y : big_fs * 1.05f;
    h += headline_h + 8 * s;
    const float date_h = out ? 0.0f : ui::TextSize(f.text, date_fs, date, inner).y;
    if (!out) h += date_h + 16 * s;
    else h += 10 * s;
    const float inv_h = ui::TextSize(f.text, inv_fs, invite, inner).y;
    h += inv_h + 18 * s + bh + pad;

    // The buttons' places (needed for the mouse before drawing).
    std::vector<std::pair<ImVec2, ImVec2>> rects;
    {
      float bx = x0 + pad;
      const float by = y0 + h - pad - bh;
      for (const CardButton& b : buttons) {
        const float bw = b_pad * 2 + ui::TextSize(f.semibold, b_fs, b.label).x + (b.discord ? icon + 10 * s : 0.0f);
        rects.push_back({ImVec2(bx, by), ImVec2(bx + bw, by + bh)});
        bx += bw + b_gap;
      }
    }
    int hovered = -1;
    for (int i = 0; i < n && !modal_.open; ++i)
      if (ImGui::IsMouseHoveringRect(rects[size_t(i)].first, rects[size_t(i)].second, false)) hovered = i;
    if (input_.mouse_moved()) {
      card_focus_ = hovered;
      if (hovered >= 0) side_focus_ = -1;
    }
    if (hovered >= 0 && ImGui::IsMouseClicked(0)) use(hovered);
#if defined(KK_DEV_TOOLS)
    static int logged_focus = -1;
    if (card_focus_ != logged_focus) REXLOG_INFO("KK dev: countdown focus {}", card_focus_);
    logged_focus = card_focus_;
#endif

    // The card, in the side panels' style, with a brass edge.
    const ImVec2 p0(x0, y0), p1(x1, y0 + h);
    const float r = 10 * s;
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 6 * s), ImVec2(p1.x, p1.y + 6 * s),
                      ui::WithAlpha(IM_COL32(0, 0, 0, 255), 0.2f * alpha), r);
    dl->AddRectFilled(p0, p1, ui::WithAlpha(IM_COL32(16, 18, 22, 255), 0.72f * alpha), r);
    dl->AddRect(p0, p1, ui::WithAlpha(color::kText, 0.10f * alpha), r, 0, 1.0f * s);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + r), ImVec2(p0.x + 3 * s, p1.y - r), ui::WithAlpha(color::kAccent, alpha));

    float y = y0 + pad;
    const float x = x0 + pad;
    float tr = cap_tr;
    std::string cap = caption;
    if (ui::TrackedWidth(f.display, cap_fs, cap, tr) > inner) {  // a long title from the website
      tr = 0;
      cap = ui::Ellipsize(f.display, cap_fs, cap, inner);
    }
    ui::DrawTracked(dl, f.display, cap_fs, ImVec2(x, y), ui::WithAlpha(color::kAccent, alpha), cap, tr);
    y += cap_fs * 1.2f + 10 * s;
    if (out) {
      dl->AddText(f.semibold, out_fs, ImVec2(x, y), ui::WithAlpha(color::kText, alpha), out_text.c_str(), nullptr,
                  inner);
      y += headline_h + 18 * s;
    } else {
      const std::string clock = countdown::CountdownText(launch_utc_ - countdown::NowUtc());
      DrawSteadyDigits(dl, f.display, big_fs, ImVec2(x - 2 * s, y), ui::WithAlpha(color::kText, alpha), clock);
      y += headline_h + 8 * s;
      dl->AddText(f.text, date_fs, ImVec2(x, y), ui::WithAlpha(color::kTextDim, alpha), date.c_str(), nullptr, inner);
      y += date_h + 16 * s;
    }
    dl->AddText(f.text, inv_fs, ImVec2(x, y), ui::WithAlpha(color::kTextDim, alpha), invite.c_str(), nullptr, inner);

    // The buttons, as in the pop-ups: the focused one filled.
    for (int i = 0; i < n; ++i) {
      const CardButton& b = buttons[size_t(i)];
      const ImVec2 a = rects[size_t(i)].first, c = rects[size_t(i)].second;
      const bool on = card_focus_ == i, hot = hovered == i;
      if (on) dl->AddRectFilled(a, c, ui::WithAlpha(color::kFocus, alpha));
      else dl->AddRect(a, c, ui::WithAlpha(color::kText, (hot ? 0.45f : 0.22f) * alpha));
      const ImU32 tc = !b.enabled ? (on ? color::kOnFocusDim : color::kTextFaint) : on ? color::kOnFocus : color::kText;
      float lx = a.x + b_pad;
      if (b.discord) {
        if (auto* t = BrandIcon(discord_icon_, kDiscordIcon, kDiscordIconSize)) {
          const float iy = a.y + (bh - icon) * 0.5f;
          dl->AddImage(ui::Tex(t), ImVec2(lx, iy), ImVec2(lx + icon, iy + icon), ImVec2(0, 0), ImVec2(1, 1),
                       ui::WithAlpha(on ? color::kOnFocus : IM_COL32(255, 255, 255, 255), alpha));
        }
        lx += icon + 10 * s;
      }
      const ImVec2 ls = ui::TextSize(f.semibold, b_fs, b.label);
      dl->AddText(f.semibold, b_fs, ImVec2(std::round(lx), std::round(a.y + (bh - ls.y) * 0.5f)),
                  ui::WithAlpha(tc, alpha), b.label.c_str());
    }
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
    if (side_focus_ >= 0 || card_focus_ >= 0) status_focus_ = false;
    HomeStatusInput(int(entries.size()));
    menu_to_status_ = ShaderStatusSelectable();
    if (const int i = DrawMenu(dl, entries, home_focus_, x, y, a, item_h); i >= 0) entries[size_t(i)].action();
    menu_to_status_ = false;  // only Home's menu
    DrawHomeStatus(dl, x, o_.y + h_ - 120 * s, a);
    DrawContentStrip(dl, a, false);  // a download running in the background
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
    if (PageInputBlocked() || side_focus_ >= 0 || card_focus_ >= 0) return;
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
    return !files_ok_ || installing_ || setup_art_ || setup_shaders_ || setup_textures_ || setup_cutscenes_ ||
           setup_graphics_ || setup_prompts_ || setup_all_set_ || update_new_ || UpdateScreen() || update_shaders_ ||
           setup_settings_ || setup_workshop_;
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
  // artwork, before the texture pack page) and after every update.
  bool ShaderScreen() const {
    if (UpdateScreen() || installing_ || setup_art_) return false;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) return dev_setup_ == 4;
#endif
    return setup_shaders_ || update_shaders_ || rebuild_shaders_screen_;
  }
  // The setup's step and the plain setup screens draw the night sky; the shader screen the stills.
  // Owner, 17 Oct: only the pages before the disc image is given (and the install and artwork that follow it)
  // go without the game's artwork; every setup page after them (the shader screen, the texture pack, the PS2
  // cutscenes, the graphics pages and the rest, the New in this update screen) uses the launcher's own look: the game's stills, its logo, the same panels.
  bool PlainSetup() const { return InSetup() && !ShaderScreen() && !AssetSetup(); }
  bool AssetSetup() const {
    if (!InSetup() || slides_.empty()) return false;
    const int stage = SetupStage();
    return AskingSetupStage(stage);
  }

  // Home's look (the stills, blurred; the game's logo; the music), with a centred panel: what is
  // happening and why, the PC being slower meanwhile, and the progress. Task 365 (owner, 23 Oct): no Continue in
  // the background and no Skip all any more (first setup, after an update, Rebuild shaders): the screen stays until
  // the shaders are ready, then the setup (or Home, or the Graphics page) comes up by itself (TickNativeShaders).
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
    const float pw = std::min(760 * s, w_ - Margin() * 2), pad = 34 * s, r = 12 * s, wrap = pw - pad * 2;
    const float why_h = ui::TextSize(f.text, 16 * s, why, wrap).y, slow_h = ui::TextSize(f.text, 14.5f * s, slow, wrap).y;
    const float ph = pad + 16 * s + 14 * s + 36 * s + 18 * s + why_h + 10 * s + slow_h + 30 * s + 3 * s + 14 * s +
                     20 * s + pad;
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
    // Task 365: no button on the panel and nothing at the top right (the screen moves on by itself).
  }

  // Task 332: 15 the Workshop page, 16 to 18 the Settings pages Display, Graphics, Controls (launcher_setup_pages.inc);
  // the graphics pages 8 to 11 only come up from the New in this update screen now.
  // 0: asking for the disc image, 1: installing, 2: taking the artwork,
  // 3: updating the port, 4: preparing the native renderer's shaders, 6: the HD texture pack, 7: restore
  // the PS2 cutscenes?, 8 to 11: the graphics pages (setup-flow.md steps 5 to 8), 12: the button prompts
  // (step 9), 13: "You're all set" (step 10), 14: New in this update (players who had the setup on an older
  // build, launcher_setup_new.inc). 5 is not used any more: it was the Original or Remastered step, removed in
  // job J6 (new installs are Remastered; the Original look is a switch on graphics page 4 and in Settings).
  // The graphics pages also come up from the New screen (gfx_from_new_), so they are checked before it.
  int SetupStage() const {
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) return dev_setup_;
#endif
    // Task 332: the first-time setup's pages in the owner's order (each page clears its own flag; Back sets the
    // one before again): shaders, the Settings pages, button prompts, texture pack, PS2 cutscenes, Workshop, all set.
    return UpdateScreen() ? 3 : installing_ ? 1 : setup_art_ ? 2 : setup_shaders_ ? 4
           : setup_settings_ ? kSettingsFirstStage + std::clamp(setup_settings_page_, 0, kSettingsSetupPages - 1)
           : setup_prompts_ ? kPromptsStage : setup_textures_ ? 6 : setup_cutscenes_ ? 7
           : setup_workshop_ ? kWorkshopStage : setup_all_set_ ? kAllSetStage
           : setup_graphics_ ? kGfxFirstStage + std::clamp(gfx_page_, 0, kGfxPages - 1)
           : update_new_ ? kNewStage : 0;
  }

  // The first-time setup after the game files are there (a fresh install, or game files given to a fresh
  // settings file): the shader screen is started by the caller, then these pages in order.
  // New installs are Remastered (owner, 17 Oct, DECISIONS "Setup answers"; 12 Oct "Remastered plays at 60 FPS
  // with Strong GTAO"; 15 Oct "GI follows the presets"): ApplyRemastered gives the hardware recommendation (its
  // preset, and the global illumination of that preset), 60 FPS and Strong GTAO, set now so a setup left half
  // way still plays Remastered. A player who had the setup on an older build (a reinstall, kk_setup_version
  // 'waiting' or a version) keeps the settings they chose. The setup also stands in for the New screen.
  // Task 350a: an existing install that has not had this round of the setup again (setup_again.h) starts it from the
  // new defaults too: its Display, Graphics and Controls settings and button prompts back to their defaults first
  // (ResetForSetupAgain), then Remastered as for a new install. Once started the round counts as had (as a new
  // install's setup does): closing the launcher half way does not reset the settings again.
  void StartFullSetup(bool save = true) {
    if (setup_again::Due(existing_install_, GetInt("kk_setup_again"), std::getenv("KK_AUTOPLAY") != nullptr)) {
      ResetForSetupAgain();
      ApplyRemastered(true);
      ApplyRecommendedPreset();
      REXLOG_INFO("KK: setup again: new defaults (native_backend={} kk_frame_rate={} native_gi={} tier {} preset '{}' "
                  "render {} upscaler {} / {})",
                  FindFlag("native_backend") ? Get("native_backend") : std::string("-"), Get("kk_frame_rate"),
                  FindFlag("native_gi") ? Get("native_gi") : std::string("-"), Get("kk_hardware_tier"),
                  Get("kk_graphics_preset"), Get("kk_render_quality"),
                  FindFlag("native_upscaler") ? Get("native_upscaler") : std::string("-"), Get("present_effect"));
    } else if (Get("kk_setup_version").empty()) {
      ApplyRemastered(true);
      ApplyRecommendedPreset();
      REXLOG_INFO("KK: setup: new install: Remastered defaults (kk_frame_rate={} ao_strength={} native_gi={} tier {})",
                  Get("kk_frame_rate"), Get("ao_strength"), FindFlag("native_gi") ? Get("native_gi") : std::string("-"),
                  Get("kk_hardware_tier"));
    } else {
      REXLOG_INFO("KK: setup: again for a player who had it before (kk_setup_version {}): settings kept",
                  Get("kk_setup_version"));
    }
    Set("kk_setup_version", KK_VERSION);
    SetInt("kk_setup_again", setup_again::kRound);  // a new install's setup counts as this round too
    update_new_ = false;
    gfx_from_new_ = false;
    if (save) SaveSettings(paths_.config_path);  // the constructor saves later (a saved file now would look like an update)
    AskContentInSetup();
  }

  // Task 350a (coordinator, 21 Oct): the first-time setup starts on a named preset, the one the hardware tier
  // recommends (setup_again::RecommendedPreset), applied whole like a choice on the Preset row: every row of it
  // matches and the Preset row shows its name, not Custom. The frame rate (60) and the look stay ApplyRemastered's.
  void ApplyRecommendedPreset() {
    const std::string tier = Get("kk_hardware_tier");
    const bool deck = DetectHardware().steam_deck;
    const std::string_view label = setup_again::RecommendedPreset(tier, deck);
    for (int i = 0; i < kGraphicsPresetCount; ++i)
      if (label == kGraphicsPresets[i].label) ApplyGraphicsPreset(i);
    ++g_changes;
    const int now = MatchGraphicsPreset();
    REXLOG_INFO("KK: setup: starts on the {} preset (tier '{}'{}); the Preset row shows {}", label, tier,
                deck ? ", Steam Deck" : "", now >= 0 ? kGraphicsPresets[now].label : "Custom");
  }

  // Task 350a: the settings the setup's pages show, back to their defaults (setup_again::Resets), and the hardware
  // recommendation a new install gets. Saves, the game files, the texture pack and the PS2 cutscenes, Audio,
  // Gameplay, Cheats and Advanced stay. A startup fallback in force (DirectX 12 could not start here) keeps its API.
  void ResetForSetupAgain() {
    const bool fallback = !startup::Message().empty();
    std::string changed;
    for (auto& e : rex::cvar::GetRegistry()) {
      if (e.type == rex::cvar::FlagType::Command || !setup_again::Resets(e.name, e.category)) continue;
      if (e.source == rex::cvar::Source::kCommandLine || e.source == rex::cvar::Source::kEnvironment) continue;
      if (fallback && e.name == "native_backend") continue;
      if (e.getter() != e.default_value) changed += (changed.empty() ? "" : " ") + e.name + "=" + e.getter();
      rex::cvar::ResetToDefault(e.name);
    }
    REXLOG_INFO("KK: setup again: back to the defaults from: {}{}", changed.empty() ? "(all at their defaults)" : changed,
                fallback ? "; the graphics API kept (a startup fallback is in force)" : "");
    const auto output = PrimaryScreenSize();
    ApplyFirstSetupHardwareRecommendation(output.first, output.second);
    ++g_changes;
    ApplyRendererChoice();
    if (cb_.set_fullscreen) cb_.set_fullscreen(GetBool("fullscreen"));
  }

  // The first-time setup's content steps (setup-flow.md 3 and 4): the cutscene page only where the PS2
  // movies can play (Windows, and Linux builds with FFmpeg); TickSetupContent skips a page whose content is already there.
  // Task 332: every page of the owner's order is set now; SetupStage shows them in that order and each page's
  // Next clears its own (the Settings pages, button prompts, texture pack, PS2 cutscenes, Workshop, all set).
  void AskContentInSetup() {
    setup_settings_ = true;
    setup_settings_page_ = 0;
    setup_prompts_ = true;
    setup_textures_ = true;
    setup_cutscenes_ = movies::PlaybackAvailable();
    setup_workshop_ = true;
    setup_all_set_ = true;
    sub_ = kNoSubPage;
    if (!setup_cutscenes_) REXLOG_INFO("KK: setup: no PS2 cutscene page (PS2 movies do not play on this system)");
  }

  void DrawSetup(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float x = o_.x + Margin();
    float y = o_.y + std::max(84 * s, h_ * 0.12f);
    const int stage = SetupStage();
    if (SettingsSetupStage(stage)) return DrawSetupSettings(dl);  // task 332: the Settings pages (launcher_setup_pages.inc)
    if (stage == kWorkshopStage) return DrawSetupWorkshop(dl);
    if (GraphicsSetupStage(stage)) return DrawSetupGraphics(dl);  // from New in this update: a layout of their own
    if (stage == kPromptsStage) return DrawSetupPrompts(dl);       // setup step 9 (launcher_setup_end.inc)
    if (stage == kAllSetStage) return DrawSetupAllSet(dl);         // setup step 10
    if (stage == kNewStage) return DrawUpdateNew(dl);              // New in this update (launcher_setup_new.inc)
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

    if (AssetSetup() && logo_) {
      // After the disc image the game's own artwork is there: its logo over its stills, as on Home.
      const float lh = 104 * s, lw = lh * logo_aspect_;
      dl->AddImage(ui::Tex(logo_), ImVec2(x - 8 * s, y - 14 * s), ImVec2(x - 8 * s + lw, y - 14 * s + lh));
      y += lh - 4 * s;
      ui::DrawTracked(dl, f.display, 13 * s, ImVec2(x + 2 * s, y), color::kTextDim,
                      Upper(T("PC port")) + "  " KK_DOT "  " + Upper(T("Setup")), 4 * s);
      y += 44 * s;
    } else {
    ui::DrawTracked(dl, f.display, 15 * s, ImVec2(x + 2 * s, y), color::kAccent, "PETER JACKSON'S", 6 * s);
    y += 20 * s;
    ui::DrawTracked(dl, f.display, 66 * s, ImVec2(x - 1 * s, y), color::kText, "KING KONG", 2 * s);
    y += 70 * s;
    ui::DrawTracked(dl, f.display, 13 * s, ImVec2(x + 2 * s, y), color::kTextFaint,
                    Upper(T("PC port")) + "  " KK_DOT "  " + Upper(T(stage == 3 ? "Update" : "Setup")), 4 * s);
    y += 62 * s;
    }

    // The steps (of the first install).
    static const char* const kSteps[] = {"Your copy", "Install", "Play"};
    if (stage != 3) {
    const int step = stage == 0 ? 0 : 1;
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
    } else if (stage == 6 || stage == 7) {
      // Setup steps 7 and 8 (setup-flow.md, task 332): the HD texture pack, and the restored PS2 cutscenes.
      DrawSetupTop(dl);
      const auto kind = stage == 6 ? content::Kind::kTexturePack : content::Kind::kCutscenes;
      const content::Info& info = content::InfoFor(kind);
      auto para = [&](const std::string& text, float size, ImU32 col) {
        dl->AddText(f.text, size * s, ImVec2(x, y), col, text.c_str(), nullptr, col_w);
        y += ui::TextSize(f.text, size * s, text, col_w).y + 18 * s;
      };
      if (stage == 6) {
        heading("HD texture pack");
        para(F("Sharper versions of the game's textures, made at four times their original size. It is a separate "
               "download of {0} GB and takes {1} GB once installed. You can turn it on or off at any time in "
               "Settings > Graphics.",
               {Gb(info.zip_size), Gb(info.unpacked)}),
             16, color::kTextDim);
      } else {
        heading("Restore the PS2 cutscenes?");
        para(F("The PS2 version of King Kong has cutscenes between some missions that the Xbox 360 version left "
               "out. These are upscaled versions of them, and the game plays them where they belong. It is a "
               "separate download of {0} GB. You can turn them off at any time in Settings > Advanced.",
               {Gb(info.zip_size)}),
             16, color::kTextDim);
      }
      const auto have = content::Service::Get().Get(kind);
      const bool already = have.installed, running = !already && (have.Busy() || have.stage == content::Stage::kPaused);
      if (already || running) {
        para(already ? (stage == 6 ? T("Already installed. Turn it on or off at any time in the Workshop or Settings > Graphics.")
                                   : T("Already installed. Turn them on or off at any time in Settings > Advanced."))
                     : T("Already downloading. It carries on in the background."),
             15.5f, color::kGood);
        DrawContentCard(dl, kind);
        y += 8 * s;
        entries.push_back({"Continue", true, [this, stage, already] {
                             FinishSetupContentStep(stage, already ? "already installed: continue" : "already downloading: continue");
                           }});
      } else
        para(T("It downloads in the background: the setup goes on, and you can use the launcher meanwhile."), 14.5f,
             color::kTextFaint);
      if (!already && !running) {
        DrawContentCard(dl, kind);
        y += 8 * s;
      }
      if (already || running) {
      } else if (stage == 6) {
        entries.push_back({"Download HD texture pack", true, [this] { StartContentDownload(content::Kind::kTexturePack, 6); }});
        entries.push_back({"I already have a texture pack", true, [this] { ChooseImportZip(content::ImportTarget::kTexturePack, 6); }});
        entries.push_back({"Skip", true, [this] { FinishSetupContentStep(6, "skipped"); }});
      } else {
        entries.push_back({"Yes, restore them", true, [this] { StartContentDownload(content::Kind::kCutscenes, 7); }});
        entries.push_back({"No thanks", true, [this] { FinishSetupContentStep(7, "no thanks"); }});
      }
    } else {
      float got = 0, size = 0;
      std::string version;
      int phase = 0;
      const bool files = FinishScreen();  // task 370: the first start check's download
      if (files) {
        got = float(finish_->bytes.load());
        size = float(finish_->total.load());
        phase = finish_->phase.load();
        version = KK_VERSION;
      } else if (update_) {
        got = float(update_->bytes.load());
        size = float(update_->total.load());
        phase = update_->phase.load();
        if (update_->found) version = update_->found->version;
      }
      bool finishing = !files && update_ && !update_->busy && update_->done;
#if defined(KK_DEV_TOOLS)
      if (dev_setup_ >= 0) version = "9.9.9", size = 61.4e6f, got = size * 0.42f, finishing = false;
#endif
      if (files) {
        heading("Finishing the update");
        body(F("Some files of version {0} are missing, so the launcher is downloading them again. Your installed "
               "game, saves and settings stay as they are.",
               {version}));
      } else {
        heading(finishing ? "Restarting" : "Updating");
        body(F("Downloading version {0} of the port. The launcher restarts by itself when it's done; your installed "
               "game, saves and settings stay as they are.",
               {version}));
      }
      bar(finishing ? 1.0f : size > 0 ? got / size : -1.0f);
      auto mb = [](float bytes) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", bytes / 1e6f);
        return std::string(buf);
      };
      // Task 370: below the bar, what the updater is doing once the download is over.
      const bool downloading = phase == int(UpdatePhase::kDownloading) && !finishing;
      const char* step = phase == int(UpdatePhase::kInstalling) || finishing ? "Installing the files." : "Checking the download.";
      caption_pair(!downloading ? T(std::string(step))
                   : size > 0   ? F("{0} of {1} MB", {mb(got), mb(size)})
                                : T(std::string("Connecting")),
                   downloading && size > 0 ? std::to_string(int(got / size * 100)) + "%" : "");
      if (files) entries.push_back({"Continue in the background", true, [this] { finish_hidden_ = true; }});
    }
    if (stage == 0) DrawSidePanels(dl, 1.0f, false, entries, setup_focus_, o_.y + h_ - 120 * s);
    else side_focus_ = -1;
    // The setup steps after the shaders show a download running in the background (setup-flow.md).
    if (stage == 6 || stage == 7) DrawContentStrip(dl, 1.0f, false);
    if (const int i = DrawMenu(dl, entries, setup_focus_, x, y, 1.0f); i >= 0) entries[size_t(i)].action();
    if (!PageInputBlocked() && input_.Pressed(Action::kBack)) {
      input_.Consume(Action::kBack);
      if (stage <= 1) ui::Cue(ui::Sound::kBack);
      if (stage == 0) OpenQuit();
      else if (stage == 1) progress_.cancel = true;
    }
  }

  static ImU32 WithAlphaText(float a) { return ui::WithAlpha(color::kText, a); }

#include "launcher_setup_graphics.inc"
#include "launcher_setup_end.inc"
#include "launcher_setup_new.inc"
#include "launcher_setup_pages.inc"
#include "launcher_coop_pages.inc"

  // The status line once the setup is over.
  const char* InstalledMessage() {
    return NativeShaderPack::Wanted() && native_pack_.Finished() && !ShadersPreparedForThisVersion()
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
        "Shows how many players are online, at the top right of the launcher. Help reduce stutter: share "
        "graphics pipeline data. It sends a tiny anonymous check in every 5 minutes (a random number, the "
        "port's version, Windows or Linux, and whether you're playing) and, when you quit, the game's graphics "
        "pipeline data and an anonymous performance summary. After a crash, it sends the crash report with "
        "your user name and personal folders removed. Nothing that identifies you or your PC is kept. Switch "
        "it off with Online players on the left, or on the About page, at any time.");
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
      cols[1] = {Upper(T("Shaders")), native_pack_.StatusText(ShadersPreparedForThisVersion()),
                 native_pack_.Status(ShadersPreparedForThisVersion()) == ShaderHomeStatus::kFailed ? color::kWarn
                                                                                                  : color::kText,
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
        const bool hovered = !PageInputBlocked() && ImGui::IsMouseHoveringRect(p0, p1, false);
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
          "Each crash saves a text report and a minidump on this PC. With Online player count on, the text report is "
          "sent, with your user name and personal folders removed. The minidump stays on this PC; it may contain "
          "private process memory, so review it before sharing.",
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

  // Play > Extras (owner, 18 Oct): the Artbook first, then the game's own bonuses. Every bonus is unlocked, so
  // there is no save to choose and no save is read to decide; a bonus starts with the save Chapter select
  // would use (ChapterSlot: the last played, or none), as before.
  std::vector<Item> ExtrasItems() {
    std::vector<Item> items;
    const int slot = ChapterSlot();
    // Beta 7: the Weta Artbook from the PS2 bonus disc (artbook.h), downloaded quietly in the background.
    items.push_back(Section("Bonus disc"));
    {
      auto& service = content::Service::Get();
      const content::Status st = service.Ready() ? service.Get(content::Kind::kArtbook) : content::Status{};
      Item it;
      it.type = ItemType::kAction;
      it.label = "Artbook";
      it.value_text = st.installed ? F("{0} paintings", {std::to_string(artbook::kPaintingCount)})
                      : st.Busy()  ? T("Downloading")
                                   : T("Not downloaded");
      it.value_color = st.installed ? color::kAccent : color::kTextFaint;
      it.description = T("Concept paintings by Weta from the game's bonus disc. Look at them up close and save "
                         "them to your PC.");
      it.on_activate = [this] { OpenPlayView(PlayView::kArtbook); };
      items.push_back(std::move(it));
    }
    // Task 333 (owner, 20 Oct, Beta 13): the game's own user manual and its original main menu.
    items.push_back(Section("Original game"));
    {
      auto& service = content::Service::Get();
      const content::Status st = service.Ready() ? service.Get(content::Kind::kManual) : content::Status{};
      Item it;
      it.type = ItemType::kAction;
      it.label = "User manual";
      it.value_text = st.installed ? T("Ready") : st.Busy() ? T("Downloading") : T("Not downloaded");
      it.value_color = st.installed ? color::kAccent : color::kTextFaint;
      it.description = T("The game's Xbox 360 manual, page by page.");
      it.on_activate = [this] { OpenPlayView(PlayView::kManual); };
      items.push_back(std::move(it));
    }
    {
      Item it;
      it.type = ItemType::kAction;
      it.label = "Original main menu";
      it.description = T("The game's own main menu, as it was on the Xbox 360. You can start a game from it.");
      it.on_activate = [this] { AskOriginalMenu(); };
      items.push_back(std::move(it));
    }
    items.push_back(Section("Extras"));
    for (const auto& extra : play::kExtras) {
      const int id = extra.id;
      if (id == 1) continue;  // the game's Old movie filter: not offered (owner, 18 Oct, task 213)
      Item it;
      it.type = ItemType::kAction;
      it.label = MenuText(0x4902EC04 + id);
      if (it.label.empty()) it.label = F("Bonus {0}", {std::to_string(id + 1)});
      it.value_text = "Unlocked";
      it.description = T("Play this bonus, then return to Extras.");
      it.value_color = color::kAccent;
      if (extra.type == play::ExtraType::kMode) {
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
      } else {
        it.on_activate = [this, slot, id] { PlayWith({play::StartKind::kExtra, slot, id, ""}); };
      }
      items.push_back(std::move(it));
    }
    return items;
  }

  // ------------------------------------------------------------ Workshop ---
  // Task 209 (owner, 18 Oct): one page with three rows of cards (workshop.h): texture packs, mods, other TRG
  // projects and games. The launcher gives it its pop-ups, file dialogs, downloads and stills (WorkshopHost).
  // It is made when the launcher starts (StartContent), so the texture pack rule and Settings > Graphics have the
  // mods scan (done on its worker) from the start.
  void EnsureWorkshop() {
    if (workshop_) return;
    workshop_ = std::make_unique<Workshop>(immediate_, paths_.user_dir, paths_.config_path);
    WorkshopHost h;
    h.download = [this](content::Kind kind) { StartContentDownload(kind, 0); };
    h.progress = [this](content::Kind kind) { OpenContentProgress(kind, 0); };
    h.remove = [this](content::Kind kind) { ConfirmContentRemove(kind); };
    h.import_zip = [this](content::ImportTarget target) { ChooseImportZip(target, 0); };
    h.choose_folder = [] { ChooseTexturePackFolder(); };
    h.ask = [this](const std::string& title, const std::string& body, std::vector<std::string> buttons, int cancel,
                   std::function<void(int)> then) { OpenModal(title, body, std::move(buttons), cancel, std::move(then), cancel); };
    h.status = [this](const std::string& line) { Status(line); };
    h.open_url = [](const std::string& url) { OpenUrl(url); };
    h.still = [this](int i, float* aspect) -> rex::ui::ImmediateTexture* {
      if (slides_.empty()) return nullptr;
      const Slide& sl = slides_[size_t(i) % slides_.size()];
      *aspect = sl.aspect;
      return sl.sharp;
    };
    h.save = [] { ++g_changes; };
    h.art = [this](const std::string& key, float* aspect) { return WorkshopArt(key, aspect); };
    h.texture = [this](int w, int h, const std::vector<uint8_t>& rgba) -> rex::ui::ImmediateTexture* {
      art::Image img;
      img.width = w;
      img.height = h;
      img.rgba = rgba;
      return MakeTexture(img);
    };
    workshop_->SetHost(std::move(h));
    texture_pack_choices_ = [this] { return workshop_->TexturePackChoices(); };
    texture_pack_active_ = [this] { return workshop_->ActiveTexturePack(); };
    texture_pack_enable_ = [this](const std::string& key) { workshop_->EnableTexturePack(key); };
    open_workshop_ = [this] { GoTo(kWorkshop); };
  }

  // Task 221: the Workshop's card pictures made from a still of the player's own game (workshop_art.h): the HD
  // texture pack (original and sharper, side by side) and the Cel shading mod (its outlines and bands). Made once
  // (a few ms each) and kept until the art is loaded again.
  std::map<std::string, std::pair<rex::ui::ImmediateTexture*, float>> workshop_art_;
  rex::ui::ImmediateTexture* WorkshopArt(const std::string& key, float* aspect) {
    if (auto it = workshop_art_.find(key); it != workshop_art_.end()) {
      *aspect = it->second.second;
      return it->second.first;
    }
    const std::vector<art::Image>& stills = SetupStills();
    if (stills.empty()) return nullptr;  // asked again later (the art may still be on its way)
    const auto t0 = std::chrono::steady_clock::now();
    const bool cel = key == "builtin:cel-shading";
    const art::Image still = compare::Fit(stills[(cel ? 2 : 1) % stills.size()], 768, 432);
    art::Image img;
    if (key == "hd") img = workshop_art::HdSplit(still);
    else if (cel) img = workshop_art::CelShade(still);
    rex::ui::ImmediateTexture* tex = img ? MakeTexture(img) : nullptr;
    workshop_art_[key] = {tex, 16.0f / 9.0f};
    *aspect = 16.0f / 9.0f;
    REXLOG_INFO("KK workshop: picture {} made from a still of the game ({} ms)", key,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    return tex;
  }

  void DrawWorkshopHeader(ImDrawList* dl) {
    const std::string sub = workshop_ ? workshop_->DetailTitle() : std::string();
    if (DrawCrumbs(dl, "Workshop", sub.empty() ? nullptr : sub.c_str()))
      while (workshop_ && workshop_->Back()) {}
  }

  // Job J8: the Workshop page (workshop.cpp), between the top bar and the prompts.
  void DrawWorkshop(ImDrawList* dl) {
    EnsureWorkshop();
    const float s = S();
    workshop_->Draw(dl, ImVec2(o_.x + Margin(), o_.y + 96 * s), ImVec2(o_.x + w_ - Margin(), o_.y + h_ - 96 * s), m_,
                    input_, !PageInputBlocked());
    focused_ = nullptr;
  }

  // Task 333 (owner, 20 Oct): Extras > Original main menu. A pop-up first; Continue starts the game on its own start
  // menu (play_session.cpp, StartKind::kOriginalMenu), once: the next launcher start is this menu again.
  void AskOriginalMenu() {
    OpenModal("Original main menu", "To come back here, start a game from the original main menu, then pause and quit.",
              {"Continue", "Back"}, 1, [this](int i) {
                if (i != 0) return;
                REXLOG_INFO("KK: Extras > Original main menu: Continue");
                PlayWith({play::StartKind::kOriginalMenu, -1, -1, ""});
              });
  }

  // Task 333: Play > Extras > User manual (artbook.cpp, Book::kManual); its viewer covers the whole window.
  void DrawManual(ImDrawList* dl) {
    if (!manual_) manual_ = std::make_unique<Artbook>(immediate_, paths_.user_dir, Artbook::Book::kManual);
    const float s = S();
    manual_->Draw(dl, ImVec2(o_.x + Margin(), o_.y + 96 * s), ImVec2(o_.x + w_ - Margin(), o_.y + h_ - 96 * s), o_,
                  ImVec2(o_.x + w_, o_.y + h_), m_, input_, !modal_.open);
    focused_ = nullptr;
  }

  // Beta 7: Play > Extras > Artbook (artbook.cpp); its viewer covers the whole window.
  void DrawArtbook(ImDrawList* dl) {
    if (!artbook_) artbook_ = std::make_unique<Artbook>(immediate_, paths_.user_dir);
    const float s = S();
    artbook_->Draw(dl, ImVec2(o_.x + Margin(), o_.y + 96 * s), ImVec2(o_.x + w_ - Margin(), o_.y + h_ - 96 * s), o_,
                   ImVec2(o_.x + w_, o_.y + h_), m_, input_, !PageInputBlocked());
    focused_ = nullptr;
  }

  // A Settings page's rows (and Controls' sub-pages), as Settings shows them. Task 332: the first-time setup's
  // Display, Graphics and Controls pages are built here too (launcher_setup_pages.inc), so they are always the same.
  std::vector<Item> SettingsPageItems(Page page) {
    switch (page) {
      case kDisplay: return DisplayItems();
      case kGraphics: return GraphicsItems();
      case kGameplay: return GameplayItems();
      case kCheats: return CheatItems();
      case kAudio: return AudioItems();
      case kControls: return sub_ == kRemapPage ? RemapItems() : sub_ == kKeysPage ? KeyItems() : ControlsItems();
      case kAdvanced: return AdvancedPageItems();
      case kAchievements: return AchievementItems();
      case kAbout: return AboutItems();
      default: return {};
    }
  }

  void DrawSettings(ImDrawList* dl) {
    const float s = S();
    std::vector<Item> items = SettingsPageItems(page_);
#if defined(KK_DEV_TOOLS)
    if (IsSettingsPage(page_) && sub_ == kNoSubPage) DevCheckLayout(page_ - kFirstSettingsPage, items, "launcher");
#endif
    const float top = o_.y + 104 * s, bottom = o_.y + h_ - 96 * s;
    const float x0 = o_.x + Margin();
    const float avail = w_ - Margin() * 2;
    const float list_w = std::round(avail * 0.56f);
    ui::ListState& st = lists_[page_ * 4 + sub_];
    if (IsSettingsPage(page_) || page_ == kAbout) AttachHelp(items);
    const Item* focused = ui::DrawList(dl, items, st, ImVec2(x0, top), ImVec2(x0 + list_w, bottom), m_, input_,
                                       !PageInputBlocked());
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
        case kCheats: items = CheatItems(); break;
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

  // Task 220: every Settings row's description panel in each language, measured with the real fonts at this
  // window's size: "KK dev: descfit" lines for rows that need smaller text (1, 2), lose the help's second
  // paragraph (3) or still overflow (4), rows without a description or help, then a summary per language.
  void LogDescriptionFit() {
    const SubPage sub = sub_;
    sub_ = kNoSubPage;
    const std::string language = Get("user_language");
    const float s = m_.s;
    const float top = o_.y + 104 * s, bottom = o_.y + h_ - 96 * s;
    const float x0 = o_.x + Margin(), list_w = std::round((w_ - Margin() * 2) * 0.56f);
    const ImVec2 p0(x0 + list_w + 76 * s, top + 18 * s), p1(o_.x + w_ - Margin(), bottom);
    for (const char* lang : {"1", "3", "5", "4", "6"}) {
      Set("user_language", lang);
      int rows = 0, counts[5] = {};
      for (int p = kFirstSettingsPage; p <= kAbout; ++p) {
        std::vector<Item> items;
        switch (Page(p)) {
          case kDisplay: items = DisplayItems(); break;
          case kGraphics: items = GraphicsItems(); break;
          case kAudio: items = AudioItems(); break;
          case kControls: items = ControlsItems(); break;
          case kGameplay: items = GameplayItems(); break;
          case kCheats: items = CheatItems(); break;
          case kAdvanced: items = AdvancedPageItems(); break;
          case kAbout: items = AboutItems(); break;
          default: break;
        }
        AttachHelp(items);
        for (const Item& it : items) {
          if (it.type == ItemType::kSection) continue;
          ++rows;
          const int fit = ui::DescriptionFit(&it, p0, p1, m_);
          ++counts[std::clamp(fit, 0, 4)];
          if (fit > 0) REXLOG_INFO("KK dev: descfit {} {} '{}' fit {}", lang, kPageNames[p], it.label, fit);
          if (it.description.empty()) REXLOG_INFO("KK dev: descfit {} {} '{}' no description", lang, kPageNames[p], it.label);
          if (!it.help && p != kAbout) REXLOG_INFO("KK dev: descfit {} {} '{}' no help", lang, kPageNames[p], it.label);
        }
      }
      REXLOG_INFO("KK dev: descfit {} done {} rows: {} full size, {} smaller, {} without how, {} overflow", lang, rows,
                  counts[0], counts[1] + counts[2], counts[3], counts[4]);
    }
    Set("user_language", language);
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
              "Prepare all of the game's shaders again from the start, even the ones already prepared? This takes a "
              "few minutes. " KK_SLOW_PC, {"Rebuild", "Cancel"},
              1,
              [this](int i) {
                if (i != 0 || native_pack_.Busy()) return;
                if (menu_scene::Active() && cb_.restart_launcher) {
                  // Task 330: the live menu scene's renderer holds the shader pack open: the launcher starts
                  // again without the scene for this time and rebuilds there (--kk_launcher_open=rebuild).
                  REXLOG_INFO("KK: rebuilding the shaders: the launcher starts again without the menu scene");
                  SaveIfChanged();
                  input_.Release();
                  StopHeartbeat();
                  leaving_ = true;
                  modal_.open = false;
                  cb_.restart_launcher(L"--kk_launcher_open=rebuild --kk_menu_scene_off=true");
                  return;
                }
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
    // Task 358: a stable build follows stable only and never offers the beta channel (update_channel.h).
    if (OffersChannelChoice(kBuildChannel)) {
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
        return {"fullscreen", "kk_fullscreen_mode", "kk_picture_size", "window_width", "window_height", "monitor", "kk_frame_rate",
                "d3d12_allow_variable_refresh_rate_and_tearing", "present_letterbox", "kk_brightness", "kk_gamma",
                "kk_performance_metrics"};
      case kGraphics:
        return {"kk_original_look", "kk_modern_settings", "kk_graphics_preset", "native_backend", "kk_renderer",
                "swap_post_effect", "anisotropic_override", "kk_shader_full_cpu",
                "native_texture_pack", "native_texture_pack_folder",
                "present_effect", "kk_render_quality", "resolution_scale", "draw_resolution_scale_x",
                "draw_resolution_scale_y", "native_upscaler", "native_upscaler_quality",
                "kk_motion_blur", "kk_big_blur", "kk_fog", "ao_mode", "ao_strength", "native_ao_method",
                "native_ao_apply", "native_ao_quality", "native_ao_samples", "native_ao_normal_smoothing", "native_gi", "native_gi_distance", "native_shadow_quality", "native_water_reflections", "native_volumetrics", "native_effect_buffers", "native_sharpness",
                "native_cel_shading", "native_cel_outline", "native_cel_darkness", "native_cel_bands", "native_cel_strength"};
      case kGameplay:
        return {"kk_fov", "kk_camera_shake", "kk_camera_shake_strength", "kk_achievement_toasts",
                "kk_achievement_style", "kk_skip_intros", "user_language"};
      case kCheats:
        return {"kk_cheats", "kk_cheat_*"};
      case kAudio:
        return {"#profile", "kk_launcher_music", "kk_launcher_music_volume", "kk_launcher_sounds",
                "kk_launcher_sounds_volume", "kk_achievement_sound", "kk_achievement_sound_file", "kk_achievement_volume"};
      case kControls:
        return {"mnk_mode", "kk_button_prompts", "kk_camera_sensitivity", "kk_camera_modern", "kk_mouse_sensitivity",
                "mnk_mouse", "kk_mouse_invert_y", "kk_invert_rs_x", "kk_invert_rs_y", "kk_invert_ls_x", "kk_invert_ls_y",
                "kk_toggle_aim", "kk_deadzone", "kk_vibration", "kk_vibration_strength", "kk_map_*",
                "@Input/Keybinds/Controller"};
      case kAdvanced:
        return {"kk_notify_downloads", "kk_ps2_cutscenes", "kk_ps2_movie_folder"};
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
#if defined(KK_DEV_TOOLS)
    REXLOG_INFO("KK dev: pop-up opened '{}'", m.title);  // tours: which pop-up took the screen, and when
#endif
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
    // Back to where it was started: Settings' About tab, or Home (king_kong_app.h passes it on).
    const bool from_settings = page_ == kAbout && about_in_settings_;
    rex::cvar::SetFlagByName("kk_benchmark_from", from_settings ? "settings" : "");
    REXLOG_INFO("KK launcher: benchmark started from {}", from_settings ? "Settings > About" : "Home");
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
                "newer than it, this goes back a version.\n\nStable versions before 2.0.0 have no native renderer, so when going "
                "back to one of those, the beta's own "
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
    // A stable version before 2.0.0 has no native renderer: it must find only settings it knows
    // (ResetBetaOnlySettings). Stable 2.0.0 and later have them, so they stay (task 358).
    if (channel == "stable" && update_->found && StableLacksNativeRenderer(update_->found->version))
      ResetBetaOnlySettings();
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

  // Task 370: the first start check runs on its own thread; the update screen (SetupStage 3) shows it only while it
  // downloads files an older updater left out, until Continue in the background. A failure is only logged.
  void StartFinishCheck() {
    finish_ = std::make_shared<FinishStatus>();
    std::thread([s = finish_, file = paths_.user_dir / "update-check.txt"] { FinishUpdate(*s, file); }).detach();
  }
  bool FinishScreen() const { return finish_ && finish_->downloading && !finish_hidden_ && !installing_update_; }
  bool UpdateScreen() const { return installing_update_ || FinishScreen(); }
  void TickFinish() {
    if (!finish_ || finish_reported_ || finish_->busy || !(finish_->done || finish_->failed)) return;
    finish_reported_ = true;
    if (finish_->done && finish_->installed > 0) Status(F("Every file of version {0} is now installed.", {KK_VERSION}));
  }

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
      OpenModal("Update failed", F(update_->message.c_str(), {update_->detail}), {"Close"}, 0, nullptr);
    } else if (update_->done && !relaunched_) {  // start the new version, then close this one
      relaunched_ = true;
      std::wstring extra;
#if defined(KK_DEV_TOOLS)
      // Task 370: a KK_DEV_UPDATE_ZIP test. The new process must not offer it again (nor its steps install it again),
      // and it must not log over this run's log (with the second --log_file it writes logs\king_kong_NNN.log in the
      // program folder, as a run without one does).
      if (const char* zip = std::getenv("KK_DEV_UPDATE_ZIP"); zip && *zip) {
#if defined(_WIN32)
        _putenv_s("KK_DEV_UPDATE_RELAUNCHED", "1");
        _putenv_s("KK_DEV_LAUNCHER_INPUT", "");
#else
        setenv("KK_DEV_UPDATE_RELAUNCHED", "1", 1);
        setenv("KK_DEV_LAUNCHER_INPUT", "", 1);
#endif
        const std::filesystem::path log = rex::cvar::GetFlagByName("log_file");
        // (Windows parses a command line, so the path is quoted; on Linux the argument arrives whole as it is.)
#if defined(_WIN32)
        constexpr const wchar_t* kQuote = L"\"";
#else
        constexpr const wchar_t* kQuote = L"";
#endif
        if (!log.empty())
          extra = L"--log_file=" + std::wstring(kQuote) +
                  (log.parent_path() / (log.stem().wstring() + L"-relaunch" + log.extension().wstring())).wstring() +
                  kQuote;
      }
#endif
      RelaunchSelf(extra);
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
      if (steam::StartedFromSteam()) {
        s->from_steam = true;
      } else if (const auto where = steam::Locate()) {
        s->in_library = steam::HasShortcut(*where);
        s->available = true;
      }
      s->checked = true;
    }).detach();
  }

  void OpenSteamPrompt() {
    if (!steam_ || steam_->busy) return;
    const bool running = steam::SteamRunning();
    OpenModal("Add to Steam",
              running ? "Adds King Kong to your Steam library as a non Steam game, with artwork from SteamGridDB, so "
                        "you can start it from Steam, Big Picture or a Steam Deck.\n\nSteam reads its list of games "
                        "only when it starts, so it closes for a moment and opens again afterwards."
                      : "Adds King Kong to your Steam library as a non Steam game, with artwork from SteamGridDB, so "
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
    // The launcher waited while the picker was open: what the controller or keyboard did in the picker (its
    // confirming press, still held or queued) must not press anything here (Linux, 26 Oct: it opened the picker
    // again behind the install).
    input_.Settle(0.35);
    if (pkg.empty()) return;
    const uint32_t title = iso::ReadTitleId(pkg);
    REXLOG_INFO("KK: install: {} has title {:08X}", pkg.filename().string(), title);
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
      try {
        install_result_ = iso::Extract(pkg, paths_.game_dir, &progress_);
      } catch (const std::exception& e) {
        install_result_ = std::string("The install stopped: ") + e.what();
        REXLOG_ERROR("KK: install stopped: {}", e.what());
      }
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
    StartGame();  // owner, 18 Oct: no frame rate warning pop-up any more
  }

  void Quit() {
    SaveIfChanged();
    input_.Release();
    StopHeartbeat();  // before the app context goes
    if (cb_.quit) cb_.quit();
  }

  // Task 331 (owner, 20 Oct): the owner's loading loops (loading_loop.h). Opening the game: the save notice;
  // back from the game: "Returning to the main menu" with our own lines over it (the save, a tip). It plays from
  // the first frame for at least 3 s and until the live menu scene is up, then fades into the launcher; without a
  // scene the launcher's pictures (or the setup / shader screen) show after the 3 s.
  static constexpr int kTipCount = 12;
  static const char* Tip(int i) {
    static const char* const kTips[kTipCount] = {
        "Press F8 while you play to change graphics settings live.",
        "Press F2 while you play to show the frame rate. Press it again for more details.",
        "Press F9 while you play for a free camera, and F10 to hide the HUD.",
        "Photo mode is in the pause menu: the game stops, and you can move the camera and save a picture.",
        "Load last checkpoint in the pause menu takes you back to your last checkpoint.",
        "Quit from the pause menu or choose Save game, and the game saves your progress.",
        "Chapter select in the Play menu lets you play any chapter again.",
        "Every bonus is unlocked in Play > Extras, with the artbook from the PS2 bonus disc.",
        "The Workshop has texture packs and mods, such as Cel shading for a cartoon look.",
        "Too dark or too bright? Change Brightness and Gamma in Settings > Display.",
        "Benchmark on the main menu shows how well your PC runs the game.",
        "Cheats are in Settings > Cheats, and you can change them in the pause menu while you play.",
    };
    return kTips[std::clamp(i, 0, kTipCount - 1)];
  }

  void StartLoadingLoop(const std::string& open_on) {
    loop_kind_ = loading_loop::KindForStart(open_on);
    const bool saved = REXCVAR_GET(kk_launcher_saved);
    rex::cvar::ResetToDefault("kk_launcher_saved");  // never saved
    if (loop_kind_ == loading_loop::Kind::kNone) {
      REXLOG_INFO("KK loading loop: none (the launcher restarted for a shader rebuild)");
      return;
    }
    loop_ = std::make_unique<LoadingLoop>(immediate_, LoadingLoopFile(loop_kind_));
    if (loop_kind_ != loading_loop::Kind::kReturning) return;
    // Back from a story run that quit from the pause menu with the save written (play_session.cpp).
    loop_saved_ = saved && open_on == "play";
    // A different tip each time: the one after the last shown (remembered in the user data, not the settings).
    const auto file = paths_.user_dir / "loading-tip.txt";
    std::string last;
    {
      std::ifstream in(file, std::ios::binary);
      std::getline(in, last);
    }
    const int tip = loading_loop::NextTip(last, kTipCount);
    std::ofstream(file, std::ios::binary | std::ios::trunc) << tip << "\n";
    loop_tip_ = T(Tip(tip));
    REXLOG_INFO("KK loading loop: back from the game ({}); saved line {}, tip {}", open_on, loop_saved_ ? "shown" : "not shown",
                tip + 1);
  }

  // Task 332 (owner, 20 Oct, DECISIONS row B): the startup movies (startup_movies_logic.h), then the save notice
  // loop until the live menu scene is ready (as at a start from the desktop). `why` is for the log.
  void StartStartupMovies(const char* why) {
    movies_ = std::make_unique<StartupMovies>(immediate_, startup_movies::Files(paths_.game_dir,
                                                                              rex::filesystem::GetExecutableFolder()));
    movies_wait_ = 0.0f;
    REXLOG_INFO("KK startup movies: start ({}; kk_skip_intros {})", why, GetBool("kk_skip_intros") ? "on" : "off");
    // The save notice loop that follows them starts decoding now, so its first picture is ready when they end.
    loop_.reset();
    loop_first_ = -1;
    loop_end_ = -1.0f;
    loop_wait_ = 0.0f;
    loop_shown_ = 0.0f;
    loop_saved_ = false;
    loop_tip_.clear();
    loop_kind_ = loading_loop::Kind::kNotice;
    loop_ = std::make_unique<LoadingLoop>(immediate_, LoadingLoopFile(loop_kind_));
  }

  // After Finish setup or Skip all: the movies always play this once; with kk_skip_intros on, a pop-up first says
  // so. The live menu scene starts behind them (if it can run now; else TickSceneStart starts it later).
  // Owner (after testing Beta 13): the pop-up comes BEFORE any movie is loaded, over the launcher's plain dark
  // background; the movies (and the scene behind them) start only after its OK.
  void StartMoviesAfterSetup() {
    const bool skip = GetBool("kk_skip_intros");
    if (startup_movies::AskFirst(true, skip)) {
      movies_asking_ = true;
      OpenModal("Startup movies",
                T(std::string("The startup movies play this first time. From the next start, your choice to skip them is used.")),
                {"OK"}, -1, [this](int) {
                  REXLOG_INFO("KK startup movies: the first time pop-up closed");
                  movies_asking_ = false;
                  StartStartupMovies("after the setup");
                  if (!menu_scene::Active()) MaybeStartScene(false);
                });
      REXLOG_INFO("KK startup movies: the first time pop-up, before any movie");
    } else {
      StartStartupMovies("after the setup");
      if (!menu_scene::Active()) MaybeStartScene(false);
    }
  }

  // Each frame, before the loading loop: true while the movies cover the whole window (nothing else is drawn or
  // answered then, but their pop-up). The first one waits for the scene's start (it pauses the launcher once),
  // at most 5 s.
  bool DrawStartupMovies(ImDrawList* dl, float dt) {
    if (!movies_) return false;
    const bool scene_starting = loop_scene_ && !menu_scene::Active();
    if (!modal_.open && scene_starting) movies_wait_ += std::clamp(dt, 0.0f, 0.1f);
    const bool go = !modal_.open && (!scene_starting || movies_wait_ > 5.0f);
    rex::ui::ImmediateTexture* frame = movies_->Frame(dt, go);
    const ImVec2 p1(o_.x + w_, o_.y + h_);
    dl->AddRectFilled(o_, p1, IM_COL32(0, 0, 0, 255));
    if (frame) {  // the picture's whole frame, its aspect kept (black bars on other window shapes)
      const float aspect = movies_->Aspect();
      float vw = w_, vh = w_ / aspect;
      if (vh > h_) {
        vh = h_;
        vw = h_ * aspect;
      }
      const ImVec2 v0(std::round(o_.x + (w_ - vw) * 0.5f), std::round(o_.y + (h_ - vh) * 0.5f));
      dl->AddImage(ui::Tex(frame), v0, ImVec2(v0.x + vw, v0.y + vh));
    }
    if (movies_->Done()) {
      movies_.reset();
      REXLOG_INFO("KK startup movies: done; the save notice loop next{}",
                  loop_scene_ ? (menu_scene::Ready() ? " (the scene is up)" : " (until the scene is up)") : "");
    }
    return true;
  }

  // Each frame, before the pages: true while the loop covers the whole window (the launcher draws nothing else
  // and takes no input then). While it fades it is drawn over everything.
  bool DrawLoadingLoop(ImDrawList* dl, float dt) {
    if (!loop_) return false;
    rex::ui::ImmediateTexture* frame = loop_->Frame(dt);
    loop_wait_ += std::clamp(dt, 0.0f, 0.1f);
    if (!frame && !loop_black_ && (loop_->Failed() || loop_wait_ > loading_loop::kMinSeconds)) {
      // Task 349c: with the scene starting, a plain black cover in its place until the scene is up (never the
      // pictures); without one, the launcher at once.
      const bool scene_coming = loop_scene_ || scene_start_pending_;
      REXLOG_INFO("KK loading loop: not shown ({}){}", loop_->Failed() ? "it cannot be played" : "no picture in time",
                  scene_coming ? "; a black cover until the scene is up" : "");
      if (!scene_coming) {
        loop_.reset();
        return false;
      }
      loop_black_ = true;
      loop_first_ = ImGui::GetTime();
    }
    // On screen since its first picture, in real time (the scene's start holds the UI thread with it on screen).
    if (frame && loop_first_ < 0) {
      loop_first_ = ImGui::GetTime();
      REXLOG_INFO("KK loading loop: its first picture is on screen");
    }
    loop_shown_ = loop_first_ < 0 ? 0.0f : float(ImGui::GetTime() - loop_first_);
    const bool scene_ready = menu_scene::Active() && menu_scene::Ready();
    // Task 349c (owner, 21 Oct): up until the scene is up; an attempt not up in 30 s starts the launcher again for a
    // second attempt; only a second attempt that fails too fades into the pictures.
    const float since_start = scene_started_at_ < 0 ? 0.0f : float(ImGui::GetTime() - scene_started_at_);
    const loading_loop::SceneStep step =
        loop_end_ >= 0 || scene_start_pending_ || scene_retrying_
            ? loading_loop::SceneStep::kWait
            : loading_loop::Step(loop_shown_, loop_scene_, scene_ready, since_start, menu_scene::SecondAttempt());
    if (step == loading_loop::SceneStep::kRetry && !RetrySceneStart()) {
      menu_scene::NoteAttemptFailed();
      REXLOG_WARN("KK loading loop: the scene is not up after {:.0f} s and cannot be tried again here: the pictures",
                  since_start);
      loop_end_ = 0;
    } else if (step == loading_loop::SceneStep::kPictures) {
      menu_scene::NoteAttemptFailed();
      REXLOG_WARN("KK loading loop: the second attempt's scene is not up after {:.0f} s either: fading into the "
                  "pictures", since_start);
      loop_end_ = 0;
    } else if (step == loading_loop::SceneStep::kEnd) {
      loop_end_ = 0;
      REXLOG_INFO("KK loading loop: fading into the launcher after {:.1f} s ({})", loop_shown_,
                  !loop_scene_ ? "no live scene" : "the scene is up");
    } else if (loop_end_ >= 0) {
      loop_end_ += std::clamp(dt, 0.0f, 0.1f);
    }
    const float cover = loading_loop::Cover(loop_end_);
    if (cover <= 0.0f) {
      loop_.reset();
      return false;
    }
    if (scene_ready) scene_mix_ = 1.0f;  // under the loop the scene replaces the pictures at once
    ImDrawList* target = cover >= 1.0f ? dl : ImGui::GetForegroundDrawList();
    const ImVec2 p1(o_.x + w_, o_.y + h_);
    target->AddRectFilled(o_, p1, IM_COL32(0, 0, 0, int(255 * cover)));
    // The picture's whole frame, its aspect kept (black bars on other window shapes).
    const float aspect = loop_->Aspect();
    float vw = w_, vh = w_ / aspect;
    if (vh > h_) {
      vh = h_;
      vw = h_ * aspect;
    }
    const ImVec2 v0(std::round(o_.x + (w_ - vw) * 0.5f), std::round(o_.y + (h_ - vh) * 0.5f));
    if (frame) target->AddImage(ui::Tex(frame), v0, ImVec2(v0.x + vw, v0.y + vh), ImVec2(0, 0), ImVec2(1, 1),
                                ui::WithAlpha(IM_COL32_WHITE, cover));
    if (loop_kind_ == loading_loop::Kind::kReturning && frame) {
      // Our lines in the empty space below the loop's "Returning to the main menu" (rows 520-561 of 1080), in the
      // launcher's font and the loop's own greys.
      const ui::Fonts f = ui::GetFonts();
      const float k = vh / 1080.0f;
      float y = v0.y + 618 * k;
      auto centred = [&](const std::string& text, float size, ImU32 col) {
        // Word-wrapped to the middle of the picture, each line centred.
        const float max_w = 1240 * k;
        std::vector<std::string> lines;
        std::string line;
        for (size_t at = 0; at <= text.size();) {
          size_t sp = text.find(' ', at);
          if (sp == std::string::npos) sp = text.size();
          const std::string word = text.substr(at, sp - at);
          const std::string tryline = line.empty() ? word : line + " " + word;
          if (!line.empty() && ui::TextSize(f.text, size, tryline).x > max_w) {
            lines.push_back(line);
            line = word;
          } else {
            line = tryline;
          }
          at = sp + 1;
        }
        if (!line.empty()) lines.push_back(line);
        for (const auto& l : lines) {
          const float lw = ui::TextSize(f.text, size, l).x;
          target->AddText(f.text, size, ImVec2(std::round(v0.x + (vw - lw) * 0.5f), std::round(y)),
                          ui::WithAlpha(col, cover), l.c_str());
          y += size * 1.3f;
        }
      };
      if (loop_saved_) {
        centred(T("Game saved at your last checkpoint."), 34 * k, IM_COL32(180, 181, 174, 255));
        y += 22 * k;
      }
      if (!loop_tip_.empty()) centred(loop_tip_, 28 * k, IM_COL32(146, 146, 144, 255));
    }
    return cover >= 1.0f;
  }

  // Task 330: the live menu scene's own sound replaces the launcher's music while the scene runs.
  bool SceneSoundPlays() const { return menu_scene::Active(); }
  // Its volume: the launcher's music volume, faded over about a second and a half as the music is.
  void FadeSceneSound(float target, float dt) {
    if (!menu_scene::Active()) return;
    const float step = std::clamp(dt, 0.0f, 0.1f) / 1.5f;
    scene_gain_ = target > scene_gain_ ? std::min(target, scene_gain_ + step) : std::max(target, scene_gain_ - step);
    menu_scene::SetAudioGain(scene_gain_);
  }

  // Task 330: the game's start menu scene behind the launcher, when nothing needs the stills (menu_scene_logic.h).
  menu_scene::StartFacts SceneFacts() {
    menu_scene::StartFacts f;
    f.files_ok = files_ok_;
    f.native_renderer = NativeShaderPack::Wanted();
    f.shaders_prepared = ShadersPreparedForThisVersion();
    f.shader_job = native_pack_.Busy() || update_shaders_ || setup_shaders_ || rebuild_shaders_screen_;
    f.setup = InSetup() || update_new_ || ShaderScreen();
    // C80: co-op keeps the live scene too. Its Start hands the session over through Play's restart
    // (coop_handover.h), so the stills rule of 20 Oct is gone.
    f.off_this_session = false;
#if defined(KK_DEV_TOOLS)
    // C81 tests: a launcher without the scene (as after failed scene starts), for the mixed co-op start.
    if (const char* off = std::getenv("KK_DEV_SCENE_OFF"); off && *off == '1') f.off_this_session = true;
#endif
    return f;
  }
  void MaybeStartScene(bool before_first_frame) {
    const menu_scene::Reason r = menu_scene::DecideAtLauncherStart(SceneFacts(), paths_.user_dir);
    // Owner's fresh install (20 Oct): no game files yet as the launcher opened, so no scene after the setup either.
    // Such a launcher tries again once the setup and the shaders are done (TickSceneStart).
    scene_retry_ = r != menu_scene::Reason::kLive && menu_scene::MayStartLater(r);
    if (r != menu_scene::Reason::kLive || !cb_.start_scene) return;
    loop_scene_ = true;  // task 331: a loading loop on screen waits for it (loading_loop_logic.h)
    scene_started_at_ = ImGui::GetTime();  // task 349c: its attempt's clock (loading_loop_logic.h Step)
    cb_.start_scene(before_first_frame);
  }
  // Task 349c: the attempt failed (not up in time): the launcher starts again as it opened (a new process: the game
  // cannot start twice in one) for the second attempt, its loop over this one's until its window has drawn.
  bool RetrySceneStart() {
    if (!cb_.restart_launcher || menu_scene::SecondAttempt() || scene_retrying_) return false;
    scene_retrying_ = true;
    menu_scene::NoteAttemptFailed();
    REXLOG_WARN("KK menu scene: not up after {:.0f} s: the launcher starts again for a second attempt",
                float(ImGui::GetTime() - scene_started_at_));
    SaveIfChanged();
    input_.Release();
    StopHeartbeat();
    leaving_ = true;
    modal_.open = false;
    const std::string open = loop_open_on_ == "rebuild" ? std::string() : loop_open_on_;
    std::wstring args = L"--kk_menu_scene_retry=true";
    if (!open.empty()) args += L" --kk_launcher_open=" + std::wstring(open.begin(), open.end());
    if (loop_saved_) args += L" --kk_launcher_saved=true";
    cb_.restart_launcher(args);
    return true;
  }
  // Back on Home after the setup, an install, the New in this update screen or a shader preparation: the scene
  // starts now (checked about once a second; the runtime's start pauses the launcher about 1.5 s once).
  void TickSceneStart() {
    // Task 332: not while the startup movies play (the scene's start pauses the launcher): it waits for them.
    if (!scene_retry_ || menu_scene::Active() || leaving_ || played_ || modal_.open || page_ != kHome || MoviesPending()) return;
    // Task 349c: the loading loop first; the scene starts once its first picture is on screen (the start pauses
    // the launcher about 1.5 s), so the launcher's pictures never show while it comes up.
    if (scene_start_pending_) {
      if (loop_ && loop_first_ < 0) return;
      scene_start_pending_ = false;
      MaybeStartScene(false);
      return;
    }
    if (ImGui::GetTime() < scene_retry_at_) return;
    scene_retry_at_ = ImGui::GetTime() + 1.0;
    const menu_scene::Reason r = menu_scene::Decide(SceneFacts());
    if (r == menu_scene::Reason::kLive) {
      REXLOG_INFO("KK menu scene: the setup and the shaders are done; the save notice loop, then the scene");
      if (!loop_ || loop_end_ >= 0) {
        loop_.reset();
        loop_first_ = -1;
        loop_end_ = -1.0f;
        loop_wait_ = 0.0f;
        loop_shown_ = 0.0f;
        loop_black_ = false;
        loop_saved_ = false;
        loop_tip_.clear();
        loop_kind_ = loading_loop::Kind::kNotice;
        loop_ = std::make_unique<LoadingLoop>(immediate_, LoadingLoopFile(loop_kind_));
      }
      scene_start_pending_ = true;
    } else if (!menu_scene::MayStartLater(r)) {
      scene_retry_ = false;
      REXLOG_INFO("KK menu scene: {}", menu_scene::ReasonText(r));
    }
  }

  void StartGame() {
    if (played_) return;
    if (!ContentReadyForPlay()) return;
    // The pack is merged into the shader cache, which the game opens as it
    // starts: a download in progress finishes first.
    if (pack_.busy) {
      waiting_for_pack_ = true;
      OpenModal("Shader pack", "The newest shader pack is still downloading. The game starts as soon as it's done.",
                {"Back"}, 0, [this](int) { waiting_for_pack_ = benchmark_pending_ = false; });
      modal_.progress = [this] { return PackProgress(); };
      return;
    }
    // The native renderer's shaders (shader_prepared.h, the one definition): a job still running (setup,
    // update, Rebuild) finishes first; prepared shaders, whatever the settings now, go straight to the game;
    // Play never prepares them again (owner rule, Beta 8 and 18 Oct). Only a version whose preparation has
    // not run yet (a setup closed half way) prepares them here, once.
    const bool checked = std::exchange(native_pack_checked_, false);
    if (NativeShaderPack::Wanted() && !checked) {
      const bool prepared = ShadersPreparedForThisVersion();
      const ShaderPlayStep step = AtPlay(native_pack_.Busy(), prepared, native_pack_.Finished());
      if (step == ShaderPlayStep::kPlay) {
        REXLOG_INFO("KK: Play: shaders {} for v{}; nothing to prepare",
                    prepared ? "prepared" : "not prepared (the game compiles them)", KK_VERSION);
      } else {
        if (step == ShaderPlayStep::kPrepareFirst) {
          REXLOG_INFO("KK: Play: shaders not prepared for v{} yet: preparing them first", KK_VERSION);
          native_pack_.Start(CacheDir(), paths_.game_dir);
        }
        waiting_for_native_ = true;
        native_wait_since_ = ImGui::GetTime();
        return;
      }
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
    // Task 330: the live menu scene is the game already running in this process: the game starts as a new one.
    if (menu_scene::Active()) needs_restart = true;
    // C80: a co-op hand over always restarts (the new process reconnects), with or without the scene.
    if (coop::HandoverPending()) needs_restart = true;
    FlushProfileOptions();
    if (coop::SessionInProcess()) {
      // A live peer and its worker cannot survive a restart. SessionStart is
      // consumed in-process; new-device settings take effect on the next run.
      if (needs_restart) for (size_t i=0;i<std::size(kRestartCvars);++i) {
        if (Get(kRestartCvars[i])==restart_baseline_[i]) continue;
        if (std::string_view(kRestartCvars[i])=="native_upscaler" &&
            !UpscalerNeedsNewDevice(restart_baseline_[i],Get("native_upscaler"))) continue;
        // The existing transient-setting overlay retains the user's saved
        // choice while this process keeps the configuration of its live device.
        SetModSetting(kRestartCvars[i],restart_baseline_[i]);
      }
      needs_restart=false;
      paths_.user_dir=coop::SessionUserDir(paths_.user_dir);
    }
    auto action = benchmark_pending_ ? cb_.benchmark : needs_restart ? cb_.restart_and_play : cb_.play;
    // The pause menu for the game about to start (the app makes it too when the launcher is skipped).
    if (!needs_restart && !benchmark_pending_) CreatePauseMenu(imgui_drawer(), immediate_, paths_.config_path, paths_.user_dir, paths_.game_dir);
    if (!needs_restart && !benchmark_pending_) photo::CreatePanel(imgui_drawer(), immediate_, paths_.user_dir);
    input_.Release();  // the game opens the controllers itself
    StopHeartbeat();   // the launcher's work is done (its process ends, or the game runs in it)
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

  // ------------------------------------------- Play while downloading (J3) ---
  // Owner, 17 Oct: Play while a download runs asks first; Play now pauses it (the partial file kept) and the
  // launcher continues it when it is back (StartContent: ResumeAfterPlay). An unpack (or Remove, or an
  // imported ZIP being installed) is never interrupted: the game waits for it ("Finishing the install"),
  // the safe option, as stopping an unpack half way would mean unpacking again from the start.
  enum class PlayWait { kNone, kPausing, kFinishing };
  static bool MustFinish(const content::Status& st, bool importing) {
    return st.stage == content::Stage::kUnpacking || st.stage == content::Stage::kRemoving || (importing && st.Busy());
  }
  // True when nothing runs (the game may start). Otherwise opens the pop-up and returns false.
  bool ContentReadyForPlay() {
    if (play_wait_ != PlayWait::kNone) return false;
    auto& service = content::Service::Get();
    if (!service.Ready()) return true;
    bool finishing = false;
    std::vector<content::Kind> running;
    for (int i = 0; i < content::kKindCount; ++i) {
      const auto kind = content::Kind(i);
      const auto st = service.Get(kind);
      if (!st.Busy() || content::Quiet(kind)) continue;
      if (MustFinish(st, kind == content::Kind::kImport)) finishing = true;
      else running.push_back(kind);
    }
    if (finishing) {
      REXLOG_INFO("KK content: Play while an install finishes: the game waits for it");
      OpenFinishingForPlay();
      return false;
    }
    if (running.empty()) return true;
    const bool tex = std::find(running.begin(), running.end(), content::Kind::kTexturePack) != running.end();
    const bool cut = std::find(running.begin(), running.end(), content::Kind::kCutscenes) != running.end();
    const bool mod_pack = std::any_of(running.begin(), running.end(), content::IsModPack);  // a Workshop download
    REXLOG_INFO("KK content: Play while downloading ({}{}): asking", tex ? "tex " : "", cut ? "cut" : "");
    OpenModal("Your download will pause while you play",
              mod_pack ? "Your downloads carry on when you come back to the launcher. What was downloaded so far is kept."
              : tex && cut ? "The HD texture pack and the PS2 cutscenes carry on downloading when you come back to the launcher. W"
                                 "hat was downloaded so far is kept." : tex ? "The HD texture pack carries on downloading when you come back to the launcher. What was downloaded so far is kept." : "The PS2 cutscenes carry on downloading when you come back to the launcher. What was downloaded so far is kept.",
              {"Play now", "Stay here"}, 1, [this, running](int i) {
                if (i != 0) {
                  REXLOG_INFO("KK content: Play while downloading: stay here");
                  benchmark_pending_ = false;
                  return;
                }
                for (const auto kind : running) content::Service::Get().PauseForPlay(kind);
                play_wait_ = PlayWait::kPausing;
                play_wait_since_ = std::chrono::steady_clock::now();
                REXLOG_INFO("KK content: Play now: pausing the download, then the game starts");
                OpenModal("Pausing the download", "The game starts in a moment.", {"Stay here"}, 0,
                          [this](int) { StopPlayWait(true); });
              }, 0);
    return false;
  }
  void OpenFinishingForPlay() {
    play_wait_ = PlayWait::kFinishing;
    OpenModal("Finishing the install", "The download has finished and is being unpacked. That cannot be paused safely, so the game start"
              "s as soon as it is done.", {"Stay here"}, 0, [this](int) { StopPlayWait(false); });
    modal_.live = [] {
      for (int i = 0; i < content::kKindCount; ++i) {
        if (content::Quiet(content::Kind(i))) continue;
        const auto st = content::Service::Get().Get(content::Kind(i));
        if (MustFinish(st, false) || st.stage == content::Stage::kVerifying) return ContentProgressLine(st, false);
      }
      return std::string(T("Finishing"));
    };
    modal_.progress = [] {
      for (int i = 0; i < content::kKindCount; ++i) {
        if (content::Quiet(content::Kind(i))) continue;
        const auto st = content::Service::Get().Get(content::Kind(i));
        if (st.stage == content::Stage::kUnpacking) return ContentFraction(st);
      }
      return -1.0f;
    };
    play_wait_title_ = modal_.title;
  }
  // "Stay here" while waiting: no game. A download paused for the game continues at once.
  void StopPlayWait(bool resume) {
    play_wait_ = PlayWait::kNone;
    benchmark_pending_ = false;
    if (resume && content::Service::Get().ResumeAfterPlay()) REXLOG_INFO("KK content: stay here: the download continues");
  }
  void TickPlayWithContent() {
    if (play_wait_ == PlayWait::kNone) return;
    auto& service = content::Service::Get();
    bool busy = false, finishing = false;
    for (int i = 0; i < content::kKindCount; ++i) {
      const auto kind = content::Kind(i);
      if (content::Quiet(kind)) continue;
      const auto st = service.Get(kind);
      busy = busy || st.Busy();
      finishing = finishing || MustFinish(st, kind == content::Kind::kImport);
    }
    if (play_wait_ == PlayWait::kPausing) {
      if (finishing) {  // the download was already past the point of pausing: wait for its install instead
        REXLOG_INFO("KK content: the download was already being unpacked: the game waits for it");
        OpenFinishingForPlay();
        return;
      }
      const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - play_wait_since_).count();
      if (busy && waited < 15.0) return;
      REXLOG_INFO("KK content: download paused for the game after {:.1f} s{}", waited, busy ? " (still stopping: starting anyway)" : "");
    } else if (busy) {
      return;
    } else {
      REXLOG_INFO("KK content: install finished: the game starts");
    }
    play_wait_ = PlayWait::kNone;
    if (modal_.open && (modal_.title == play_wait_title_ || modal_.title == "Pausing the download")) modal_.open = false;
    StartGame();  // checks again: a download queued behind the install asks first
  }

  // ------------------------------------------------- content setup (J2) ---
  // Setup steps 3 and 4 (codex/state/setup-flow.md): the HD texture pack page (Download / I already have a
  // texture pack / Skip) and "Restore the PS2 cutscenes?" (Yes / No thanks), shown after the shader screen.
  // A download starts with its progress pop-up (Pause / Run in background / Cancel), then runs in the
  // background (content.h): the setup goes on, a strip shows its progress on the later setup pages and
  // on Home, and a pop-up says when it has finished or what went wrong. The same pop-ups serve the Settings
  // rows (content_action_).

  // 6: the texture pack page, 7: the cutscenes page (SetupStage numbers).
  void FinishSetupContentStep(int stage, const char* what) {
    REXLOG_INFO("KK: setup: {} {}", stage == 6 ? "texture pack" : "PS2 cutscenes", what);
    if (stage == 6) setup_textures_ = false;
    else setup_cutscenes_ = false;
    setup_focus_ = 0;
    if (content_watch_ >= 0 && content_watch_setup_ == stage) content_watch_setup_ = 0;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ == 6) dev_setup_ = movies::PlaybackAvailable() ? 7 : kWorkshopStage;
    else if (dev_setup_ == 7) dev_setup_ = kWorkshopStage;
#endif
  }

  // A setup content page is skipped when there is nothing to ask as it comes up (installed, or a download
  // from an earlier session already kept).
  void TickSetupContent() {
    auto& service = content::Service::Get();
    const int stage = SetupStage();
    if (!service.Ready() || (stage != 6 && stage != 7) || (setup_content_seen_ & (1 << stage))) return;
    setup_content_seen_ |= 1 << stage;
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) return;  // screenshots of the page
#endif
    // Task 209: installed or downloading from before, the page still shows (with "Already installed" and Continue)
    // instead of being skipped without a word.
    const auto st = service.Get(stage == 6 ? content::Kind::kTexturePack : content::Kind::kCutscenes);
    REXLOG_INFO("KK: setup: {} page{}", stage == 6 ? "texture pack" : "PS2 cutscenes",
                st.installed ? " (already installed)" : st.Busy() || st.stage == content::Stage::kPaused ? " (already downloading)" : "");
  }

  void OnContentAction(content::Kind kind, int verb) {
    switch (verb) {
      case kContentDownload: StartContentDownload(kind, 0); break;
      case kContentWatch: OpenContentProgress(kind, 0); break;
      case kContentImport: ChooseImportZip(content::ImportTarget::kTexturePack, 0); break;
      case kContentRemove: ConfirmContentRemove(kind); break;
      case kContentCancel: ConfirmContentCancel(kind, false); break;
    }
  }

  // A download starts straight away and its progress pop-up opens (owner, 19 Oct: no "This can take a while"
  // question and no minimise first): Pause, Run in background, Cancel. `setup_stage`: the setup page it was
  // started on (0: Settings, the Workshop, the New screen): Run in background moves the setup on.
  void StartContentDownload(content::Kind kind, int setup_stage) {
    content::Service::Get().Download(kind);
    REXLOG_INFO("KK content: {} download started ({}); watching the progress", content::InfoFor(kind).id,
                setup_stage ? "setup" : "Settings");
    OpenContentProgress(kind, setup_stage);
  }

  // Job J3: system notifications (system_notify.h) on their own worker thread. A click on one restores the
  // launcher's window (on the UI thread).
  void StartNotifications() {
    notify::Start(
        [post = cb_.post_to_ui, restore = cb_.restore] {
          if (!post || !restore) return;
          post([restore] {
            REXLOG_INFO("KK notify: restoring the launcher's window (notification clicked)");
            restore();
          });
        },
        [](bool warning, const std::string& line) {
          if (warning) REXLOG_WARN("{}", line);
          else REXLOG_INFO("{}", line);
        });
  }

  // Job J3: a download or install ended (`title` and `body` already in the player's language). Besides the
  // launcher's own pop-up (ContentEnded): the ding unless the launcher's sounds are muted, and (Settings switch
  // on) a system notification when the launcher is minimised or not the active window, e.g. after Run in
  // background (notify_logic.h).
  void NotifyContentDone(content::Kind kind, bool ok, const std::string& title, const std::string& body) {
    notify::Situation s;
    if (cb_.window_state) std::tie(s.minimised, s.focused) = cb_.window_state();
    s.setting_on = GetBool("kk_notify_downloads");
    s.sounds_on = GetBool("kk_launcher_sounds");
    s.sounds_volume = GetInt("kk_launcher_sounds_volume", 50);
    const notify::Plan plan = notify::Decide(s);
    REXLOG_INFO("KK notify: {} {}: {}; window {}, {}; ding {}; system notification {} ({})",
                content::InfoFor(kind).id, ok ? "finished" : "failed", title, s.minimised ? "minimised" : "shown",
                s.focused ? "active" : "not active", plan.ding ? "yes" : "no (sounds muted)",
                plan.system ? "yes" : "no", plan.why);
    if (plan.ding) PlayDing();
    if (plan.system) notify::Show(title, body, content::InfoFor(kind).id);
  }

  void PlayDing() {
    if (!ui_sounds_ && files_ok_) ui_sounds_ = std::make_unique<LauncherSounds>(paths_.game_dir);
    if (!ui_sounds_) return;
    const float v = GetInt("kk_launcher_sounds_volume", 50) / 100.0f;
    ui_sounds_->PlayDing(v * v);
  }

  // One line about a download: what it does now, how far, how fast, the time left.
  static std::string ContentProgressLine(const content::Status& st, bool other_running) {
    using content::Stage;
    switch (st.stage) {
      case Stage::kQueued: return other_running ? T("Waiting for the other download to finish") : T("Starting");
      case Stage::kStarting: return T("Connecting");
      case Stage::kDownloading: {
        std::string line = st.total ? F("{0} of {1} GB", {Gb(st.done), Gb(st.total)}) : std::string(T("Downloading"));
        if (st.speed > 0) {
          char speed[32];
          std::snprintf(speed, sizeof(speed), "%.1f", st.speed / 1e6);
          line += "  " KK_DOT "  " + F("{0} MB/s", {speed});
        }
        if (const std::string left = TimeLeft(st.seconds_left); !left.empty()) line += "  " KK_DOT "  " + left;
        return line;
      }
      case Stage::kPaused: return st.total ? F("Paused at {0} of {1} GB", {Gb(st.done), Gb(st.total)}) : T("Paused");
      case Stage::kVerifying: return T("Checking the download");
      case Stage::kUnpacking:  // task 209: a big pack takes minutes: how far it is, steadily
        return st.total ? F("Unpacking: {0} of {1} GB", {Gb(st.done), Gb(st.total)}) : T("Unpacking the files");
      case Stage::kRemoving: return T("Removing");
      case Stage::kInstalled: return T("Installed");
      case Stage::kFailed: return T("Stopped");
      case Stage::kNone: break;
    }
    return {};
  }
  static float ContentFraction(const content::Status& st) {
    if (st.stage == content::Stage::kInstalled) return 1.0f;
    if (!st.total || st.stage == content::Stage::kQueued || st.stage == content::Stage::kStarting) return -1.0f;
    return float(std::clamp(double(st.done) / double(st.total), 0.0, 1.0));
  }
  static bool OtherRunning(content::Kind kind) {
    for (int i = 0; i < content::kKindCount; ++i) {  // every job but the quiet artbook
      const auto k = content::Kind(i);
      if (k == kind || content::Quiet(k)) continue;
      const auto st = content::Service::Get().Get(k);
      if (st.Busy() && st.stage != content::Stage::kQueued) return true;
    }
    return false;
  }
  // Task 209: what a download or install is called in its pop-ups and on the strip.
  static const char* ContentName(content::Kind kind) {
    switch (kind) {
      case content::Kind::kTexturePack: return "HD texture pack";
      case content::Kind::kCutscenes: return "PS2 cutscenes";
      case content::Kind::kImport:
        return content::Service::Get().Get(content::Kind::kImport).target == content::ImportTarget::kMod ? "Mod" : "Texture pack";
      default: {
        const content::ModPack* pack = content::ModPackFor(kind);  // a Workshop mod pack: its name
        return pack ? pack->title : "";
      }
    }
  }

  // The progress pop-up ("Watch progress", or a Settings row while a download runs): the line, the bar,
  // Pause or Continue, Run in background (also Back) and Cancel. Its buttons follow the stage (TickContent
  // opens it again when they change); it closes when the job ends, and the finished or error pop-up follows.
  static int ContentWatchMode(const content::Status& st, bool importing) {
    using content::Stage;
    if (importing) return st.stage == Stage::kRemoving ? 3 : 2;  // task 209: an import can be cancelled
    if (st.stage == Stage::kPaused) return 1;
    if (st.stage == Stage::kUnpacking || st.stage == Stage::kRemoving) return 2;
    return 0;
  }
  void OpenContentProgress(content::Kind kind, int setup_stage, int focus = -1) {
    using namespace content;
    const bool tex = kind == Kind::kTexturePack;
    const auto st = Service::Get().Get(kind);
    const bool importing = kind == Kind::kImport;
    const bool mod_import = importing && Service::Get().Get(kind).target == ImportTarget::kMod;
    const int mode = ContentWatchMode(st, importing);
    std::vector<std::string> buttons;
    if (mode == 0) buttons = {"Pause", "Run in background", "Cancel"};
    else if (mode == 1) buttons = {"Continue", "Run in background", "Cancel"};
    else if (mode == 2) buttons = {"Run in background", "Cancel"};
    else buttons = {"Run in background"};
    const int background = mode <= 1 ? 1 : 0;
    const std::string title = importing ? (mod_import ? "Installing your mod" : "Installing your texture pack")
                              : IsModPack(kind) ? "Downloading " + std::string(ModPackFor(kind)->title)  // its own text row
                              : tex     ? "Downloading the HD texture pack"
                                        : "Downloading the PS2 cutscenes";
    OpenModal(title,
              importing ? T("Your ZIP file is checked and unpacked. It carries on in the background if you leave "
                            "this popup, and Cancel stops it without changing anything you have installed.")
                        : T("It carries on in the background if you leave this popup. If the connection drops or "
                            "you close the launcher, it continues from where it stopped."),
              buttons, background,
              [this, kind, mode, background](int i) {
                const int setup_stage = content_watch_setup_;
                content_watch_ = -1;
                auto& service = content::Service::Get();
                if (i == background) {
                  REXLOG_INFO("KK content: {}: run in background", content::InfoFor(kind).id);
                  if (setup_stage) FinishSetupContentStep(setup_stage, "download runs in the background");
                } else if (mode <= 1 && i == 0) {
                  if (mode == 0) service.Pause(kind);
                  else service.Download(kind);
                  REXLOG_INFO("KK content: {}: {}", content::InfoFor(kind).id, mode == 0 ? "pause" : "continue");
                  OpenContentProgress(kind, setup_stage, 0);
                } else {
                  ConfirmContentCancel(kind, true, setup_stage);
                }
              },
              focus >= 0 ? focus : background);
    modal_.live = [kind] { return ContentProgressLine(content::Service::Get().Get(kind), OtherRunning(kind)); };
    modal_.progress = [kind] { return ContentFraction(content::Service::Get().Get(kind)); };
    content_watch_ = int(kind);
    content_watch_mode_ = mode;
    content_watch_title_ = modal_.title;
    content_watch_setup_ = setup_stage;
  }

  // Cancel (from the progress pop-up, or the Settings row): asked first; what was downloaded is deleted.
  void ConfirmContentCancel(content::Kind kind, bool from_progress, int setup_stage = 0) {
    if (kind == content::Kind::kImport) {  // task 209: nothing to keep; the staging folder is deleted
      OpenModal("Cancel the install?", "Nothing is installed and everything you had stays as it was. Your ZIP file is kept.",
                {"Cancel install", "Keep going"}, 1, [this, from_progress, setup_stage](int i) {
                  if (i == 0) {
                    content::Service::Get().Cancel(content::Kind::kImport);
                    REXLOG_INFO("KK content: import cancelled by the player");
                    Status("Install cancelled.");
                  } else if (from_progress) {
                    OpenContentProgress(content::Kind::kImport, setup_stage);
                  }
                });
      return;
    }
    OpenModal("Cancel the download?",
              "What was downloaded so far is deleted. You can download it again at any time.",
              {"Cancel download", "Keep downloading"}, 1, [this, kind, from_progress, setup_stage](int i) {
                if (i == 0) {
                  content::Service::Get().Cancel(kind);
                  REXLOG_INFO("KK content: {}: cancelled by the player", content::InfoFor(kind).id);
                  Status("Download cancelled.");
                } else if (from_progress) {
                  OpenContentProgress(kind, setup_stage);
                }
              });
  }

  void ConfirmContentRemove(content::Kind kind) {
    const bool tex = kind == content::Kind::kTexturePack;
    OpenModal(tex ? "Remove the texture pack?" : "Remove the PS2 cutscenes?",
              tex ? "This deletes the installed texture pack from your user folder and turns Texture pack off. You "
                    "can download or import it again at any time."
                  : "This deletes the restored cutscenes from your user folder and turns PS2 cutscenes off. You can "
                    "download them again at any time.",
              {"Remove", "Keep it"}, 1, [kind](int i) {
                if (i != 0) return;
                REXLOG_INFO("KK content: {}: remove", content::InfoFor(kind).id);
                content::Service::Get().Remove(kind);
              });
  }

  // "I already have a texture pack" (setup), the Workshop's Import texture pack and Import mod: the system's file
  // dialog; the chosen ZIP is installed (TickContent) with the progress pop-up open.
  void ChooseImportZip(content::ImportTarget target, int setup_stage) {
    {
      std::lock_guard lock(texture_pack_zip_choice.mutex);
      if (texture_pack_zip_choice.busy) return;
      texture_pack_zip_choice.busy = true;
      texture_pack_zip_choice.target = target;
    }
    content_import_setup_ = setup_stage;
    REXLOG_INFO("KK content: choosing a {} ZIP ({})", target == content::ImportTarget::kMod ? "mod" : "texture pack",
                setup_stage ? "setup" : "Workshop");
    static const SDL_DialogFileFilter kZip[] = {{"ZIP", "zip"}};
    SDL_ShowOpenFileDialog([](void*, const char* const* files, int) {
      std::lock_guard lock(texture_pack_zip_choice.mutex);
      if (files && files[0]) texture_pack_zip_choice.selected = files[0];
      texture_pack_zip_choice.busy = false;
    }, nullptr, nullptr, kZip, 1, nullptr, false);
  }

  // A download or install that ended: the progress pop-up closes, the notification hook, and the finished
  // or error pop-up (shown as soon as no other pop-up is open and the shader screen is not up).
  void ContentEnded(content::Kind kind, const content::Status& st) {
    using namespace content;
    const bool tex = kind == Kind::kTexturePack || IsModPack(kind);
    const bool imported = kind == Kind::kImport;
    const bool mod_import = imported && st.target == ImportTarget::kMod;
    if (content_watch_ == int(kind)) {
      if (modal_.open && modal_.title == content_watch_title_) modal_.open = false;
      const int setup_stage = content_watch_setup_;
      content_watch_ = -1;
      if (setup_stage) FinishSetupContentStep(setup_stage, st.stage == Stage::kInstalled ? "installed" : "failed");
    }
    if (st.stage == Stage::kInstalled) {
      std::string title = imported ? (mod_import ? "Mod installed" : "Texture pack installed")
                          : IsModPack(kind) ? std::string(ModPackFor(kind)->title) + " installed"  // its own text row
                          : tex ? "HD texture pack installed" : "PS2 cutscenes restored";
      std::string body =
          mod_import ? F("{0} is in your mods folder. Switch it on in the Workshop's Mods row.", {st.installed_name})
          : imported || IsModPack(kind)
              ? F("{0} is installed and turned on. It is the only texture pack on: the one before it was switched off. "
                  "Choose another at any time in the Workshop or in Settings > Graphics.",
                  {st.installed_name})
          : tex ? T("The HD texture pack is installed and turned on. It is the only texture pack on: the one before it "
                    "was switched off. You can turn it off at any time in Settings > Graphics or the Workshop.")
                : T("The PS2 cutscenes are restored and turned on. They play in the game where the Xbox 360 version "
                    "has none. You can turn them off at any time in Settings > Advanced.");
      NotifyContentDone(kind, true, T(title), body);
      content_popups_.push_back([this, title, body] { OpenModal(title, body, {"OK"}, 0, {}); });
      return;
    }
    // Failed.
    std::string title, body;
    std::vector<std::string> buttons = {"Try again", "Later"};
    switch (st.error) {
      case Error::kConnection:
        title = "Download interrupted";
        body = T("The connection was lost. What was downloaded so far is kept, so the download continues from where "
                 "it stopped.");
        buttons = {"Continue download", "Later"};
        break;
      case Error::kChecksum:
        title = "Download check failed";
        body = T("The downloaded file did not match the original, so it was deleted. This can happen when a "
                 "connection damages the download. Download it again?");
        buttons = {"Download again", "Later"};
        break;
      case Error::kSpace:
        title = "Not enough free space";
        body = F("It needs {0} GB of free space on the drive with your user folder while it installs, and the drive "
                 "has {1} GB free. Free some space, then try again.",
                 {Gb(st.space_needed), Gb(st.space_free)});
        break;
      default:
        title = imported ? (mod_import ? "Mod not installed" : "Texture pack not installed") : "Download failed";
        body = ContentError(st, tex || imported);
        if (imported) body += std::string(" ") + T("Nothing was installed and everything you had stays as it was.");
        break;
    }
    if (imported) buttons = {"OK"};
    NotifyContentDone(kind, false, T(title), body);
    content_popups_.push_back([this, kind, title, body, buttons] {
      OpenModal(title, body, buttons, int(buttons.size()) - 1, [kind, retry = buttons.size() > 1](int i) {
        if (!retry || i != 0) return;
        REXLOG_INFO("KK content: {}: try again", content::InfoFor(kind).id);
        content::Service::Get().Download(kind);
      });
    });
  }

  // The size facts beside a setup content page: the download, the installed size, the free space it
  // needs and what the drive has.
  void DrawContentCard(ImDrawList* dl, content::Kind kind) {
    using namespace content;
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const Info& info = InfoFor(kind);
    const float right = o_.x + w_ - Margin();
    const float x0 = std::max(o_.x + Margin() + std::min(620 * s, w_ * 0.52f) + 48 * s, right - 470 * s);
    const float w = right - x0, pad = 18 * s, r = 10 * s;
    const float y = o_.y + std::max(84 * s, h_ * 0.12f) + 4 * s;
    if (ImGui::GetTime() - free_space_at_ > 2.0) {
      free_space_at_ = ImGui::GetTime();
      std::error_code ec;
      const auto sp = std::filesystem::space(paths_.user_dir, ec);
      free_space_ = ec ? 0 : sp.available;
    }
    const uint64_t needed = SpaceNeeded(info.zip_size, 0, info.unpacked);
    struct Row {
      const char* label;
      std::string value;
      ImU32 color;
    };
    const Row rows[] = {
        {"Download", F("{0} GB", {Gb(info.zip_size)}), color::kText},
        {"Installed size", F("{0} GB", {Gb(info.unpacked)}), color::kText},
        {"Space needed", F("{0} GB while it installs", {Gb(needed)}), color::kText},
        {"Free space", free_space_ ? F("{0} GB", {Gb(free_space_)}) : T(std::string("Unknown")),
         free_space_ && free_space_ < needed ? color::kWarn : color::kText},
    };
    const std::string note = T("It goes into your user folder. Remove it at any time in Settings.");
    const float label_w = 150 * s, line = 26 * s;
    const float h = pad + 26 * s + line * float(std::size(rows)) + 6 * s +
                    ui::TextSize(f.text, 12.5f * s, note, w - pad * 2).y + pad;
    dl->AddRectFilled(ImVec2(x0, y + 5 * s), ImVec2(right, y + h + 5 * s), IM_COL32(0, 0, 0, 40), r);
    dl->AddRectFilled(ImVec2(x0, y), ImVec2(right, y + h), ui::WithAlpha(IM_COL32(16, 18, 22, 255), 0.62f), r);
    dl->AddRect(ImVec2(x0, y), ImVec2(right, y + h), ui::WithAlpha(color::kText, 0.09f), r, 0, 1.0f * s);
    ui::DrawTracked(dl, f.display, 12 * s, ImVec2(x0 + pad, y + pad), color::kAccent,
                    Upper(T(ContentName(kind))), 2.6f * s);
    float ry = y + pad + 26 * s;
    for (const Row& row : rows) {
      dl->AddText(f.text, 13.5f * s, ImVec2(x0 + pad, ry), color::kTextDim, T(row.label));
      dl->AddText(f.semibold, 13.5f * s, ImVec2(x0 + pad + label_w, ry), row.color, row.value.c_str());
      ry += line;
    }
    dl->AddLine(ImVec2(x0 + pad, ry + 2 * s), ImVec2(right - pad, ry + 2 * s), color::kHairline, 1.0f);
    dl->AddText(f.text, 12.5f * s, ImVec2(x0 + pad, ry + 8 * s), color::kTextDim, note.c_str(), nullptr, w - pad * 2);
  }

  // The download strip: the later setup pages and Home show what downloads, how far it is and the time
  // left (bottom right above the side panels' row; `top`: at the top right instead, where the later setup pages'
  // cards leave room). A click opens the progress pop-up; controllers reach it through Settings.
  bool DrawContentStrip(ImDrawList* dl, float alpha, bool top) {
    using namespace content;
    auto& service = Service::Get();
    if (!service.Ready()) return false;
    int pick = -1, waiting = 0;
    content::Status st;
    for (const int rank : {0, 1, 2, 3}) {  // running, queued, paused, stopped
      for (int i = 0; i < kKindCount && pick < 0; ++i) {
        if (Quiet(Kind(i))) continue;  // the artbook shows no strip
        const content::Status k = service.Get(Kind(i));
        const bool match = rank == 0   ? k.Busy() && k.stage != Stage::kQueued && k.stage != Stage::kRemoving
                           : rank == 1 ? k.stage == Stage::kQueued
                           : rank == 2 ? k.stage == Stage::kPaused
                                       : k.stage == Stage::kFailed && !k.installed;
        if (match) pick = i, st = k;
      }
    }
    if (pick < 0) return false;
    for (int i = 0; i < kKindCount; ++i)
      if (i != pick && !Quiet(Kind(i)) && service.Get(Kind(i)).stage == Stage::kQueued) ++waiting;
    const Kind kind = Kind(pick);
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float w = std::min(420 * s, w_ * 0.4f), h = 66 * s, pad = 14 * s, r = 10 * s;
    const float right = o_.x + w_ - Margin();
    const float y0 = top ? o_.y + std::max(84 * s, h_ * 0.12f) : o_.y + h_ - 120 * s - 24 * s - h;
    const ImVec2 p0(right - w, y0), p1(right, y0 + h);
    const bool hovered = !PageInputBlocked() && ImGui::IsMouseHoveringRect(p0, p1, false);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 5 * s), ImVec2(p1.x, p1.y + 5 * s), IM_COL32(0, 0, 0, int(40 * alpha)), r);
    dl->AddRectFilled(p0, p1, ui::WithAlpha(hovered ? IM_COL32(30, 33, 40, 255) : IM_COL32(16, 18, 22, 255), 0.72f * alpha), r);
    dl->AddRect(p0, p1, ui::WithAlpha(color::kText, (hovered ? 0.22f : 0.09f) * alpha), r, 0, 1.0f * s);
    std::string caption = Upper(T(ContentName(kind)));
    if (waiting) caption += "  " KK_DOT "  " + Upper(F("{0} more waiting", {std::to_string(waiting)}));
    ui::DrawTracked(dl, f.display, 11.5f * s, ImVec2(p0.x + pad, p0.y + pad), ui::WithAlpha(color::kAccent, alpha),
                    caption, 2.4f * s);
    const float fraction = ContentFraction(st);
    const std::string pct = fraction >= 0 && st.stage != Stage::kFailed ? std::to_string(int(fraction * 100)) + "%" : "";
    const float pw = ui::TextSize(f.semibold, 13 * s, pct).x;
    dl->AddText(f.semibold, 13 * s, ImVec2(p1.x - pad - pw, p0.y + pad - 1 * s), ui::WithAlpha(color::kText, alpha),
                pct.c_str());
    const std::string line = ui::Ellipsize(f.text, 13.5f * s, ContentProgressLine(st, OtherRunning(kind)), w - pad * 2);
    dl->AddText(f.text, 13.5f * s, ImVec2(p0.x + pad, p0.y + pad + 19 * s),
                ui::WithAlpha(st.stage == Stage::kFailed ? color::kWarn : color::kTextDim, alpha), line.c_str());
    const float by = std::round(p1.y - pad + 2 * s), bx0 = p0.x + pad, bx1 = p1.x - pad;
    dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + 2 * s), ui::WithAlpha(color::kText, 0.12f * alpha));
    if (fraction >= 0) {
      dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx0 + (bx1 - bx0) * fraction, by + 2 * s),
                        ui::WithAlpha(st.stage == Stage::kPaused || st.stage == Stage::kFailed ? color::kTextDim
                                                                                                : color::kAccent,
                                      alpha));
    } else {
      const float t = float(std::fmod(ImGui::GetTime() * 0.6, 1.0));
      dl->AddRectFilled(ImVec2(bx0 + (bx1 - bx0) * std::max(0.0f, t * 1.3f - 0.3f), by),
                        ImVec2(bx0 + (bx1 - bx0) * std::min(1.0f, t * 1.3f), by + 2 * s),
                        ui::WithAlpha(color::kAccent, alpha));
    }
    if (hovered && ImGui::IsMouseClicked(0) && st.stage != Stage::kFailed) {
      ui::Cue(ui::Sound::kSelect);
      OpenContentProgress(kind, 0);
    }
#if defined(KK_DEV_TOOLS)
    {  // Developer: what the strip shows, every 5 s (tour logs).
      static double logged = -10;
      if (ImGui::GetTime() - logged >= 5.0 && st.stage != Stage::kUnpacking) {  // no UI log while it unpacks
        logged = ImGui::GetTime();
        REXLOG_INFO("KK dev: content strip {} '{}' {} ({})", InfoFor(kind).id, line, pct, top ? "top" : "bottom");
      }
    }
#endif
    return true;
  }

  // ------------------------------------------------------------ Easter egg ---
  // Task 357 (owner, 22 Oct): on Home, a controller's Up, Up, Down, Down, Left, Right, Left, Right, B, A, Start
  // starts the owner's little game (launcher\easter-egg\). While it runs the launcher keeps reading the
  // controller: Start brings the launcher in front with a pause panel (Resume, Quit to main menu). Its music
  // and sounds are silent meanwhile. Nothing on screen tells of the code.
  // Task 358: the keyboard too, mixed with the controller in one code: the arrow keys for the directions, B and A
  // for B and A, Enter for Start (kept from the launcher while it is the code's next press, as Start is). Any
  // other key is a wrong press.
  // Task 364: the original game's cheat menu codes start it too, from a controller only (egg::kHeldCodes): PS2
  // (hold LB and RB), Xbox and GameCube (hold a left and a right bumper or trigger), Xbox 360 (hold all four).
  // While a code's hold is down, Home does nothing else with the controller or the keyboard (no moving, no Steam
  // or Discord or Quit from X, Y or B); with the hold up, Home works as always. LB, RB, LT and RT alone already do
  // nothing on Home. The game starts on the code's last press (no need to let go first).
  void EggCodeInput() {
    const uint32_t presses = input_.pad_presses(), keys = input_.key_presses();
    {
      const uint32_t held = input_.pad_held();
      unsigned hold = 0;
      if (held & ui::Input::kPadLB) hold |= egg::kHoldLB;
      if (held & ui::Input::kPadRB) hold |= egg::kHoldRB;
      if (held & ui::Input::kPadLT) hold |= egg::kHoldLT;
      if (held & ui::Input::kPadRT) hold |= egg::kHoldRT;
      egg_held_codes_.SetHeld(hold);
    }
    const bool holding = egg_held_codes_.Holding();
#if defined(KK_DEV_TOOLS)
    if (holding != egg_holding_) REXLOG_INFO("KK dev: easter egg: code hold {}", holding ? "down" : "up");
#endif
    egg_holding_ = holding;
    if (holding) {
      static constexpr std::pair<uint32_t, egg::Pad> kHeldBits[] = {
          {ui::Input::kPadUp, egg::Pad::kUp}, {ui::Input::kPadDown, egg::Pad::kDown},
          {ui::Input::kPadLeft, egg::Pad::kLeft}, {ui::Input::kPadRight, egg::Pad::kRight},
          {ui::Input::kPadA, egg::Pad::kA}, {ui::Input::kPadB, egg::Pad::kB},
          {ui::Input::kPadX, egg::Pad::kX}, {ui::Input::kPadY, egg::Pad::kY},
          {ui::Input::kPadStart, egg::Pad::kStart}, {ui::Input::kPadBack, egg::Pad::kOther},
      };  // LB, RB, LT and RT are the holds, not presses
      bool start = false;
      for (const auto& [bit, pad] : kHeldBits) {
        if (!(presses & bit)) continue;
        const egg::Step step = egg_held_codes_.Press(pad);
#if defined(KK_DEV_TOOLS)
        REXLOG_INFO("KK dev: easter egg: held code press {} step {}", int(pad), int(step));
#endif
        if (step == egg::Step::kComplete) {
          REXLOG_INFO("KK: easter egg: the original {} code", egg::kHeldCodes[egg_held_codes_.completed()].name);
          start = true;
        }
      }
      input_.ConsumeAll();
      egg_code_.Reset();
      if (start) StartEgg();
      return;
    }
    if (!presses && !keys) return;
    using P = egg::Pad;
    struct Bit {
      uint32_t bit;
      P pad;
      Action action;
    };
    static constexpr Bit kBits[] = {
        {ui::Input::kPadUp, P::kUp, Action::kCount},      {ui::Input::kPadDown, P::kDown, Action::kCount},
        {ui::Input::kPadLeft, P::kLeft, Action::kCount},  {ui::Input::kPadRight, P::kRight, Action::kCount},
        {ui::Input::kPadB, P::kB, Action::kBack},         {ui::Input::kPadA, P::kA, Action::kAccept},
        {ui::Input::kPadStart, P::kStart, Action::kPlay}, {ui::Input::kPadX, P::kX, Action::kCount},
        {ui::Input::kPadY, P::kY, Action::kCount},        {ui::Input::kPadLB, P::kOther, Action::kCount},
        {ui::Input::kPadRB, P::kOther, Action::kCount},   {ui::Input::kPadBack, P::kOther, Action::kCount},
        {ui::Input::kPadLT, P::kOther, Action::kCount},   {ui::Input::kPadRT, P::kOther, Action::kCount},
    };
    for (const Bit& b : kBits) {
      if (!(presses & b.bit)) continue;
      const egg::Step step = egg_code_.Press(b.pad);
      if (egg::Swallowed(b.pad, step)) input_.Consume(b.action);
      if (step == egg::Step::kComplete) StartEgg();
    }
    // The keyboard (Input::key_presses): Enter is the launcher's Accept, kept from it while it is the code's next press.
    using K = egg::Key;
    static constexpr std::pair<uint32_t, K> kKeys[] = {
        {ui::Input::kPadUp, K::kUp}, {ui::Input::kPadDown, K::kDown}, {ui::Input::kPadLeft, K::kLeft},
        {ui::Input::kPadRight, K::kRight}, {ui::Input::kPadB, K::kB}, {ui::Input::kPadA, K::kA},
        {ui::Input::kPadStart, K::kEnter}, {ui::Input::kKeyOther, K::kOther},
    };
    for (const auto& [bit, key] : kKeys) {
      if (!(keys & bit)) continue;
      const egg::Step step = egg_code_.Press(egg::FromKey(key));
      if (egg::KeySwallowed(key, step)) input_.Consume(Action::kAccept);
      if (step == egg::Step::kComplete) StartEgg();
    }
  }

  void StartEgg() {
    const std::filesystem::path exe = egg::AppPath();
    std::error_code ec;
    if (exe.empty() || !std::filesystem::is_regular_file(exe, ec)) {
      REXLOG_WARN("KK: easter egg: launcher\\easter-egg is missing; nothing started");
      return;
    }
    if (!egg_) egg_ = std::make_unique<egg::Process>();
    if (!egg_->Start(exe)) return;
    egg_running_ = true;
    egg_paused_ = egg_quitting_ = false;
    egg_enter_down_ = true;  // the Enter that ended the code counts only once let go
    egg_esc_down_ = true;    // task 364: likewise an Esc held as it starts
  }

  static double SteadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  // TickState (also from the heartbeat while nothing is drawn): the game ended (by itself, or Quit to main menu,
  // which ends it if it has not closed after 3 s): Home again, the launcher in front.
  void TickEgg() {
    if (!egg_running_) return;
    if (egg_ && egg_->Running()) {
      if (!egg_quitting_ || SteadySeconds() - egg_close_at_ < 3.0) return;
      REXLOG_INFO("KK: easter egg did not close within 3 s: ending it");
      egg_->Terminate();
    }
    REXLOG_INFO("KK: easter egg over{}: back to Home", egg_quitting_ ? " (Quit to main menu)" : "");
    egg_running_ = egg_paused_ = egg_quitting_ = false;
    egg_code_.Reset();
    egg_held_codes_.Reset();
    input_.ConsumeAll();
    if (cb_.native_window) egg::ForceToFront(cb_.native_window());
  }

  // The frame while the game runs. False when it has just ended (Home is drawn this frame).
  bool DrawEgg(ImDrawList* dl, ImGuiIO& io) {
    if (music_) music_->Update(0.0f, io.DeltaTime);  // silent while it runs, back after
    FadeSceneSound(0.0f, io.DeltaTime);
    TickState();  // the launcher's jobs go on (and TickEgg)
    if (!egg_running_) return false;
    DrawBackdrop(dl);
    // Task 358: Enter acts as Start. With the game in front the launcher's window gets no keys, so it is read from
    // the system then (only while the game's window is the foreground one); a press the launcher sees itself counts
    // too. A held Enter counts once.
    const bool enter_down = egg::EnterKeyDown();
    const bool enter = (enter_down && !egg_enter_down_ && egg_ && egg_->InFront()) ||
                       (input_.key_presses() & ui::Input::kPadStart);
    egg_enter_down_ = enter_down;
    // Task 364 (owner): Esc pauses it too, read the same way as Enter. In the pause panel Esc is Back (Resume) as
    // before; the Esc that resumed counts once let go, so it does not pause again.
    const bool esc_down = egg::EscapeKeyDown();
    const bool esc = (esc_down && !egg_esc_down_ && egg_ && egg_->InFront()) ||
                     (!egg_paused_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    egg_esc_down_ = esc_down;
    if (egg_paused_) {
      DrawEggPause(dl);
    } else if (!egg_quitting_ && ((input_.pad_presses() & ui::Input::kPadStart) || enter || esc) && egg_ &&
               egg_->WindowShown()) {
      // (only once its window is up: a Start before that does nothing)
      egg_paused_ = true;
      egg_focus_ = 0;
      egg_pause_time_ = ImGui::GetTime();
      const bool front = cb_.native_window && egg::ForceToFront(cb_.native_window());
      REXLOG_INFO("KK: easter egg paused ({}): launcher {}", enter ? "Enter" : esc ? "Esc" : "Start",
                  front ? "in front" : "asked to the front, Windows kept another window in front");
    }
    input_.ConsumeAll();
    return true;
  }

  // The pause panel over the game, as the game's own pause menu looks: the shade, PAUSED, Resume and Quit to
  // main menu; B (Esc) or Start resumes.
  void DrawEggPause(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float t = std::clamp(float(ImGui::GetTime() - egg_pause_time_) / 0.18f, 0.0f, 1.0f);
    const ImVec2 p1(o_.x + w_, o_.y + h_);
    const float x = o_.x + 72 * s, list_w = std::min(400 * s, w_ * 0.42f);
    const float solid = x - o_.x + std::min(500 * s, w_ * 0.5f - 72 * s) + 40 * s;
    dl->AddRectFilled(o_, p1, ui::WithAlpha(color::kBase, t * 0.28f));
    const ImU32 c0 = ui::WithAlpha(color::kBase, t * 0.82f), c1 = ui::WithAlpha(color::kBase, 0.0f);
    dl->AddRectFilled(o_, ImVec2(o_.x + solid, p1.y), c0);
    dl->AddRectFilledMultiColor(ImVec2(o_.x + solid, o_.y), ImVec2(o_.x + solid + w_ * 0.18f, p1.y), c0, c1, c1, c0);
    const ImU32 b0 = ui::WithAlpha(color::kBase, t * 0.55f);
    dl->AddRectFilledMultiColor(ImVec2(o_.x, p1.y - 120 * s), p1, c1, c1, b0, b0);
    const float cy = o_.y + 72 * s;
    ui::DrawTracked(dl, f.display, 15 * s, ImVec2(x, cy - 7.5f * s), color::kAccent, text::Upper(T("Paused")), 5 * s);
    dl->AddRectFilled(ImVec2(x, cy + 17 * s), ImVec2(x + 44 * s, cy + 19 * s), ui::WithAlpha(color::kAccent, 0.7f));
    static const char* const kEntries[2] = {"Resume", "Quit to main menu"};
    const float item_h = 40 * s, fs = 23 * s, tr = 1.6f * s;
    if (input_.Pressed(Action::kUp) || input_.Pressed(Action::kDown)) egg_focus_ = 1 - egg_focus_;
    int chosen = -1;
    float iy = o_.y + 118 * s;
    for (int i = 0; i < 2; ++i) {
      if (ImGui::IsMouseHoveringRect(ImVec2(x - 8 * s, iy), ImVec2(x + list_w, iy + item_h), false)) {
        if (input_.mouse_moved()) egg_focus_ = i;
        if (ImGui::IsMouseClicked(0)) egg_focus_ = chosen = i;
      }
      const bool on = i == egg_focus_;
      egg_offsets_[size_t(i)] = ui::Approach(egg_offsets_[size_t(i)], on ? 1.0f : 0.0f, 14.0f);
      const float k = egg_offsets_[size_t(i)];
      if (k > 0.02f)
        dl->AddRectFilled(ImVec2(x, iy + item_h * 0.5f - fs * 0.55f), ImVec2(x + 4 * s, iy + item_h * 0.5f + fs * 0.55f),
                          ui::WithAlpha(color::kAccent, k));
      ui::DrawTracked(dl, f.display, fs, ImVec2(x + 20 * s + 6 * s * k, iy + (item_h - fs) * 0.5f),
                      on ? color::kText : color::kTextDim, text::Upper(T(kEntries[i])), tr);
      iy += item_h;
    }
    if (input_.Pressed(Action::kAccept)) chosen = egg_focus_;
    if (input_.Pressed(Action::kBack) || input_.Pressed(Action::kPlay)) chosen = 0;
    const Action clicked = ui::DrawPrompts(dl, glyphs_, PromptSet(), {{Action::kAccept, "Select"}, {Action::kBack, "Resume"}},
                                           o_.x + w_ - 72 * s, o_.y + h_ - 48 * s, m_, true);
    if (clicked != Action::kCount) pending_click_ = clicked;
    if (chosen == 0) {
      egg_paused_ = false;
      REXLOG_INFO("KK: easter egg resumed");
      if (egg_) egg_->ToFront();
    } else if (chosen == 1) {
      egg_paused_ = false;
      egg_quitting_ = true;
      egg_close_at_ = SteadySeconds();
      REXLOG_INFO("KK: easter egg: Quit to main menu");
      if (egg_) egg_->RequestClose();
    }
  }

  // ---------------------------------------------------------- heartbeat ---
  // Task 173: the launcher's logic runs in OnDraw, which the SDK calls only while it presents. With the
  // window minimised, or after a "swapchain outdated" frame whose reconnect found no surface to draw to,
  // nothing is presented and nothing retries until a resize, so the setup waited (the shader job finished
  // behind a screen that never moved on). A small thread looks four times a second; once nothing has been
  // drawn for 0.4 s it has the UI thread run the bookkeeping (TickState: the setup's steps and the jobs)
  // and, once a second while the window is not minimised, ask the presenter to reconnect. While the
  // launcher draws normally it posts nothing.
  // Task 209 (the owner's Beta 9 flicker): a reconnect makes a new swapchain, which shows as a flash. It is asked
  // for only when the UI thread is free and frames still do not come (two heartbeats in a row that ran at once,
  // so at least 0.5 s): a UI thread that was merely busy for a moment (a slow disk during a 5 GB import, a
  // settings write, a long frame) used to get a new swapchain every time it came back, which flickered.
  struct Heartbeat {
    std::mutex mutex;
    std::condition_variable wake;
    Launcher* owner = nullptr;
    bool stop = false, queued = false;
    std::atomic<int64_t> last_draw{0};  // steady_clock ticks of the last OnDraw
    std::atomic<int64_t> posted{0};     // when the heartbeat's call was posted to the UI thread
    std::thread thread;
  };
  static int64_t SteadyTicks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

  void StartHeartbeat() {
#if defined(KK_DEV_TOOLS)
    if (std::getenv("KK_DEV_NO_HEARTBEAT")) {  // the behaviour before task 173, for comparison runs
      REXLOG_INFO("KK dev: launcher heartbeat off (KK_DEV_NO_HEARTBEAT)");
      return;
    }
#endif
    if (!cb_.post_to_ui) return;
    auto hb = std::make_shared<Heartbeat>();
    hb->owner = this;
    // The first frame comes a moment after this (the window opening): no reconnect before then, unless
    // nothing has been drawn after 3 s (a launcher started minimised still does its setup).
    hb->last_draw = SteadyTicks() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                        std::chrono::milliseconds(2600)).count();
    hb->thread = std::thread([hb, post = cb_.post_to_ui] {
      const int64_t stale = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::milliseconds(400)).count();
      std::unique_lock lock(hb->mutex);
      while (!hb->stop) {
        hb->wake.wait_for(lock, std::chrono::milliseconds(250));
        if (hb->stop || hb->queued || SteadyTicks() - hb->last_draw.load() < stale) continue;
        hb->queued = true;
        hb->posted = SteadyTicks();
        lock.unlock();  // the UI thread's call takes the lock
        post([hb] {
          Launcher* owner;
          {
            std::lock_guard l(hb->mutex);
            hb->queued = false;
            owner = hb->owner;
          }
          // How long the call waited for the UI thread: long means it was busy, not that frames stopped.
          const double waited = std::chrono::duration<double>(std::chrono::steady_clock::duration(SteadyTicks() - hb->posted.load())).count();
          if (owner) owner->OnHeartbeat(waited);
        });
        lock.lock();
      }
    });
    heartbeat_ = std::move(hb);
  }

  void StopHeartbeat() {
    if (!heartbeat_) return;
    {
      std::lock_guard l(heartbeat_->mutex);
      heartbeat_->owner = nullptr;
      heartbeat_->stop = true;
    }
    heartbeat_->wake.notify_all();
    if (heartbeat_->thread.joinable()) heartbeat_->thread.join();
    heartbeat_.reset();
  }

  // OnDraw: drawing (again).
  void NoteDrawn() {
    exit_evidence::NoteFrame();  // task 229: logs\session-launcher.txt keeps the last frame's time
    if (!heartbeat_) return;
    heartbeat_->last_draw = SteadyTicks();
    if (stalled_since_ != std::chrono::steady_clock::time_point{}) {
      const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - stalled_since_).count();
      REXLOG_INFO("KK: launcher drawing again after {:.1f} s without frames ({} heartbeat ticks, {} reconnects asked)",
                  s, stall_ticks_, stall_reconnects_);
      stalled_since_ = {};
    }
  }

  // The UI thread, nothing drawn for 0.4 s or more. `waited`: seconds the call waited for the UI thread.
  void OnHeartbeat(double waited) {
    if (played_) return;  // the game is starting: nothing left to do here
    const auto now = std::chrono::steady_clock::now();
    if (stalled_since_ == std::chrono::steady_clock::time_point{}) {
      stalled_since_ = now;
      stall_ticks_ = stall_reconnects_ = stall_free_ticks_ = 0;
      last_reconnect_ = {};
      REXLOG_INFO("KK: launcher not drawing (window minimised or its surface lost): setup and jobs go on, the "
                  "presenter is asked to reconnect while the window can be drawn");
    }
    ++stall_ticks_;
    // Busy, not stalled (task 209): the call waited for the UI thread. Frames come again right after it.
    const bool busy = waited > 0.15;
    stall_free_ticks_ = busy ? 0 : stall_free_ticks_ + 1;
    const bool was_setup = InSetup(), was_shaders = ShaderScreen();
    TickState();
    if (was_setup != InSetup() || was_shaders != ShaderScreen())
      REXLOG_INFO("KK: launcher setup moved on without frames (setup {} -> {}, shader screen {} -> {})", was_setup,
                  InSetup(), was_shaders, ShaderScreen());
    if (cb_.recover_presentation && stall_free_ticks_ >= 2 && now - last_reconnect_ >= std::chrono::seconds(1)) {
      last_reconnect_ = now;
      ++stall_reconnects_;
      cb_.recover_presentation();
    }
  }

  // --------------------------------------------------------------- tick ---
  // Content downloads (content.h): the service's user folder, and a download that was running when the
  // launcher closed goes on. Developer builds: KK_DEV_CONTENT_URL_TEX / _CUT point at a local server
  // (same files, so the same SHA-256), KK_DEV_CONTENT_DROP_AT=<bytes> loses the connection once.
  void StartContent() {
    content::SetLog([](bool warning, const std::string& line) {
      if (warning) REXLOG_WARN("{}", line);
      else REXLOG_INFO("{}", line);
    });
    auto& service = content::Service::Get();
#if defined(KK_DEV_TOOLS)
    if (const char* v = std::getenv("KK_DEV_CONTENT_URL_TEX"); v && *v)
      service.OverrideUrl(content::Kind::kTexturePack, v, content::InfoFor(content::Kind::kTexturePack).sha256);
    if (const char* v = std::getenv("KK_DEV_CONTENT_URL_CUT"); v && *v)
      service.OverrideUrl(content::Kind::kCutscenes, v, content::InfoFor(content::Kind::kCutscenes).sha256);
    if (const char* v = std::getenv("KK_DEV_CONTENT_URL_ART"); v && *v)
      service.OverrideUrl(content::Kind::kArtbook, v, content::InfoFor(content::Kind::kArtbook).sha256);
    if (const char* v = std::getenv("KK_DEV_CONTENT_URL_MANUAL"); v && *v)  // task 333
      service.OverrideUrl(content::Kind::kManual, v, content::InfoFor(content::Kind::kManual).sha256);
    for (const content::ModPack& pack : content::kModPacks) {  // KK_DEV_CONTENT_URL_UHD, KK_DEV_CONTENT_URL_COMIC
      std::string name = "KK_DEV_CONTENT_URL_";
      for (const char* k = pack.key; *k; ++k) name += char(std::toupper(static_cast<unsigned char>(*k)));
      if (const char* v = std::getenv(name.c_str()); v && *v) service.OverrideUrl(pack.kind, v, content::InfoFor(pack.kind).sha256);
    }
#endif
    // Beta 7 artbook: each picture that comes off the bonus disc must decode (WIC on Windows, stb_image on Linux).
    content::SetPictureCheck([](const std::filesystem::path& file) {
      std::vector<uint8_t> bytes;
      return artbook::ReadBytes(file, bytes, artbook::kMaxPictureBytes) && bool(workshop::DecodePicture(bytes, 256));
    });
    service.Init(paths_.user_dir);
    // Job J3: a download paused because the game was started (PlayWithContent) goes on now.
    if (service.ResumeAfterPlay()) REXLOG_INFO("KK content: download paused for the game continues");
  }

  // Each tick (also from the heartbeat while minimised): the settings an install or Remove changes, a
  // chosen ZIP to import, a status line when something finished. Progress is logged by the service's own
  // thread: a log write from the UI thread can wait for the disk while 5 GB are being unpacked.
  void TickContent() {
    auto& service = content::Service::Get();
    if (!service.Ready()) return;
    EnsureWorkshop();  // task 209: the texture pack rule needs the mods scan (its own worker)
    for (const auto& c : service.TakeChanges()) {
      // Task 209: the service's events (content.h Change): a texture pack mod installed becomes the one texture pack
      // on; a mod installed or removed is read again (and a removed one leaves the load order).
      if (!c.name.empty() && c.name[0] == '@') {
        REXLOG_INFO("KK content: {} {}", c.name, c.value);
        if (c.name == "@texture-pack-mod") workshop_->OnTexturePackModInstalled(c.value);
        else if (c.name == "@mod-installed") workshop_->OnModsChanged({});
        else if (c.name == "@mod-removed") workshop_->OnModsChanged(c.value);
        continue;
      }
      if (!c.when_name.empty() && Get(c.when_name.c_str()) != c.when_value) continue;
      if (c.reset) Reset(c.name.c_str());
      else Set(c.name.c_str(), c.value);
      REXLOG_INFO("KK content: setting {} = {}", c.name, c.reset ? std::string("(default)") : c.value);
      // The HD texture pack installed and turned on: the only texture pack on (texture pack mods off).
      if (c.name == "native_texture_pack" && c.value == "true") workshop_->OnFolderPackOn();
    }
    {
      std::string folder;
      {
        std::lock_guard lock(texture_pack_folder_choice.mutex);
        folder.swap(texture_pack_folder_choice.selected);
      }
      if (!folder.empty()) {  // the player's own folder: remembered, and switched on (the only one on)
        Set("kk_texture_pack_own_folder", folder);
        ++g_changes;
        REXLOG_INFO("KK content: your texture pack folder {}", folder);
        workshop_->EnableTexturePack("folder");
      }
    }
    {
      std::string zip;
      content::ImportTarget target;
      {
        std::lock_guard lock(texture_pack_zip_choice.mutex);
        zip.swap(texture_pack_zip_choice.selected);
        target = texture_pack_zip_choice.target;
      }
      if (!zip.empty()) {
        const auto before = service.Get(content::Kind::kImport);
        if (!before.Busy()) {
          service.Import(std::filesystem::path(std::u8string(zip.begin(), zip.end())), target);
          // The progress pop-up follows the install; on a setup page, Run in background goes on to the next step.
          REXLOG_INFO("KK content: importing the chosen ZIP");
          OpenContentProgress(content::Kind::kImport, content_import_setup_);
        } else {
          Status("Another import is running. Try again when it has finished.");
        }
      }
    }
    workshop_->Tick();
    for (int i = 0; i < content::kKindCount; ++i) {
      const auto kind = content::Kind(i);
      const auto st = service.Get(kind);
      auto& last = content_last_[i];
      if (content::Quiet(kind)) {
        if (st.stage != last) {
          if (kind == content::Kind::kManual) ManualStageChanged(st, last);
          else ArtbookStageChanged(st, last);
        }
        last = st.stage;
        continue;
      }
      if (st.stage != last) {
        const bool tex = kind == content::Kind::kTexturePack;
        // Not while it unpacks: a log write from the UI thread can wait for the busy disk (REPORT-181).
        if (st.stage != content::Stage::kUnpacking)
          REXLOG_INFO("KK content: {} stage {} -> {}", content::InfoFor(kind).id, int(last), int(st.stage));
        if (st.stage == content::Stage::kInstalled && last != content::Stage::kNone) {
          // Task 212: a Workshop download or an import says its own name (it said the PS2 cutscenes before).
          const content::ModPack* pack = content::ModPackFor(kind);
          Status(tex ? std::string("The HD texture pack is installed and turned on.")
                 : pack ? std::string(pack->title) + " installed"
                 : kind == content::Kind::kImport
                     ? std::string(st.target == content::ImportTarget::kMod ? "Mod installed" : "Texture pack installed")
                     : std::string("The PS2 cutscenes are restored and turned on."));
          ContentEnded(kind, st);
        }
        if (st.stage == content::Stage::kNone && last == content::Stage::kRemoving) Status("Removed.");
        if (st.stage == content::Stage::kFailed) {
          REXLOG_WARN("KK content: {} failed: {}", content::InfoFor(kind).id, ContentError(st, tex));
          ContentEnded(kind, st);
        }
        last = st.stage;
      }
    }
    TickArtbook();
    TickManual();
    // The progress pop-up: closed by something else (forget it), its buttons changed with the stage (open
    // it again with the same focus), or its job cancelled (close it).
    if (content_watch_ >= 0) {
      const auto kind = content::Kind(content_watch_);
      const auto st = service.Get(kind);
      if (!modal_.open || modal_.title != content_watch_title_) {
        content_watch_ = -1;
      } else if (st.stage == content::Stage::kNone) {
        modal_.open = false;
        content_watch_ = -1;
      } else if (const int mode = ContentWatchMode(st, kind == content::Kind::kImport);
                 mode != content_watch_mode_) {
        // Pause and Continue swap in place (same buttons, same focus); otherwise the focus goes to Run in
        // background.
        OpenContentProgress(kind, content_watch_setup_, mode <= 1 && content_watch_mode_ <= 1 ? modal_.focus : -1);
      }
    }
    if (!content_popups_.empty() && !modal_.open && !ShaderScreen()) {
      auto popup = std::move(content_popups_.front());
      content_popups_.erase(content_popups_.begin());
      popup();
    }
    TickSetupContent();
  }

  // ------------------------------------------------ Beta 7: the artbook ---
  // The bonus disc downloads by itself, quietly (no pop-up, strip or Play question), at the end of the first setup
  // and on the first start after an update, when the artbook is not installed (DECISIONS 17 Oct "Beta 7 Extras",
  // ROADMAP Beta 7). Remembered in content\downloads\artbook.auto until it is installed: a failed try is tried
  // again once more in the same session after a while, and at the next starts; after kArtbookTries failed tries
  // in a row, one pop-up says so. Developer builds start it only with KK_DEV_CONTENT_URL_ART (a local server) or
  // KK_DEV_ARTBOOK_AUTO=1, so test runs never fetch the disc from archive.org by accident.
  static constexpr int kArtbookTries = 3;
  std::string artbook_want_;       // why it should download now (set before the content service is ready)
  bool artbook_checked_ = false;   // the remembered wish was looked at this session
  bool artbook_auto_run_ = false;  // the running download was started automatically
  double artbook_retry_at_ = 0;    // ImGui time of the retry in this session (0: none)

  static bool ArtbookAutoAllowed() {
#if defined(KK_DEV_TOOLS)
    const char* url = std::getenv("KK_DEV_CONTENT_URL_ART");
    const char* on = std::getenv("KK_DEV_ARTBOOK_AUTO");
    return (url && *url) || (on && std::string_view(on) == "1");
#else
    return true;
#endif
  }
  void StartArtbookAuto(const std::string& why) {
    if (!ArtbookAutoAllowed()) {
      REXLOG_INFO("KK artbook: automatic download not started ({}): developer build without KK_DEV_CONTENT_URL_ART "
                  "or KK_DEV_ARTBOOK_AUTO=1", why);
      return;
    }
    REXLOG_INFO("KK artbook: downloading the bonus disc in the background ({})", why);
    artbook_auto_run_ = true;
    content::Service::Get().Download(content::Kind::kArtbook);
  }
  void TickArtbook() {
    auto& service = content::Service::Get();
    const content::Status st = service.Get(content::Kind::kArtbook);
    const std::filesystem::path& user = paths_.user_dir;
    if (!artbook_want_.empty()) {
      const std::string why = std::exchange(artbook_want_, std::string());
      if (st.installed) return;
      artbook::AutoState a = artbook::ReadAuto(user);
      if (!a.wanted) {
        a = {true, 0, false};
        artbook::WriteAuto(user, a);
      }
      artbook_checked_ = true;
      if (st.Busy()) artbook_auto_run_ = true;
      else if (a.fails < kArtbookTries) StartArtbookAuto(why);
      return;
    }
    if (!artbook_checked_) {  // once a session: wanted at an earlier start and not installed yet
      artbook_checked_ = true;
      const artbook::AutoState a = artbook::ReadAuto(user);
      if (a.wanted && st.installed) artbook::ClearAuto(user);
      else if (a.wanted && st.Busy()) artbook_auto_run_ = true;  // continued by the service as it started
      else if (a.wanted && a.fails < kArtbookTries) StartArtbookAuto("not finished at an earlier start");
    }
    if (artbook_retry_at_ > 0 && ImGui::GetTime() >= artbook_retry_at_) {
      artbook_retry_at_ = 0;
      if (!st.installed && !st.Busy()) StartArtbookAuto("trying again");
    }
  }
  void ArtbookStageChanged(const content::Status& st, content::Stage last) {
    using content::Stage;
    if (st.stage != Stage::kUnpacking) REXLOG_INFO("KK content: artbook stage {} -> {}", int(last), int(st.stage));
    if (st.stage == Stage::kInstalled && last != Stage::kNone) {
      artbook::ClearAuto(paths_.user_dir);
      artbook_auto_run_ = false;
      artbook_retry_at_ = 0;
      REXLOG_INFO("KK artbook: ready in Play > Extras > Artbook");
      return;
    }
    if (st.stage != Stage::kFailed) return;
    REXLOG_WARN("KK artbook: download failed: {}", ContentError(st, false));
    if (!std::exchange(artbook_auto_run_, false)) return;  // started on the Artbook page: the page shows it
    artbook::AutoState a = artbook::ReadAuto(paths_.user_dir);
    a.wanted = true;
    ++a.fails;
    if (a.fails < kArtbookTries) {
      double wait = 90.0;
#if defined(KK_DEV_TOOLS)
      if (const char* v = std::getenv("KK_DEV_ARTBOOK_RETRY_S"); v && *v) wait = std::atof(v);  // tests: shorter
#endif
      artbook_retry_at_ = ImGui::GetTime() + wait;
      REXLOG_INFO("KK artbook: try {} of {} failed; trying again in {:.0f} s", a.fails, kArtbookTries, wait);
    } else if (!a.told) {
      a.told = true;
      REXLOG_INFO("KK artbook: {} tries failed: telling the player", a.fails);
      content_popups_.push_back([this] {
        OpenModal("Artbook download failed",
                  "The artbook could not be downloaded from archive.org after several tries. You can try again now, "
                  "or later in Play > Extras > Artbook.",
                  {"Try again", "Later"}, 1, [this](int i) {
                    if (i != 0) return;
                    artbook::WriteAuto(paths_.user_dir, {true, 0, false});
                    REXLOG_INFO("KK artbook: try again");
                    artbook_auto_run_ = true;
                    content::Service::Get().Download(content::Kind::kArtbook);
                  });
      });
    }
    artbook::WriteAuto(paths_.user_dir, a);
  }

  // ------------------------------------------- task 333: the user manual ---
  // The game's Xbox 360 manual (owner, 20 Oct, Beta 13): downloaded quietly in the background at run time, with the
  // artbook's handling: once a session while it is not installed (from the first launcher start on), a failed try
  // again after a while in the same session and at the next starts, and after kManualTries failed tries in a row a
  // pop-up (Try again / Later); Extras > User manual shows its progress and has Download now. Remembered in
  // content\downloads\manual.auto until it is installed. Developer builds only with KK_DEV_CONTENT_URL_MANUAL or
  // KK_DEV_MANUAL_AUTO=1.
  static constexpr int kManualTries = 3;
  bool manual_checked_ = false;   // looked at this session
  bool manual_auto_run_ = false;  // the running download was started automatically
  double manual_retry_at_ = 0;    // ImGui time of the retry in this session (0: none)
  std::string manual_want_;       // developer step: start now
  static bool ManualAutoAllowed() {
#if defined(KK_DEV_TOOLS)
    const char* url = std::getenv("KK_DEV_CONTENT_URL_MANUAL");
    const char* on = std::getenv("KK_DEV_MANUAL_AUTO");
    return (url && *url) || (on && std::string_view(on) == "1");
#else
    return true;
#endif
  }
  void StartManualAuto(const std::string& why) {
    if (!ManualAutoAllowed()) {
      REXLOG_INFO("KK manual: automatic download not started ({}): developer build without KK_DEV_CONTENT_URL_MANUAL "
                  "or KK_DEV_MANUAL_AUTO=1", why);
      return;
    }
    REXLOG_INFO("KK manual: downloading the user manual in the background ({})", why);
    manual_auto_run_ = true;
    content::Service::Get().Download(content::Kind::kManual);
  }
  void TickManual() {
    auto& service = content::Service::Get();
    const content::Status st = service.Get(content::Kind::kManual);
    const std::filesystem::path& user = paths_.user_dir;
    if (!manual_checked_ || !manual_want_.empty()) {
      manual_checked_ = true;
      const std::string why = manual_want_.empty() ? "not downloaded yet" : std::exchange(manual_want_, std::string());
      artbook::AutoState a = artbook::ReadAuto(user, "manual");
      if (st.installed) {
        if (a.wanted) artbook::ClearAuto(user, "manual");
      } else if (st.Busy()) {
        manual_auto_run_ = true;  // continued by the service as it started
      } else if (a.fails < kManualTries) {
        if (!a.wanted) artbook::WriteAuto(user, {true, a.fails, a.told}, "manual");
        StartManualAuto(why);
      }
    }
    if (manual_retry_at_ > 0 && ImGui::GetTime() >= manual_retry_at_) {
      manual_retry_at_ = 0;
      if (!st.installed && !st.Busy()) StartManualAuto("trying again");
    }
  }
  void ManualStageChanged(const content::Status& st, content::Stage last) {
    using content::Stage;
    if (st.stage != Stage::kUnpacking) REXLOG_INFO("KK content: manual stage {} -> {}", int(last), int(st.stage));
    if (st.stage == Stage::kInstalled && last != Stage::kNone) {
      artbook::ClearAuto(paths_.user_dir, "manual");
      manual_auto_run_ = false;
      manual_retry_at_ = 0;
      REXLOG_INFO("KK manual: ready in Play > Extras > User manual");
      return;
    }
    if (st.stage != Stage::kFailed) return;
    REXLOG_WARN("KK manual: download failed: {}", ContentError(st, false));
    if (!std::exchange(manual_auto_run_, false)) return;  // started on the manual's page: the page shows it
    artbook::AutoState a = artbook::ReadAuto(paths_.user_dir, "manual");
    a.wanted = true;
    ++a.fails;
    if (a.fails < kManualTries) {
      double wait = 90.0;
#if defined(KK_DEV_TOOLS)
      if (const char* v = std::getenv("KK_DEV_ARTBOOK_RETRY_S"); v && *v) wait = std::atof(v);  // tests: shorter
#endif
      manual_retry_at_ = ImGui::GetTime() + wait;
      REXLOG_INFO("KK manual: try {} of {} failed; trying again in {:.0f} s", a.fails, kManualTries, wait);
    } else if (!a.told) {
      a.told = true;
      REXLOG_INFO("KK manual: {} tries failed: telling the player", a.fails);
      content_popups_.push_back([this] {
        OpenModal("Manual download failed",
                  "The user manual could not be downloaded after several tries. You can try again now, or later in "
                  "Play > Extras > User manual.",
                  {"Try again", "Later"}, 1, [this](int i) {
                    if (i != 0) return;
                    artbook::WriteAuto(paths_.user_dir, {true, 0, false}, "manual");
                    REXLOG_INFO("KK manual: try again");
                    manual_auto_run_ = true;
                    content::Service::Get().Download(content::Kind::kManual);
                  });
      });
    }
    artbook::WriteAuto(paths_.user_dir, a, "manual");
  }

  void SaveIfChanged() {
    if (saved_changes_ == g_changes) return;
    SaveSettings(paths_.config_path);
    saved_changes_ = seen_changes_ = g_changes;
  }

  // Bookkeeping each frame: downloads, the install, pop-ups that open by
  // themselves, and saving settings a moment after they change.
  void Tick() {
    CaptureKey();
    TickNameEditor();
    TickState();
    PreviewAudio();
  }

  // The part of Tick that needs no frame (no input, no drawing): the setup's steps, the jobs (install,
  // artwork, shaders, downloads) and what follows them, the pop-ups they open, saving the settings. It also
  // runs from the heartbeat while nothing is drawn (task 173), so a minimised launcher's setup goes on.
  void TickState() {
    const double now = ImGui::GetTime();
    if (!played_ && !InSetup() && !ShaderScreen() && CanPlay()) {
      play::StartRequest launch;
      if (coop::TakeSessionLaunch(launch)) { play::SetStartRequest(launch); Play(); return; }
      // C80: a co-op start plan both games agreed on: Play restarts into the game, which reconnects.
      if (coop::TakeHandover()) { play::SetStartRequest({}); Play(); return; }
    }
#if defined(KK_DEV_TOOLS)
    DevNotifyTest();
#endif
    FlushProfileOptions(false);
    TickEgg();
    FinishInstallIfDone();
    FinishArtIfDone();
    RefreshPack();
    TickContent();
    TickUpdate();
    TickLaunchTime();
    TickFinish();
    TickSteam();
    // Saved crash reports never open a pop-up (owner, 19 Oct: it showed on every start and covered the
    // benchmark results); they are still saved and uploaded as before, and the Settings Diagnostics row
    // "Saved crash reports" opens their folder.
    // What's new only once the game is playable (owner, 16 Oct): never during the first-time setup (the
    // game files asked for, installed, the artwork, the shaders, the setup pages up to Finish setup), nor over the
    // New in this update screen (job J6), nor while shaders are prepared (that loads every processor core and the
    // launcher answers slowly meanwhile). launcher_policy.h.
    if (open_whats_new_ &&
        ShowWhatsNewNow({open_whats_new_, modal_.open, files_ok_, InSetup() || MoviesPending(), update_new_, ShaderScreen(), native_pack_.Busy(),
                         NativeShaderPack::Wanted(), native_pack_.Finished() || ShadersPreparedForThisVersion()})) {
      open_whats_new_ = false;
      whats_new_shown_ = true;
      OpenWhatsNew();
    }
    TickNativeShaders();
    TickSceneStart();  // task 330
    TickUpscalerQuery();
    if (waiting_for_pack_ && !pack_.busy) {
      waiting_for_pack_ = false;
      modal_.open = false;
      StartGame();
    }
    TickPlayWithContent();
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
    for (const auto& [on, name] : {std::pair{c.dlss, "dlss"}, std::pair{c.fsr4 && Fsr4AllowedOnThisGpu(), "fsr4"},
                                   std::pair{c.fsr3, "fsr3"},
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
  // The one shader preparation of this version has finished (first install, update or Rebuild) and its packs
  // are in the cache: shader_prepared.h, the ONE definition the setup's end, Home, What's new and Play share. It
  // is not started again until the next version or the player asks.
  bool ShadersPreparedForThisVersion() const {
    // Home asks every frame: the cache folder is looked at again when the marker changes or after a second,
    // not at every frame.
    const std::string marker = Get("kk_shaders_prepared");
    const auto now = std::chrono::steady_clock::now();
    if (marker != prepared_marker_ || now - prepared_checked_ > std::chrono::seconds(1)) {
      std::error_code ec;
      const auto dir = CacheDir() / "native-shaders";
      int present = 0;
      for (int pack : {kShaderPackSpirv, kShaderPackDxil})
        if (std::filesystem::is_regular_file(dir / ShaderPackFileName(pack), ec)) present |= pack;
      prepared_ = ShadersPrepared(marker, KK_VERSION, present);
      prepared_marker_ = marker;
      prepared_checked_ = now;
    }
    return prepared_;
  }

  void TickNativeShaders() {
    // Each job, once, as it ends: the version's one preparation only when it ran to its end and left every pack
    // it covers (a cancelled job, e.g. the launcher closed during the setup's shaders, is not one). Saved at
    // once, so a launcher closed or a game started right after it still finds it.
    if (native_pack_.TakeFinished()) {
      const ShaderJobEnd end = native_pack_.End();
      if (JobIsThePreparation(end)) {
        Set("kk_shaders_prepared", FormatPreparedMarker(KK_VERSION, end.packs_needed));
        SaveSettings(paths_.config_path);
        REXLOG_INFO("KK: shaders prepared for v{} ({}, {}); not prepared again until an update or Rebuild", KK_VERSION,
                    Get("kk_shaders_prepared"), native_pack_.Ok() ? "every pipeline" : "some pipelines left to the game");
      } else {
        REXLOG_WARN("KK: shader job ended without preparing the shaders ({}packs needed {} ready {}); prepared: {}",
                    end.cancelled ? "cancelled, " : "", end.packs_needed, end.packs_ready,
                    ShadersPreparedForThisVersion() ? "yes (an earlier preparation)" : "no");
      }
    }
    if (setup_shaders_ && !native_pack_.Busy()) {
      setup_shaders_ = false;
      if (!InSetup()) Status(InstalledMessage());  // else once the setup is finished (FinishSetup)
    }
    if (rebuild_shaders_screen_ && !native_pack_.Busy()) {
      rebuild_shaders_screen_ = false;
      Status(ShadersPreparedForThisVersion() ? "The shaders are ready."
                                             : "The shaders were not prepared: the game compiles them as it starts.");
    }
    if (update_shaders_ && !native_pack_.Busy()) {
      update_shaders_ = false;
      Status(ShadersPreparedForThisVersion() ? F("Updated to v{0}. The shaders are ready.", {KK_VERSION})
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
        {kCheats, kNoSubPage, "cheats"},     {kControls, kNoSubPage, "controls"},
        {kControls, kRemapPage, "remap"},    {kAdvanced, kNoSubPage, "advanced"},
        {kAchievements, kNoSubPage, "achievements"}, {kAbout, kNoSubPage, "about"}};
    constexpr int kCount = int(std::size(kSteps));
    if (step < kCount) {
      page_ = kSteps[step].page;
      about_in_settings_ = page_ == kAbout;
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
  // "setup:N" shows setup stage N (SetupStage numbers; 14 New in this update); "reset-beta" does to the settings what switching to
  // stable does (and saves them); "type:<text>" types text (New game's name); "whats-new" opens What's new as after 1.9.11; "update-test"
  // logs the update choice cases (RunUpdatePickTests); "updated" acts as the first start after an
  // update (the full shader rebuild and its screen); "setup:4" shows the setup's shader screen.
  // The made-up download end of "notify-test" (the real path: decision, ding, system notification, pop-up),
  // then the notification centre's count 3 s later. Steady clock: ImGui's time stops while minimised.
  void DevNotifyTest() {
    using clock = std::chrono::steady_clock;
    if (dev_notify_at_ != clock::time_point{} && clock::now() >= dev_notify_at_) {
      dev_notify_at_ = {};
      dev_history_at_ = clock::now() + std::chrono::seconds(3);
      const std::string title = "King Kong Recompiled test notification";
      const std::string body = dev_notify_ok_
                                   ? "Test 2 of 3 from the launcher agent: HD texture pack installed. No action needed."
                                   : "Test 2 of 3 from the launcher agent: download failed. No action needed.";
      REXLOG_INFO("KK dev: notify test fires ({})", dev_notify_ok_ ? "finished" : "failed");
      NotifyContentDone(content::Kind::kTexturePack, dev_notify_ok_, title, body);
      content_popups_.push_back([this, title, body] { OpenModal(title, body, {"OK"}, 0, {}); });
    }
    if (dev_history_at_ != clock::time_point{} && clock::now() >= dev_history_at_) {
      dev_history_at_ = {};
      notify::LogHistory();
    }
  }
  std::chrono::steady_clock::time_point dev_notify_at_{}, dev_history_at_{};
  bool dev_notify_ok_ = true;
  ImGuiKey dev_key_up_ = ImGuiKey_None;  // task 358: the key a "key:" step pressed (let go next frame)

  void DevInput() {
    ImGuiIO& io = ImGui::GetIO();
    // Scripted runs wait for the setup's work (only captures go on); the pages that ask something take input.
    const bool waiting = std::getenv("KK_DEV_LAUNCHER_INPUT_FILE") && (InSetup() || ShaderScreen()) &&
                         (ShaderScreen() || !AskingSetupStage(SetupStage()));
    static bool release = false;
    if (release) {
      io.AddMouseButtonEvent(0, false);
      release = false;
    }
    if (dev_key_up_ != ImGuiKey_None) {  // task 358: the key a "key:" step pressed is let go the next frame
      io.AddKeyEvent(dev_key_up_, false);
      dev_key_up_ = ImGuiKey_None;
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
    // Job J2: "wait-setup:N" holds the steps until the setup shows stage N, "wait-home" until the setup is
    // over, "wait-content" until no download or install is running, waiting or paused.
    if (const std::string& w = steps[next]; w.rfind("wait-", 0) == 0) {
      bool met = true;
      if (w.rfind("wait-setup:", 0) == 0) met = InSetup() && SetupStage() == std::atoi(w.c_str() + 11);
      else if (w == "wait-home") met = !InSetup() && !ShaderScreen() && !loop_ && !MoviesPending();  // task 331/332: the loop and movies gone
      else if (w == "wait-movies") met = movies_ != nullptr;  // task 332: the startup movies are up
      else if (w == "wait-modal") met = modal_.open;  // a pop-up is up (logged with its title)
      else if (w == "wait-files") met = FinishScreen();  // task 370: the first start check downloads
      else if (w == "wait-files-done") met = finish_ && !finish_->busy && (finish_->done || finish_->failed);
      else if (w == "wait-artbook") {  // Beta 7: the artbook installed, or its download failed
        const auto st = content::Service::Get().Get(content::Kind::kArtbook);
        met = st.installed || st.stage == content::Stage::kFailed;
      }
      else if (w == "wait-manual") {  // task 333: the user manual installed, or its download failed
        const auto st = content::Service::Get().Get(content::Kind::kManual);
        met = st.installed || st.stage == content::Stage::kFailed;
      }
      else if (w == "wait-unpack") {  // job J3: a download is being unpacked
        met = false;
        for (int i = 0; i < content::kKindCount; ++i)
          met = met || content::Service::Get().Get(content::Kind(i)).stage == content::Stage::kUnpacking;
      }
      else if (w == "wait-content") {
        met = true;  // (its finished pop-ups may be waiting behind each other)
        for (int i = 0; i < content::kKindCount; ++i) {
          const auto st = content::Service::Get().Get(content::Kind(i));
          if (st.Busy() || st.stage == content::Stage::kPaused) met = false;
        }
      }
      if (!met) return;
    }
    std::string t = steps[next++];
    at = now + 0.45;
    REXLOG_INFO("KK dev: input {}", t.rfind("direct:",0)==0 ? "direct panel action" : t);
    if (t.rfind("direct:",0)==0) {
      if (!social_.DirectInput(t.substr(7),coop::Social())) { --next; at=now+.5; }
      return;
    }
    // C81: "coop:<page>" opens a co-op page; "coop:wait:<page>" waits until that page shows.
    if (t.rfind("coop:",0)==0) {
      if (!CoopDevInput(t.substr(5))) { --next; at=now+.3; }
      return;
    }
    // Same SessionStart path as the Social Start action, no direct game boot.
    if (t.rfind("session:",0)==0) {
      const auto value=t.substr(8);
      coop::SelectSessionStart(value=="continue" ? play::StartRequest{play::StartKind::kContinue,-1,-1,{}} :
          play::StartRequest{play::StartKind::kChapter,ChapterSlot(),std::atoi(value.c_str()),{}});
      // Automated sweeps use the real Direct IP lobby, which requires the host
      // to ready the displayed choice as well as the guest.
      const auto model=coop::Social().Snapshot();
      if (model.choice.valid()) coop::Social().Act({coop::SocialAction::SetReady,0,true,model.choice});
      if (!coop::RequestSessionStart()) { --next; at=now+0.5; }
      return;
    }
    // Preview states only touch the SDK-free fake selected at startup.
    if (t.rfind("social:", 0) == 0) {
      // Appended command files may contain trailing whitespace. Do not silently ignore a failed scene.
      const size_t end = t.find_last_not_of(" \t\r\n");
      const std::string scene = end == std::string::npos ? "" : t.substr(7, end >= 7 ? end - 6 : 0);
      if (coop::SocialFakeScenario(scene)) {
        social_.ResetPreview();
        const auto model = coop::Social().Snapshot();
        REXLOG_INFO("KK dev: Social scene {} applied: status {} lobby {} host {} members {} ready {}", scene,
                    int(model.status), model.lobby != 0, model.lobby != 0 && model.owner == model.self,
                    model.members.size(), model.can_start);
      } else {
        REXLOG_WARN("KK dev: Social scene failed (requires co-op and --kk_social_fake=1)");
      }
      return;
    }
    if (t == "wait-modal") REXLOG_INFO("KK dev: pop-up '{}' buttons {}", modal_.title, modal_.buttons.size());
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
    if (t.rfind("benchmark:", 0) == 0) {  // as the benchmark pop-up's Quick (1) / Standard (3) run
      // Not repeated by the launcher the finished run returns to.
#if defined(_WIN32)
      _putenv_s("KK_DEV_LAUNCHER_INPUT", "shot:returned-benchmark");
#else
      setenv("KK_DEV_LAUNCHER_INPUT", "shot:returned-benchmark", 1);
#endif
      StartBenchmark(std::clamp(std::atoi(t.c_str() + 10), 1, 5));
      return;
    }
    if (t == "page:settings") {  // as Home's Settings: the last Settings tab shown
      GoTo(settings_page_, /*settings_tab=*/true);
      return;
    }
    if (t == "focused") {  // the page, whether it is a Settings tab, and its focused row
      const auto& state = play_ != PlayView::kNone ? play_lists_[int(play_)] : lists_[page_ * 4 + sub_];
      REXLOG_INFO("KK dev: launcher focused page {} tab {} row {} label {}",
                  play_ != PlayView::kNone ? PlayViewName(play_) : kPageNames[page_], OnSettingsTab() ? 1 : 0,
                  state.focus, focused_ ? focused_->label : std::string("-"));
      return;
    }
    if (t.rfind("artbook:", 0) == 0) {  // Beta 7: artbook:<page|auto|state|open:N|next|prev|zoom:X|pan:DX:DY|save|back|download>
      const std::string c = t.substr(8);
      if (c == "page") {
        OpenPlayMenu();
        OpenPlayView(PlayView::kExtras);
        OpenPlayView(PlayView::kArtbook);
      } else if (c == "auto") {
        artbook_want_ = "developer step";
      } else if (artbook_) {
        artbook_->DevCommand(c);
      }
      return;
    }
    if (t.rfind("manual:", 0) == 0) {  // task 333: manual:<page|auto|state|open:N|next|prev|zoom:X|pan:DX:DY|back|download>
      const std::string c = t.substr(7);
      if (c == "page") {
        OpenPlayMenu();
        OpenPlayView(PlayView::kExtras);
        OpenPlayView(PlayView::kManual);
      } else if (c == "auto") {
        manual_want_ = "developer step";
      } else if (c == "original-menu") {  // the Original main menu row's pop-up
        OpenPlayMenu();
        OpenPlayView(PlayView::kExtras);
        AskOriginalMenu();
      } else if (manual_) {
        manual_->DevCommand(c);
      }
      return;
    }
    if (t.rfind("workshop:", 0) == 0) {  // job J8: workshop:<state|open:N|tile:N|mods|download|github|link:N|links|scroll:PX>
      if (workshop_) workshop_->DevCommand(t.substr(9));
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
    if (t == "home-play") {  // task 357: Home's focus on Play, as when Home opens
      home_focus_ = 0;
      side_focus_ = -1;
      status_focus_ = false;
      return;
    }
    if (t.rfind("focus:", 0) == 0) {
      const bool gfx = InSetup() && GraphicsSetupStage(SetupStage());
      const bool setup_settings = InSetup() && SettingsSetupStage(SetupStage());  // task 332
      for (size_t i = 0; i < focused_items_.size(); ++i)
        if (focused_items_[i].label == t.substr(6) && focused_items_[i].type != ItemType::kSection) {
        auto& state = gfx ? gfx_lists_[GfxPage()] : setup_settings ? setup_lists_[SetupSettingsPage() * 4 + sub_]
                      : play_ != PlayView::kNone ? play_lists_[int(play_)] : lists_[page_ * 4 + sub_];
        if (gfx) gfx_on_cards_ = false;
        state.focus = int(i);
        state.focus_label = focused_items_[i].label;
        REXLOG_INFO("KK dev: launcher focused {} value {} help {}", state.focus_label, focused_items_[i].value_text,
                    text::T(focused_items_[i].description));
        return;
      }
      REXLOG_INFO("KK dev: launcher focus FAIL {}", t.substr(6));
      return;
    }
    if (t == "rows" && InSetup() && GraphicsSetupStage(SetupStage())) {  // job J4: a graphics page's cards too
      for (const auto& c : GfxCards(GfxPage()))
        REXLOG_INFO("KK dev: setup graphics card {} {}{}", c.label, c.selected ? "selected" : "-", c.enabled ? "" : " (not offered)");
      LogGraphicsChoices("rows");
    }
    if (t == "rows") {
      // With the shown value (a choice's option, else its value text), so a test can read settings back.
      for (const auto& row : focused_items_) {
        const bool choice = row.index >= 0 && row.index < int(row.options.size());
        REXLOG_INFO("KK dev: launcher page {} row {} = {}",
                    play_ != PlayView::kNone ? PlayViewName(play_)
                    : InSetup() && SettingsSetupStage(SetupStage())
                        ? (std::string("setup ") + kPageNames[kSetupSettingsPages[SetupSettingsPage()]]).c_str()
                        : kPageNames[page_],
                    row.label,
                    choice ? row.options[size_t(row.index)] : row.value_text);
      }
      return;
    }
    if (t.rfind("content:", 0) == 0) {  // content:<download|pause|cancel|remove>:<tex|cut>, content:import:<zip>, content:status
      auto& service = content::Service::Get();
      const std::string rest = t.substr(8);
      const size_t colon = rest.find(':');
      const std::string verb = rest.substr(0, colon), arg = colon == std::string::npos ? "" : rest.substr(colon + 1);
      const auto kind = arg == "cut" ? content::Kind::kCutscenes : content::Kind::kTexturePack;
      if (verb == "download") service.Download(kind);
      else if (verb == "pause") service.Pause(kind);
      else if (verb == "cancel") service.Cancel(kind);
      else if (verb == "remove") service.Remove(kind);
      else if (verb == "import") service.Import(std::filesystem::path(std::u8string(arg.begin(), arg.end())));
      else if (verb == "choose" || verb == "choose-mod") {  // as if the import's file dialog returned this ZIP
        std::lock_guard lock(texture_pack_zip_choice.mutex);
        texture_pack_zip_choice.selected = arg;
        texture_pack_zip_choice.target = verb == "choose" ? content::ImportTarget::kTexturePack : content::ImportTarget::kMod;
      }
      else if (verb == "cancel-import") service.Cancel(content::Kind::kImport);
      for (const content::ModPack& pack : content::kModPacks) {  // download-uhd, cancel-comic, url-comic:<url>, ...
        if (verb == std::string("download-") + pack.key) service.Download(pack.kind);
        if (verb == std::string("cancel-") + pack.key) service.Cancel(pack.kind);
        if (verb == std::string("url-") + pack.key) {  // url-comic:<url>[|<sha256 of a test copy>]
          const size_t bar = arg.rfind('|');
          service.OverrideUrl(pack.kind, arg.substr(0, bar),
                              bar == std::string::npos ? std::string(content::InfoFor(pack.kind).sha256) : arg.substr(bar + 1));
        }
      }
      for (int i = 0; i < content::kKindCount; ++i) {
        const auto st = service.Get(content::Kind(i));
        REXLOG_INFO("KK dev: content {} stage {} error {} problem {} {} / {} installed {} name {}",
                    content::InfoFor(content::Kind(i)).id, int(st.stage), int(st.error), int(st.problem), st.done,
                    st.total, st.installed ? 1 : 0, st.installed_name);
      }
      return;
    }
    if (t.rfind("notify-test", 0) == 0) {  // notify-test[:fail]: a made-up download end in 2.5 s
      dev_notify_ok_ = t.find(":fail") == std::string::npos;
      dev_notify_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
      REXLOG_INFO("KK dev: notify test armed ({})", t);
      return;
    }
    if (t.rfind("stall:", 0) == 0) {  // task 209: the UI thread busy this many ms (a slow disk write, a long frame)
      std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(t.c_str() + 6)));
      return;
    }
    if (t.rfind("pause:", 0) == 0) {  // job J3: the next step this many seconds later
      at = now + std::atof(t.c_str() + 6);
      return;
    }
    if (t == "play") {  // job J3: Play as from Home (the Play while downloading pop-up)
      Play();
      return;
    }
    if (t == "quit") {  // task 286: the Quit pop-up's Quit
      Quit();
      return;
    }
    if (t.rfind("set:", 0) == 0) {  // task 286: "set:<cvar>=<value>" as a settings row would (e.g. a restart-only one)
      if (const size_t eq = t.find('=', 4); eq != std::string::npos) Set(t.substr(4, eq - 4).c_str(), t.substr(eq + 1));
      return;
    }
    if (t == "notify-history") {
      notify::LogHistory();
      return;
    }
    if (t.rfind("choose-zip:", 0) == 0) {  // as if the file dialog of "I already have a texture pack" chose it
      content_import_setup_ = InSetup() && SetupStage() == 6 ? 6 : 0;
      std::lock_guard lock(texture_pack_zip_choice.mutex);
      texture_pack_zip_choice.selected = t.substr(11);
      texture_pack_zip_choice.target = content::ImportTarget::kTexturePack;
      return;
    }
    if (t == "layout-check") {  // every settings page against settings_layout.h
      LogLayoutCheck();
      return;
    }
    if (t == "desc-fit") {  // task 220: every description panel in the five languages
      LogDescriptionFit();
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
    if (t == "shaders:status") {  // task 210: Home's SHADERS status and the one definition behind it
      REXLOG_INFO("KK dev: shaders status '{}' prepared {} marker '{}' job busy {} finished {}",
                  native_pack_.StatusText(ShadersPreparedForThisVersion()), ShadersPreparedForThisVersion(),
                  Get("kk_shaders_prepared"), native_pack_.Busy(), native_pack_.Finished());
      return;
    }
    if (t == "shaders:rebuild") return OpenRebuildShaders();  // task 210: the Rebuild shaders pop-up, as from Home
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
    // Task 358: "key:<name>" presses a real keyboard key (ImGui key events, let go at the next step), so the
    // keyboard path (Input::key_presses, the Home code) runs as when typed: up, down, left, right, a, b, enter,
    // space, escape, x.
    if (t.rfind("key:", 0) == 0) {
      static const std::map<std::string, ImGuiKey> kKeyNames = {
          {"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow}, {"left", ImGuiKey_LeftArrow},
          {"right", ImGuiKey_RightArrow}, {"a", ImGuiKey_A}, {"b", ImGuiKey_B}, {"enter", ImGuiKey_Enter},
          {"space", ImGuiKey_Space}, {"escape", ImGuiKey_Escape}, {"x", ImGuiKey_X}};
      if (auto it = kKeyNames.find(t.substr(4)); it != kKeyNames.end()) {
        io.AddKeyEvent(it->second, true);
        dev_key_up_ = it->second;
      }
      return;
    }
    // Task 364: "hold:lb+rb+lt+rt" holds those controller buttons down until the next hold step ("hold:none" lets
    // go), for the Home codes entered while shoulder buttons are held. Presses meanwhile: pad:down, pad:x, ...
    if (t.rfind("hold:", 0) == 0) {
      static const std::map<std::string, uint32_t> kHoldNames = {{"lb", ui::Input::kPadLB}, {"rb", ui::Input::kPadRB},
                                                                 {"lt", ui::Input::kPadLT}, {"rt", ui::Input::kPadRT}};
      uint32_t bits = 0;
      std::string rest = t.substr(5);
      for (size_t at = 0; at <= rest.size();) {
        const size_t plus = std::min(rest.find('+', at), rest.size());
        if (auto it = kHoldNames.find(rest.substr(at, plus - at)); it != kHoldNames.end()) bits |= it->second;
        at = plus + 1;
      }
      input_.InjectHeld(bits);
      REXLOG_INFO("KK dev: held {}", rest);
      return;
    }
    if (t.rfind("wheel:", 0) == 0) return io.AddMouseWheelEvent(0, std::stof(t.substr(6)));
    ui::Device d = ui::Device::kKeyboard;
    if (t.rfind("pad:", 0) == 0) d = ui::Device::kXbox, t = t.substr(4);
    else if (t.rfind("ps:", 0) == 0) d = ui::Device::kPlayStation, t = t.substr(3);
    else if (t.rfind("kb:", 0) == 0) t = t.substr(3);
    static const std::map<std::string, Action> kNames = {
        {"up", Action::kUp},          {"down", Action::kDown},       {"left", Action::kLeft},
        {"right", Action::kRight},    {"lt", Action::kPageUp},
        {"rt", Action::kPageDown},
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
  // Task 357: the Easter egg game (EggCodeInput, DrawEgg).
  egg::Code egg_code_;
  egg::HeldCodes egg_held_codes_;  // task 364: the original game's codes
  bool egg_holding_ = false;       // task 364: a code's hold was down last frame (logged once)
  std::unique_ptr<egg::Process> egg_;
  bool egg_running_ = false, egg_paused_ = false, egg_quitting_ = false;
  int egg_focus_ = 0;          // the pause panel: 0 Resume, 1 Quit to main menu
  double egg_pause_time_ = 0;  // ImGui time the pause panel opened (its shade fades in)
  double egg_close_at_ = 0;    // SteadySeconds when Quit to main menu asked it to close
  bool egg_enter_down_ = false;  // task 358: Enter was held last frame (read from the system)
  bool egg_esc_down_ = false;    // task 364: Esc likewise
  std::array<float, 2> egg_offsets_{};
  bool recovery_shown_ = false;
  bool crash_notice_shown_ = false;  // task 228
  int shown_frames_ = 0;
  std::map<int, ui::ListState> lists_;
  std::vector<Item> focused_items_;
  std::unique_ptr<Artbook> artbook_;  // Beta 7: made when Extras > Artbook first opens
  std::unique_ptr<Artbook> manual_;   // task 333: made when Extras > User manual first opens
  std::unique_ptr<Workshop> workshop_;  // job J8: made when the Workshop page first opens
  const Item* focused_ = nullptr;
  Action pending_click_ = Action::kCount;
  ui::SocialPanel social_;
  int home_focus_ = 0;
  PlayView play_ = PlayView::kNone;      // the Play menu and its pages, over Home
  int play_focus_ = 0;
  std::map<int, ui::ListState> play_lists_;
  coop::CoopPage coop_page_ = coop::CoopPage::Hub;   // C81: the Play Online Co-op page shown (PlayView::kCoop)
  std::map<int, ui::ListState> coop_lists_;
  std::optional<play::StartRequest> coop_choice_;     // the host's chapter or save, before its lobby exists
  bool coop_startup_checked_ = false;                 // the Steam invite start (+connect_lobby) was looked at
  // UI1 (Design 1): the pages' focus targets this frame, each page's focused target, the early access warning.
  std::vector<CoopTarget> coop_targets_;
  std::map<int, std::string> coop_focus_;
  coop::EarlyAccessGate coop_gate_;
  std::string coop_gate_focus_ = "continue";
  coop::SocialId coop_gate_lobby_ = 0;  // a Steam invite start waits behind the warning
  CoopRejoin coop_rejoin_;
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
  int card_focus_ = -1;      // task 373: the countdown card's button with the focus (Home), or -1
  int64_t launch_utc_ = 0;   // the Remastered update's launch (seconds since 1970 UTC)
  std::string launch_title_;  // the countdown's title from the website (empty: built in)
  std::shared_ptr<countdown::Fetch> launch_fetch_;
  bool dev_countdown_ = false;  // KK_DEV_COUNTDOWN set the time
  std::unique_ptr<rex::ui::ImmediateTexture> discord_icon_, steam_icon_;
  std::vector<float> home_offset_;
  float home_mix_ = 1.0f;
  float scene_mix_ = 0.0f;  // task 330: the live menu scene over the stills (0 .. 1)
  // Task 332: the startup movies (StartStartupMovies / DrawStartupMovies).
  std::unique_ptr<StartupMovies> movies_;
  float movies_wait_ = 0.0f;  // seconds the first movie has waited for the scene's start
  // Task 332 follow-up (owner): the "Startup movies" pop-up is up, over the plain background; no movie is loaded yet.
  bool movies_asking_ = false;
  bool MoviesPending() const { return movies_ != nullptr || movies_asking_; }
  // Task 331: the loading loop (StartLoadingLoop / DrawLoadingLoop).
  std::unique_ptr<LoadingLoop> loop_;
  loading_loop::Kind loop_kind_ = loading_loop::Kind::kNone;
  bool loop_scene_ = false;   // the live menu scene was started for this launcher: the loop waits for it
  bool loop_black_ = false;   // task 349c: the loop's video cannot show: a black cover while the scene starts
  std::string loop_open_on_;  // task 349c: --kk_launcher_open of this launcher (its second attempt opens the same)
  double scene_started_at_ = -1;  // task 349c: ImGui time of the scene's start (its attempt's clock)
  bool scene_start_pending_ = false;  // task 349c: the scene starts once the loop's first picture is on screen
  bool scene_retrying_ = false;       // task 349c: the launcher is starting again for the second attempt
  bool loop_saved_ = false;   // "Game saved at your last checkpoint."
  std::string loop_tip_;      // this time's tip (translated)
  float loop_shown_ = 0.0f;   // seconds on screen (from its first picture)
  double loop_first_ = -1;   // ImGui time of its first picture
  float loop_end_ = -1.0f;    // seconds since it started fading (< 0: not yet)
  float loop_wait_ = 0.0f;   // seconds since the launcher's first frame
  bool first_frame_noted_ = false;  // task 353: the start-up step log
  float scene_gain_ = 0.0f;  // task 330: the live menu scene's sound (0 .. 1)
  bool scene_retry_ = false;    // task 330: the scene may start later in this launcher (TickSceneStart)
  double scene_retry_at_ = 0;
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
  std::shared_ptr<Heartbeat> heartbeat_;  // task 173 (StartHeartbeat)
  std::chrono::steady_clock::time_point stalled_since_{}, last_reconnect_{};  // not drawing since
  int stall_ticks_ = 0, stall_reconnects_ = 0;
  int stall_free_ticks_ = 0;  // heartbeats in a row that ran at once (the UI thread free, still no frames)
  bool benchmark_pending_ = false;
  bool open_benchmark_results_ = false;  // --kk_launcher_open=benchmark, once the launcher draws
  std::shared_ptr<UpdateStatus> update_;
  std::shared_ptr<SteamState> steam_;
  bool update_manual_ = false, update_prompted_ = false, installing_update_ = false, relaunched_ = false;
  std::shared_ptr<FinishStatus> finish_;  // task 370: the first start check
  bool finish_hidden_ = false, finish_reported_ = false;
  std::string switch_to_;  // the channel Change version is switching to, while it checks
  bool open_whats_new_ = false;
  bool whats_new_shown_ = false;  // opened once in this launcher session (FinishSetup asks for it only if not)
  std::string whats_new_from_;  // version the player had before this one
  ShaderPackStatus pack_;
  std::thread pack_thread_;
  int pack_installed_ = -1;        // -1: not read yet
  bool waiting_for_pack_ = false;  // Play was pressed during the download
  NativeShaderPack native_pack_;      // the native renderer's shaders (native_shaders.h)
  bool existing_install_ = false;     // task 350a: run before this start (StartFullSetup, setup_again.h)
  bool setup_shaders_ = false;        // being prepared as the install's last step
  bool update_shaders_ = false;       // built again in full after an update (DrawShaderScreen)
  bool rebuild_shaders_screen_ = false;  // Graphics > Rebuild shaders: the shader screen until it's done
  Page settings_page_ = kDisplay;     // the Settings tab Home's Settings opens (the last one shown, About too)
  bool about_in_settings_ = false;    // About is open (always as Settings' last tab since 18 Oct)
  std::vector<std::string> tab_names_;  // the settings tabs, upper case, in tab_names_lang_
  int tab_names_lang_ = -1;
  bool waiting_for_native_ = false;   // Play was pressed: they are checked (and built) first
  bool native_modal_ = false;         // the pop-up of that wait is open
  bool native_pack_checked_ = false;  // checked for this Play: StartGame goes on
  mutable bool prepared_ = false;      // ShadersPreparedForThisVersion(), as last looked at
  mutable std::string prepared_marker_;  // ... for this kk_shaders_prepared
  mutable std::chrono::steady_clock::time_point prepared_checked_{};
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
  bool setup_textures_ = false;   // setup step 3: the HD texture pack page (SetupStage 6)
  bool setup_cutscenes_ = false;  // setup step 4: "Restore the PS2 cutscenes?" (SetupStage 7)
  int setup_content_seen_ = 0;    // TickSetupContent: the content pages already looked at (bit per stage)
  // Content downloads (job J2): the progress pop-up while it is open (its kind, button set, title and the
  // setup page it came from), an import in progress, and the finished / error pop-ups waiting for a free moment.
  PlayWait play_wait_ = PlayWait::kNone;  // J3: Play waits for a download to pause or an install to finish
  std::chrono::steady_clock::time_point play_wait_since_{};
  std::string play_wait_title_;
  int content_watch_ = -1, content_watch_mode_ = -1, content_watch_setup_ = 0;
  std::string content_watch_title_;
  int content_import_setup_ = 0;
  std::vector<std::function<void()>> content_popups_;
  uint64_t free_space_ = 0;
  double free_space_at_ = -10;
  int setup_focus_ = 0;
  content::Stage content_last_[content::kKindCount] = {};  // TickContent: the stage last seen
  exit_evidence::FocusLossLog focus_log_;  // task 229 (exit_evidence_logic.h)
  bool pads_active_ = true;  // the launcher's window is the active one: controllers count
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
      case kCheats: items = CheatItems(); break;
      case kAudio: items = AudioItems(); break;
      case kControls:
        items = sub_ == kRemapPage ? RemapItems() : sub_ == kKeysPage ? KeyItems() : ControlsItems();
        break;
      case kAdvanced: items = AdvancedItems(); break;
      default: break;
    }
    items = LiveOnly(std::move(items));
    AttachHelp(items);
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
  static_assert(int(kPageCount) == int(settings_layout::kPageCount));  // the same tabs, same order
  return page >= 0 && page < kPageCount ? settings_layout::kPageNames[page] : "";
}

std::unique_ptr<InGameSettings> CreateInGameSettings(rex::ui::ImmediateDrawer* immediate,
                                                     std::filesystem::path config_path,
                                                     std::filesystem::path user_dir) {
  return std::make_unique<InGameSettingsImpl>(immediate, std::move(config_path), user_dir);
}

bool NativeWindowDrawable(void* handle) {
#if defined(_WIN32)
  const HWND hwnd = static_cast<HWND>(handle);
  RECT r{};
  return hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetClientRect(hwnd, &r) &&
         r.right > 0 && r.bottom > 0;
#else
  (void)handle;
  return true;
#endif
}

std::string ForegroundWindowOwner() {
#if defined(_WIN32)
  const HWND hwnd = GetForegroundWindow();
  if (!hwnd) return "no window";
  wchar_t cls[128] = L"";
  GetClassNameW(hwnd, cls, 128);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  std::wstring exe = L"?";
  if (pid == GetCurrentProcessId()) {
    exe = L"this program";
  } else if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
    wchar_t image[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, image, &size)) {
      const wchar_t* slash = wcsrchr(image, L'\\');
      exe = slash ? slash + 1 : image;
    }
    CloseHandle(p);
  }
  return "class '" + std::filesystem::path(cls).string() + "' of " + std::filesystem::path(exe).string();
#else
  return {};
#endif
}

std::pair<bool, bool> NativeWindowState(void* handle) {
#if defined(_WIN32)
  const HWND hwnd = static_cast<HWND>(handle);
  if (!hwnd || !IsWindow(hwnd)) return {false, false};
  return {IsIconic(hwnd) != FALSE, GetForegroundWindow() == hwnd};
#else
  (void)handle;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (!windows) return {false, false};
  bool minimised = count > 0, focused = false;
  for (int i = 0; i < count; ++i) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(windows[i]);
    minimised = minimised && (flags & SDL_WINDOW_MINIMIZED);
    focused = focused || (flags & SDL_WINDOW_INPUT_FOCUS);
  }
  SDL_free(windows);
  return {minimised, focused};
#endif
}

bool RestoreNativeWindow(void* handle) {
#if defined(_WIN32)
  const HWND hwnd = static_cast<HWND>(handle);
  if (!hwnd || !IsWindow(hwnd)) return false;
  if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
  // Windows lets a process take the foreground only in some cases (the player's click on the notification
  // usually counts); otherwise the taskbar button flashes instead.
  const bool front = SetForegroundWindow(hwnd) != FALSE;
  REXLOG_INFO("KK notify: window restored{}", front ? " and brought to the front" : " (Windows kept another window in front)");
  return true;
#else
  (void)handle;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (!windows) return false;
  for (int i = 0; i < count; ++i) {
    SDL_RestoreWindow(windows[i]);
    SDL_RaiseWindow(windows[i]);
  }
  SDL_free(windows);
  return count > 0;
#endif
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
  SetCvarDefault("native_ao_normal_smoothing", "2");  // High with Strong GTAO (owner 17 Oct)
  // Beta 8 (owner): GI distance Full is F8 only; an Ultra preset saved with Full moves to Far.
  if (Get("native_gi_distance") == "full" && Get("kk_graphics_preset") == "Ultra") Set("native_gi_distance", "far");
  ApplyUpscalerDefaults();
  return startup::ApplyChoice(rex::cvar::GetFlagByName("gpu_plugin"));
}

void AskUpscalersBeforeWindow() {
  if (!HasTemporalSetting() || !UsingNativeRenderer()) return;
  const auto start = std::chrono::steady_clock::now();
  if (nr::kk::QueryUpscalers().state != 1) return;  // kept by the renderer (or none here): no device made
  nr::kk::WaitForUpscalers();
  nr::kk::QueryUpscalers();  // takes the answer, which the renderer then keeps in the settings
  REXLOG_INFO("KK: upscalers asked before the launcher's window opened ({:.0f} ms)",
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

void ShowLauncher(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, LauncherPaths paths,
                  LauncherCallbacks callbacks) {
  // Task 285: one SDL audio reference for this process's life, taken here on the main thread. Without it
  // the launcher's music and sounds were SDL's only audio users: closing the launcher for Play (the game
  // in this process) shut Windows audio down completely on this thread, and a second later the game thread
  // started it again and opened the device (SDL asks for both on the main thread). A restarted game once
  // stalled at exactly that open (REPORT-271, run l1dx). Now the game's open finds SDL audio running.
  static const bool audio_kept = SDL_InitSubSystem(SDL_INIT_AUDIO);
  (void)audio_kept;
  new Launcher(drawer, immediate, std::move(paths), std::move(callbacks));
}

}  // namespace kk
