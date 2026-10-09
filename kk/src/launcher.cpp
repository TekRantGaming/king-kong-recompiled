#include "launcher.h"
#include "launcher_art.h"
#include "launcher_music.h"
#include "launcher_sounds.h"
#include "launcher_text.h"
#include "launcher_ui.h"
#include "shader_pack.h"
#include "steam_shortcut.h"
#include "update.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
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
#include "iso.h"
#include "toast.h"

namespace kk {
namespace {

using text::F;
using text::T;
using ui::Action;
using ui::Item;
using ui::ItemType;
namespace color = ui::color;

constexpr uint32_t kTitleId = 0x555307D3;
constexpr const char* kProjectUrl = "https://github.com/TekRantGaming/king-kong-recompiled";
#define KK_TIMES "\xC3\x97"
#define KK_DOT "\xC2\xB7"
#define KK_DEG "\xC2\xB0"

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

const rex::cvar::FlagEntry* FindFlag(std::string_view name) {
  for (auto& e : rex::cvar::GetRegistry())
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
constexpr const char* kRestartCvars[] = {"present_effect", "window_width", "window_height", "monitor"};

enum Page { kHome, kDisplay, kGraphics, kGameplay, kControls, kCheatsPage, kAchievements, kAbout, kPageCount };
constexpr const char* kPageNames[kPageCount] = {"Home",     "Display", "Graphics",     "Gameplay",
                                                "Controls", "Cheats",  "Achievements", "About"};
// Sub-pages of Controls.
enum SubPage { kNoSubPage, kRemapPage, kKeysPage };

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

// ------------------------------------------------------------ presets ---
// Graphics presets set the upscaler, render quality, anti-aliasing, texture
// filtering and ambient occlusion (motion blur and fog are left to taste).
// The preset shown is the one the settings match; anything else is Custom.
// Medium is the default settings (FSR 1 at Quality).
struct GraphicsPreset {
  const char* label;
  const char* upscaler;   // present_effect
  const char* quality;    // kk_render_quality
  const char* aa;         // swap_post_effect
  const char* filtering;  // anisotropic_override: 2 = 2x, 3 = 4x, 4 = 8x, 5 = 16x
  bool ao;                // ao_mode 1 at strength 1
};
constexpr GraphicsPreset kGraphicsPresets[] = {
    {"Low", "fsr", "performance", "fxaa", "2", false},
    {"Medium", "fsr", "quality", "fxaa", "3", false},
    {"High", "bilinear", "native", "fxaa", "4", true},
    {"Ultra", "bilinear", "supersample", "fxaa_extreme", "5", true},
    {"Steam Deck", "bilinear", "native", "fxaa", "2", false},
};
constexpr int kGraphicsPresetCount = int(std::size(kGraphicsPresets));

// The upscaler modes (kk_render_quality); -1 for another render quality.
constexpr const char* kUpscalerModes[] = {"native", "quality", "balanced", "performance"};
constexpr const char* kUpscalerModeNames[] = {"Native", "Quality", "Balanced", "Performance"};
int UpscalerMode() {
  const std::string q = Get("kk_render_quality");
  for (int i = 0; i < 4; ++i)
    if (q == kUpscalerModes[i]) return i;
  return -1;
}

int MatchGraphicsPreset() {
  for (int i = 0; i < kGraphicsPresetCount; ++i) {
    const GraphicsPreset& p = kGraphicsPresets[i];
    const bool ao = GetInt("ao_mode", 0) != 0;
    if (Get("present_effect") == p.upscaler && Get("kk_render_quality") == p.quality &&
        Get("swap_post_effect") == p.aa && Get("anisotropic_override") == p.filtering && ao == p.ao &&
        (!ao || GetInt("ao_strength", 1) == 1))
      return i;
  }
  return -1;
}

void ApplyGraphicsPreset(int index) {
  const GraphicsPreset& p = kGraphicsPresets[index];
  Set("present_effect", p.upscaler);
  Set("kk_render_quality", p.quality);
  Set("swap_post_effect", p.aa);
  Set("anisotropic_override", p.filtering);
  SetInt("ao_mode", p.ao ? 1 : 0);
  if (p.ao) SetInt("ao_strength", 1);
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
    if (saved.empty()) ApplyGraphicsPreset(1);  // Medium
    std::stringstream in(saved);
    for (std::string item; std::getline(in, item, ';');) {
      const size_t eq = item.find('=');
      if (eq != std::string::npos) Set(item.substr(0, eq).c_str(), item.substr(eq + 1));
    }
    Set("kk_modern_settings", "");
  }
  SetBool("kk_original_look", on);
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

// A small, blurred copy of a backdrop, for behind the settings.
art::Image SoftCopy(const art::Image& src) {
  constexpr int kStep = 4;
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
  Blur(out.rgba, out.width, out.height, std::max(2, out.width / 100), 3);
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

class Launcher final : public rex::ui::ImGuiDialog {
 public:
  Launcher(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, LauncherPaths paths,
           LauncherCallbacks callbacks)
      : ImGuiDialog(drawer),
        immediate_(immediate),
        paths_(std::move(paths)),
        cb_(std::move(callbacks)),
        glyphs_(immediate, rex::filesystem::GetExecutableFolder() / "glyphs") {
    glyphs_.AddSet("xbox360", art::LauncherArtDir(paths_.user_dir) / "glyphs" / "xbox360");
    for (const char* name : kRestartCvars) restart_baseline_.push_back(Get(name));
    files_ok_ = GameFilesPresent(paths_.game_dir);
    home_focus_ = 0;
    EnforceOriginalLook();
    LoadArt();
    // Installed before the launcher took its art from the game files: take it now.
    if (files_ok_ && !art::HasLauncherArt(ArtDir())) StartArtExtraction(false);
    toast_ = std::make_unique<AchievementToast>(drawer, immediate_, paths_.game_dir, paths_.user_dir);
    CleanUpAfterUpdate();
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
      }
      Set("kk_last_version", KK_VERSION);
      SaveSettings(paths_.config_path);
    }
    saved_changes_ = seen_changes_ = g_changes;
  }

  ~Launcher() override {
    ui::SetSoundHandler(nullptr);
    if (install_thread_.joinable()) {
      progress_.cancel = true;
      install_thread_.join();
    }
    if (pack_thread_.joinable()) pack_thread_.join();
    if (art_thread_.joinable()) art_thread_.join();
    for (auto& texture : textures_) KeepTextureAlive(std::move(texture));
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    // Testing aid: KK_AUTOPLAY=1 presses Play after a couple of seconds.
    static const bool autoplay = std::getenv("KK_AUTOPLAY") != nullptr;
    if (autoplay && files_ok_ && ++autoplay_frames_ == 120) StartGame();

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

    input_.set_keyboard_enabled(capturing_.empty());  // keys go to the binding being set
    input_.Update();
    if (pending_click_ != Action::kCount) {
      input_.Inject(pending_click_, ui::Device::kKeyboard);
      pending_click_ = Action::kCount;
    }
#if defined(KK_DEV_TOOLS)
    DevTour();
    DevInput();
#endif
    Tick();
    if (files_ok_ && !music_) music_ = std::make_unique<LauncherMusic>(paths_.game_dir);
    if (music_) {
      const float v = GetBool("kk_launcher_music") ? GetInt("kk_launcher_music_volume", 50) / 100.0f : 0.0f;
      music_->Update(v * v, io.DeltaTime);  // a gentler curve than linear
    }
    if (files_ok_ && GetBool("kk_launcher_sounds") && !ui_sounds_)
      ui_sounds_ = std::make_unique<LauncherSounds>(paths_.game_dir);

    DrawBackdrop(dl);
    if (InSetup()) {
      DrawSetup(dl);
    } else {
      if (!modal_.open) HandleGlobalInput();
      DrawTopBar(dl);
      if (page_ == kHome) DrawHome(dl);
      else DrawSettings(dl);
    }
    ui::DrawModal(dl, modal_, o_, ImVec2(o_.x + w_, o_.y + h_), m_, input_);
    DrawBottomBar(dl);  // over a pop-up's shade: its prompts stay clear
    ImGui::End();
    (void)io;
  }

 private:
  float S() const { return m_.s; }
  float Margin() const { return 64 * m_.s; }

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
      page_ = kHome;
      home_focus_ = 0;
      home_mix_ = 0;  // the art fades in
      Status("Installed. Your copy of King Kong is ready to play.");
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
    const bool home = page_ == kHome && !InSetup();
    home_mix_ = ui::Approach(home_mix_ * 100.0f, home ? 100.0f : 0.0f, 9.0f) / 100.0f;
    slide_clock_ += std::min(ImGui::GetIO().DeltaTime, 0.1f);
    const ImVec2 p1(o_.x + w_, o_.y + h_);
    if (!slides_.empty() && !InSetup()) {
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
    dl->AddRectFilled(o_, p1, ui::WithAlpha(color::kBase, InSetup() ? 0.1f : lerp(0.80f, 0.16f, hm)));
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
  void DrawTopBar(ImDrawList* dl) {
    const float s = S();
    std::vector<std::string> names;
    for (const char* n : kPageNames) names.push_back(Upper(T(n)));
    float ux = 0, uw = 0;
    const int clicked = ui::DrawTabs(dl, glyphs_, PromptSet(), names, page_, ImVec2(o_.x + Margin(), o_.y + 44 * s),
                                     m_, !modal_.open, &ux, &uw);
    if (clicked >= 0 && clicked != int(page_)) {
      ui::Cue(ui::Sound::kTab);
      GoTo(Page(clicked));
    }
    if (tab_x_ < 0) {
      tab_x_ = ux;
      tab_w_ = uw;
    }
    tab_x_ = ui::Approach(tab_x_, ux, 22.0f);
    tab_w_ = ui::Approach(tab_w_, uw, 22.0f);
    const float ly = std::round(o_.y + 74 * s);
    dl->AddLine(ImVec2(o_.x + Margin(), ly), ImVec2(o_.x + w_ - Margin(), ly), color::kHairline, 1.0f);
    dl->AddRectFilled(ImVec2(tab_x_, ly - 2 * s), ImVec2(tab_x_ + tab_w_, ly), color::kAccent);
    // The game's name at the right, away from Home (which shows it large).
    if (home_mix_ < 0.99f) {
      const ui::Fonts f = ui::GetFonts();
      const std::string name = "KING KONG";
      const float fs = 15 * s, tr = 4 * s;
      const float tw = ui::TrackedWidth(f.display, fs, name, tr);
      ui::DrawTracked(dl, f.display, fs, ImVec2(o_.x + w_ - Margin() - tw, o_.y + 44 * s - fs * 0.5f),
                      ui::WithAlpha(color::kTextDim, 1.0f - home_mix_), name, tr);
    }
  }

  // The button pictures the Button prompts setting picks, as in the game.
  std::string PadSet() const {
    const std::string p = Get("kk_button_prompts");
    if (p == "xbox360" || p == "xbox_series" || p == "ps5" || p == "ps2" || p == "keyboard") return p;
    return input_.pad_device() == ui::Device::kPlayStation ? "ps5" : "xbox_series";
  }
  // The prompts' pictures: keys while the keyboard and mouse are in use.
  std::string PromptSet() const { return input_.device() == ui::Device::kKeyboard ? "keyboard" : PadSet(); }

  void GoTo(Page page) {
    if (page != page_) sub_ = kNoSubPage;
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
    if (InSetup()) {
      switch (SetupStage()) {
        case 0: return {{Action::kAccept, "Select"}, {Action::kBack, "Quit"}};
        case 1: return {{Action::kBack, "Cancel"}};
        default: return {};
      }
    }
    if (page_ == kHome) {
      p.push_back({Action::kAccept, "Select"});
      p.push_back({Action::kBack, "Quit"});
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

  void Status(std::string text) {
    status_ = std::move(text);
    status_time_ = ImGui::GetTime();
  }

  // ------------------------------------------------------- global input ---
  void HandleGlobalInput() {
    ui::Input& in = input_;
    if (in.Pressed(Action::kPrevTab) || in.Pressed(Action::kNextTab)) {
      const int dir = in.Pressed(Action::kNextTab) ? 1 : -1;
      ui::Cue(ui::Sound::kTab);
      GoTo(Page((int(page_) + dir + kPageCount) % kPageCount));
      in.Consume(Action::kPrevTab);
      in.Consume(Action::kNextTab);
    }
    if (in.Pressed(Action::kPlay)) {
      in.Consume(Action::kPlay);
      if (CanPlay()) Play();
    }
    if (in.Pressed(Action::kResetPage) && page_ != kHome && sub_ == kNoSubPage && !PageSettings(page_).empty()) {
      in.Consume(Action::kResetPage);
      OpenResetPage();
    }
    if (in.Pressed(Action::kBack)) {
      in.Consume(Action::kBack);
      ui::Cue(ui::Sound::kBack);
      if (sub_ != kNoSubPage) sub_ = kNoSubPage;
      else if (page_ != kHome) GoTo(kHome);
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
  };

  std::vector<HomeEntry> HomeEntries() {
    std::vector<HomeEntry> e;
    e.push_back({"Play", CanPlay(), [this] { Play(); }});
    if (installing_) e.push_back({"Cancel install", true, [this] { progress_.cancel = true; }});
    else e.push_back({files_ok_ ? "Reinstall game" : "Install game", true, [this] { StartInstall(); }});
    if (update_ && update_->found && !installing_update_)
      e.push_back({F("Update to v{0}", {update_->found->version}), true, [this] { OpenUpdatePrompt(); }});
    else if (update_ && update_->busy)
      e.push_back({"Checking for updates", false, nullptr});
    else
      e.push_back({"Check for updates", true, [this] { StartUpdateCheck(true); }});
    if (steam_ && steam_->available && !steam_->in_library)
      e.push_back({"Add to Steam", !steam_->busy, [this] { OpenSteamPrompt(); }});
    e.push_back({"Settings", true, [this] { GoTo(kDisplay); }});
    e.push_back({"Achievements", true, [this] { GoTo(kAchievements); }});
    e.push_back({"Quit", true, [this] { Quit(); }});
    return e;
  }

  // A column of large menu entries (Home and the setup screen): the focused
  // one moves in, with a brass mark. Returns the one chosen, or -1.
  int DrawMenu(ImDrawList* dl, const std::vector<HomeEntry>& entries, int& focus, float x, float y, float alpha) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const int n = int(entries.size());
    if (n == 0) return -1;
    focus = std::clamp(focus, 0, n - 1);
    const int focus_before = focus;
    const bool active = !modal_.open;
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
    const float item_h = 42 * s, fs = 24 * s, tr = 3 * s;
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
      const bool on = i == focus;
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
    if (chosen >= 0 && entries[size_t(chosen)].label != "Play") ui::Cue(ui::Sound::kSelect);
    return chosen;
  }

  void DrawHome(ImDrawList* dl) {
    const float s = S();
    const ui::Fonts f = ui::GetFonts();
    const float x = o_.x + Margin();
    const float a = home_mix_;  // fades in with the clear backdrop
    const std::vector<HomeEntry> entries = HomeEntries();
    // The title and the menu stay clear of the status row: with more entries
    // they move up, then the logo gets smaller.
    const float bottom = o_.y + h_ - 150 * s - 42 * s * float(entries.size());
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
    if (const int i = DrawMenu(dl, entries, home_focus_, x, y, a); i >= 0) entries[size_t(i)].action();
    DrawHomeStatus(dl, x, o_.y + h_ - 120 * s, a);
  }

  // ------------------------------------------------------------- setup ---
  // Before the game is installed, the launcher is a setup screen of its own:
  // it asks for the player's copy of the game (the launcher's artwork comes
  // from it too), then shows the install and the artwork being prepared.
  bool InSetup() const {
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) return true;
#endif
    return !files_ok_ || installing_ || setup_art_ || installing_update_;
  }

  // 0: asking for the disc image, 1: installing, 2: taking the artwork,
  // 3: updating the port.
  int SetupStage() const {
#if defined(KK_DEV_TOOLS)
    if (dev_setup_ >= 0) return dev_setup_;
#endif
    return installing_update_ ? 3 : installing_ ? 1 : setup_art_ ? 2 : 0;
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
      body("This port plays the original Xbox 360 game, so it needs your own copy: a disc image (.iso) of "
           "Peter Jackson's King Kong for Xbox 360. The game's files are copied from it, and so is the artwork "
           "this launcher shows. Nothing from the game comes with the port.");
      body("The game takes about 6.3 GB once it's installed.");
      y += 8 * s;
      entries.push_back({"Select disc image", true, [this] { StartInstall(); }});
      entries.push_back(LanguageEntry());
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
    if (const int i = DrawMenu(dl, entries, setup_focus_, x, y, 1.0f); i >= 0) entries[size_t(i)].action();
    if (!modal_.open && input_.Pressed(Action::kBack)) {
      input_.Consume(Action::kBack);
      if (stage <= 1) ui::Cue(ui::Sound::kBack);
      if (stage == 0) OpenQuit();
      else if (stage == 1) progress_.cancel = true;
    }
  }

  static ImU32 WithAlphaText(float a) { return ui::WithAlpha(color::kText, a); }

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
    if (installing_) {
      const double total = std::max<double>(1.0, double(progress_.bytes_total.load()));
      const float p = float(double(progress_.bytes_done.load()) / total);
      cols[0] = {Upper(T("Game")), F("Installing {0}%", {std::to_string(int(p * 100))}), color::kText, p};
    } else {
      cols[0] = {Upper(T("Game")), T(files_ok_ ? "Ready to play" : "Not installed"),
                 files_ok_ ? color::kText : color::kWarn, -1};
    }
    cols[1] = {Upper(T("Shader pack")), PackStatusText(), pack_.failed ? color::kWarn : color::kText, PackProgress()};
    if (update_ && update_->found)
      cols[2] = {Upper(T("Version")), F("v{0}   " KK_DOT "   v{1} is out", {KK_VERSION, update_->found->version}),
                 color::kAccent, -1};
    else
      cols[2] = {Upper(T("Version")), "v" KK_VERSION, color::kText, -1};
    float cx = x;
    for (const Column& c : cols) {
      ui::DrawTracked(dl, f.display, 12 * s, ImVec2(cx, y), ui::WithAlpha(color::kTextFaint, alpha), c.caption, 2.2f * s);
      dl->AddText(f.text, 15.5f * s, ImVec2(cx, y + 20 * s), ui::WithAlpha(c.color, alpha), c.value.c_str());
      if (c.progress >= 0) {
        const float bw = 150 * s, by = y + 46 * s;
        dl->AddRectFilled(ImVec2(cx, by), ImVec2(cx + bw, by + 2 * s), ui::WithAlpha(color::kText, 0.12f * alpha));
        dl->AddRectFilled(ImVec2(cx, by), ImVec2(cx + bw * std::clamp(c.progress, 0.0f, 1.0f), by + 2 * s),
                          ui::WithAlpha(color::kAccent, alpha));
      }
      cx += std::max(210 * s, ui::TextSize(f.text, 15.5f * s, c.value).x + 48 * s);
    }
  }

  // ----------------------------------------------------- settings pages ---
  void DrawSettings(ImDrawList* dl) {
    const float s = S();
    std::vector<Item> items;
    switch (page_) {
      case kDisplay: items = DisplayItems(); break;
      case kGraphics: items = GraphicsItems(); break;
      case kGameplay: items = GameplayItems(); break;
      case kControls:
        items = sub_ == kRemapPage ? RemapItems() : sub_ == kKeysPage ? KeyItems() : ControlsItems();
        break;
      case kCheatsPage: items = CheatItems(); break;
      case kAchievements: items = AchievementItems(); break;
      case kAbout: items = AboutItems(); break;
      default: break;
    }
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

  static constexpr const char* kSetByOriginal =
      "Set by the Original Xbox 360 look. Switch Look to Modern on the Graphics page to change it.";

  // ------------------------------------------------------------ Display ---
  std::vector<Item> DisplayItems() {
    std::vector<Item> items;
    items.push_back(Section("Window"));
    {
      Item it = CvarToggle("Window mode", "Fullscreen uses a borderless window at your desktop resolution.",
                           "fullscreen", "Windowed", "Fullscreen");
      it.on_choice = [this](int i) { SetFullscreen(i == 1); };
      it.on_default = [this] {
        Reset("fullscreen");
        if (cb_.set_fullscreen) cb_.set_fullscreen(GetBool("fullscreen"));
      };
      items.push_back(std::move(it));
    }
    items.push_back(WindowSizeItem());
    items.push_back(MonitorItem());
    items.push_back(Section("Picture"));
    items.push_back(CvarToggle("VSync",
                               "On waits for the display for tear-free frames. Off has the lowest latency and lets "
                               "G-Sync and FreeSync displays run freely.",
                               "d3d12_allow_variable_refresh_rate_and_tearing", "Off", "On", /*invert=*/true));
    items.push_back(CvarChoice("Aspect ratio",
                               "The game is 16:9. Letterbox keeps its shape on other screens; Stretch fills them.",
                               "present_letterbox", {{"Letterbox 16:9", "true"}, {"Stretch", "false"}}));
    return items;
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
    Disable(it, GetBool("fullscreen"), "Only used in windowed mode.");
    return it;
  }

  Item MonitorItem() {
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
    const auto [out_w, out_h] = cb_.output_size ? cb_.output_size() : std::pair<int, int>{1280, 720};
    const bool original = GetBool("kk_original_look");
    const char* draws = GetBool("fullscreen") ? "Draws {0} for your {1} screen." : "Draws {0} for your {1} window.";
    std::vector<Item> items;
    items.push_back(Section("Look"));
    {
      Item it = CvarChoice("Look",
                           "Original is the Xbox 360 version as it was: 720p at 30 FPS, the console's own "
                           "anti-aliasing and texture filtering, its 69" KK_DEG " field of view, motion blur, screen "
                           "blur and distance fog, and no ambient occlusion. Modern gives you the presets and every "
                           "setting on this page.",
                           "kk_original_look", {{"Original Xbox 360", "true"}, {"Modern", "false"}});
      it.on_choice = [](int i) { SetOriginalLook(i == 0); };
      it.on_default = [] { SetOriginalLook(false); };
      items.push_back(std::move(it));
    }
    {
      Item it;
      it.type = ItemType::kChoice;
      it.label = "Preset";
      it.description =
          "Quick settings for the upscaler, render quality, anti-aliasing, texture filtering and ambient occlusion. "
          "Change any of them yourself and it shows Custom.";
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

    items.push_back(Section("Resolution"));
    {
      // NIS is in the D3D12 presenter only (the Vulkan one, used on Linux, would give FSR).
      std::vector<Option> opts = {{"Off", "bilinear"}, {"AMD FSR 1", "fsr"}};
#if defined(_WIN32)
      opts.push_back({"NVIDIA NIS", "nis"});
#endif
      Item it = CvarChoice("Upscaler",
                           "Scales the picture up to your screen and sharpens it, for a clearer image when the game "
                           "draws fewer pixels than your screen has. AMD FSR 1 and NVIDIA Image Scaling (NIS) both "
                           "work on any graphics card. Off uses plain smooth scaling.",
                           "present_effect", opts);
      std::vector<std::string> values;
      for (const auto& o : opts) values.push_back(o.value);
      it.on_choice = [values](int i) {
        Set("present_effect", values[size_t(i)]);
        // The upscaler modes are Native to Performance; others start at Native.
        if (i != 0 && UpscalerMode() < 0) Set("kk_render_quality", "native");
      };
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
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
          "Performance modes draw fewer pixels and scale up (an upscaler makes that sharper); Supersample draws more "
          "for the cleanest edges. The game renders in steps of its original 720p.";
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

    items.push_back(Section("Image"));
    {
      Item it = CvarChoice("Anti-aliasing", "Smooths jagged edges after the frame is drawn. Extreme is softer but cleaner.",
                           "swap_post_effect", {{"Off", "none"}, {"FXAA", "fxaa"}, {"FXAA Extreme", "fxaa_extreme"}});
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
    {
      // Ambient occlusion lives in the GPU plugin (ao_mode, ao_strength).
      Item it = CvarChoice("Ambient occlusion",
                           "Soft shading where surfaces meet: in corners and creases, and on the ground under rocks, "
                           "grass and people. Costs about 1 ms a frame at 4K. Not available with Intel graphics yet.",
                           "ao_mode", {{"Off", "0"}, {"On", "1"}});
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
      if (GetInt("ao_mode", 0) != 0) {
        Item st = CvarChoice("AO strength", "How dark the shading gets. 1 is recommended.", "ao_strength",
                             {{"1 (recommended)", "1"}, {"2", "2"}, {"3", "3"}});
        items.push_back(std::move(Disable(st, original, kSetByOriginal)));
      }
    }

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

    return items;
  }

  // ----------------------------------------------------------- Gameplay ---
  std::vector<Item> GameplayItems() {
    const bool original = GetBool("kk_original_look");
    std::vector<Item> items;
    items.push_back(Section("Game"));
    {
      std::vector<Option> opts;
      for (int f : kFrameRateChoices)
        opts.push_back({f <= 0 ? "Unlimited" : f == 30 ? "30 FPS (Xbox 360)" : std::to_string(f) + " FPS",
                        std::to_string(f)});
      const std::string cur = Get("kk_frame_rate");
      bool listed = false;
      for (const auto& o : opts) listed = listed || o.value == cur;
      if (!listed && !cur.empty()) opts.push_back({F("{0} FPS (settings file)", {cur}), cur});
      Item it = CvarChoice("Frame rate",
                           "30 matches the Xbox 360 and keeps every animation right. Higher is smoother, but some "
                           "character animations are not right above 30 yet.",
                           "kk_frame_rate", opts);
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    {
      Item it = CvarSlider("Field of view",
                           "How wide the camera sees. 69" KK_DEG " is the original for Jack; Kong, cutscene and other "
                           "cameras widen by the same amount. Jack's gun keeps its usual size.",
                           "kk_fov", 69, 110, 1, [](float v) {
                             const int d = int(std::lround(v));
                             return d <= 69 ? F("{0}\xC2\xB0 (original)", {std::to_string(d)}) : std::to_string(d) + KK_DEG;
                           });
      items.push_back(std::move(Disable(it, original, kSetByOriginal)));
    }
    items.push_back(CvarChoice("Language", "The game's language, where the game includes it.", "user_language",
                               {{"English", "1"},
                                {"Deutsch", "3"},
                                {"Espa\xC3\xB1ol", "5"},
                                {"Fran\xC3\xA7" "ais", "4"},
                                {"Italiano", "6"}}));
    items.push_back(Section("Extras"));
    items.push_back(CvarToggle("Frame counter", "Shows the game's frame rate in the corner. F2 toggles it while playing.",
                               "kk_show_fps", "Hidden", "Shown"));
    items.push_back(CvarToggle("Startup logos",
                               "The Ubisoft, Universal and WingNut movies before the title screen. Story movies "
                               "still play.",
                               "kk_skip_intros", "Play", "Skip"));
    return items;
  }

  // ----------------------------------------------------------- Controls ---
  std::vector<Item> ControlsItems() {
    std::vector<Item> items;
    const auto percent = [](float v) { return std::to_string(int(std::lround(v))) + "%"; };
    items.push_back(Section("Controller"));
    items.push_back(CvarToggle("Input",
                               "Controllers work automatically. Keyboard & mouse plays with a keyboard and mouse "
                               "instead; its keys are under Keyboard bindings.",
                               "mnk_mode", "Controller", "Keyboard & mouse"));
    items.push_back(CvarChoice("Button prompts", "Which controller's buttons the game shows in menus and hints.",
                               "kk_button_prompts",
                               {{"Xbox 360", "xbox360"},
                                {"Xbox Series", "xbox_series"},
                                {"PlayStation 5", "ps5"},
                                {"PlayStation 2", "ps2"},
                                {"Keyboard", "keyboard"}}));
    items.push_back(CvarSlider("Camera sensitivity", "How fast the right stick turns the camera and moves your aim.",
                               "kk_camera_sensitivity", 25, 300, 5, percent));
    items.push_back(CvarToggle("Camera response",
                               "Modern turns the camera the same way in every direction, so circles and diagonals feel "
                               "even. Original is the Xbox 360's own: small pushes up and down turn much more slowly "
                               "than small pushes sideways.",
                               "kk_camera_modern", "Original", "Modern"));
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
    items.push_back(CvarSlider("Stick deadzone", "Ignores small stick movements. Raise it if the camera or cursor drifts.",
                               "kk_deadzone", 0, 40, 1, percent));
    items.push_back(CvarToggle("Aim",
                               "Hold: keep the left trigger held to raise the gun, as on the console. Toggle: press "
                               "it once to raise the gun and again to lower it. Pausing lowers it.",
                               "kk_toggle_aim", "Hold", "Toggle"));
    items.push_back(CvarToggle("Vibration", "Controller rumble.", "kk_vibration"));
    {
      Item it = CvarSlider("Vibration strength", "How strong the rumble is.", "kk_vibration_strength", 10, 100, 5,
                           percent);
      items.push_back(std::move(Disable(it, !GetBool("kk_vibration"), "Switch Vibration on to set its strength.")));
    }

    const bool mnk = GetBool("mnk_mode");
    const char* mnk_note = "Used when Input is Keyboard & mouse.";
    items.push_back(Section("Keyboard & mouse"));
    {
      Item it = CvarSlider("Mouse sensitivity", "How fast the mouse turns the camera.", "mnk_sensitivity", 0.1f, 5.0f,
                           0.05f, [](float v) {
                             char buf[32];
                             std::snprintf(buf, sizeof(buf), "%.2f" KK_TIMES, v);
                             return std::string(buf);
                           });
      items.push_back(std::move(Disable(it, !mnk, mnk_note)));
    }
    {
      Item it = CvarToggle("Mouse camera", "Move the camera (right stick) with the mouse.", "mnk_mouse");
      items.push_back(std::move(Disable(it, !mnk, mnk_note)));
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
    for (auto* e : binds) {
      const std::string value = pretty(e->getter());
      Item it = ActionItem(e->description, "Select, then press the key to use. Esc cancels.",
                           value.empty() ? "None" : value, [this, name = e->name, label = e->description] {
                             capturing_ = name;
                             ui::Modal m;
                             m.open = true;
                             m.title = "Press a key";
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
                               "Switches on the cheats you pick below as soon as the game reaches its main menu, the "
                               "same as typing their codes on the game's Cheat screen. They last until you quit the "
                               "game.",
                               "kk_cheats"));
    const bool on = GetBool("kk_cheats");
    items.push_back(Section("Cheat list"));
    for (const auto& cheat : kCheats) {
      Item it = CvarToggle(cheat.label, "One of the game's own cheats.", std::string("kk_cheat_") + cheat.id);
      it.facts.push_back(F("Code: {0}", {cheat.code}));
      items.push_back(std::move(Disable(it, !on, "Switch Cheats on to use it.")));
    }
    return items;
  }

  // ------------------------------------------------------- Achievements ---
  std::vector<Item> AchievementItems() {
    std::vector<Item> items;
    items.push_back(Section("Notifications"));
    items.push_back(CvarToggle("Notifications", "An Xbox 360-style pop-up when you unlock an achievement in game.",
                               "kk_achievement_toasts"));
    items.push_back(CvarToggle("Sound", "The sound that plays with each pop-up.", "kk_achievement_sound"));
    const bool sound = GetBool("kk_achievement_sound");
    const char* sound_note = "Switch Sound on to choose it.";
    {
      if (sounds_.empty() || ImGui::GetTime() - sounds_scanned_ > 3.0) {
        sounds_ = ListSounds(paths_.user_dir);
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
        PlayAchievementSound(paths_.user_dir);  // preview
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
      OpenInExplorer(SoundsDir(paths_.user_dir));
      sounds_.clear();  // rescan when the list is next built
    }));
    items.push_back(ActionItem("Test notification", "Shows a sample notification right now.", "", [this] {
      if (!GetBool("kk_achievement_toasts")) Status("Notifications are off; switch them on to see the test.");
      const uint32_t icon = achievements_.empty() ? 0 : achievements_[test_index_++ % achievements_.size()].id;
      toast_->Show("Test achievement", 10, icon);
    }));

    items.push_back(Section("PC port"));
    for (const auto& pa : PortAchievements()) {
      const bool unlocked = IsPortAchievementUnlocked(paths_.user_dir, pa.id);
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
    items.push_back(ActionItem("Project page", "The port's GitHub page: releases, issues and the source code.",
                               "GitHub", [] { OpenUrl(kProjectUrl); }));

    items.push_back(Section("Launcher"));
    items.push_back(CvarToggle("Show this launcher",
                               "Off starts the game directly. Hold Shift while starting to bring the launcher back.",
                               "kk_launcher", "Off", "At startup"));
    items.push_back(CvarToggle("Music", "The game's main menu music while the launcher is open, from your copy of the game.",
                               "kk_launcher_music"));
    {
      Item it = CvarSlider("Music volume", "How loud the launcher's music is.", "kk_launcher_music_volume", 0, 100, 5,
                           [](float v) { return std::to_string(int(std::lround(v))) + "%"; });
      items.push_back(std::move(Disable(it, !GetBool("kk_launcher_music"), "Switch Music on to set its volume.")));
    }
    items.push_back(CvarToggle("Menu sounds", "The game's own menu sounds as you move around the launcher and choose things.",
                               "kk_launcher_sounds"));
    {
      Item it = CvarSlider("Menu sound volume", "How loud the launcher's menu sounds are.",
                           "kk_launcher_sounds_volume", 0, 100, 5,
                           [](float v) { return std::to_string(int(std::lround(v))) + "%"; });
      items.push_back(std::move(Disable(it, !GetBool("kk_launcher_sounds"), "Switch Menu sounds on to set their volume.")));
    }
    items.push_back(CvarToggle("Check for updates",
                               "Checks GitHub for a newer version of the port each time the launcher opens, and offers "
                               "it. The shader pack keeps itself up to date either way.",
                               "kk_check_updates", "Off", "At startup"));
    items.push_back(ActionItem("Check for updates now", "Looks for a newer version of the port right now.",
                               update_ && update_->busy ? "Checking..." : "",
                               [this] { StartUpdateCheck(true); }));
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
    items.push_back(ActionItem("Open save folder", "Your saves, achievements and caches.", "",
                               [this] { OpenInExplorer(paths_.user_dir); }));
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
      const bool have = art::HasLauncherArt(ArtDir());
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
        return {"fullscreen", "window_width", "window_height", "monitor",
                "d3d12_allow_variable_refresh_rate_and_tearing", "present_letterbox"};
      case kGraphics:
        return {"kk_original_look", "kk_modern_settings", "present_effect", "kk_render_quality", "resolution_scale",
                "draw_resolution_scale_x", "draw_resolution_scale_y", "swap_post_effect", "anisotropic_override",
                "kk_motion_blur", "kk_big_blur", "kk_fog", "ao_mode", "ao_strength"};
      case kGameplay:
        return {"kk_frame_rate", "kk_fov", "kk_show_fps", "kk_skip_intros", "user_language"};
      case kControls:
        return {"mnk_mode", "kk_button_prompts", "kk_camera_sensitivity", "kk_camera_modern", "mnk_sensitivity",
                "mnk_mouse", "kk_invert_rs_x", "kk_invert_rs_y", "kk_invert_ls_x", "kk_invert_ls_y",
                "kk_toggle_aim", "kk_deadzone", "kk_vibration", "kk_vibration_strength", "kk_map_*",
                "@Input/Keybinds/Controller"};
      case kCheatsPage:
        return {"kk_cheats", "kk_cheat_*"};
      case kAchievements:
        return {"kk_achievement_toasts", "kk_achievement_sound", "kk_achievement_sound_file",
                "kk_achievement_volume"};
      default:
        return {};
    }
  }

  // Puts one page's settings back to their defaults; the other pages keep theirs.
  void ResetPage(Page page) {
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
    if (page == kDisplay && cb_.set_fullscreen) cb_.set_fullscreen(GetBool("fullscreen"));
    Status(F("{0} settings are back to their defaults.", {T(std::string(kPageNames[page]))}));
  }

  void ResetAllSettings() {
    for (auto& e : rex::cvar::GetRegistry()) {
      if (e.type == rex::cvar::FlagType::Command) continue;
      if (e.source == rex::cvar::Source::kCommandLine || e.source == rex::cvar::Source::kEnvironment) continue;
      rex::cvar::ResetToDefault(e.name);
    }
    ++g_changes;
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
  void StartUpdateCheck(bool manual) {
    if (update_ && update_->busy) return;
    update_ = std::make_shared<UpdateStatus>();
    update_manual_ = manual;
    update_prompted_ = false;
    std::thread([s = update_] { CheckForUpdate(*s); }).detach();
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
                  // The update screen shows the download (SetupStage 3).
                  installing_update_ = true;
                  update_->done = update_->failed = false;
                  update_->busy = true;
                  update_->bytes = update_->total = 0;
                  std::thread([s = update_, info] { InstallUpdate(info, *s); }).detach();
                } else if (i == 1) {
                  OpenUrl(info.page_url);
                  OpenUpdatePrompt();
                }
              });
  }

  void TickUpdate() {
    if (!update_) return;
    if (update_manual_ && !update_->busy && (update_->done || update_->failed) && !update_->found) {
      Status(update_->failed ? update_->message : F("You have the newest version (v{0}).", {KK_VERSION}));
      update_manual_ = false;
    }
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
    const int fps = GetInt("kk_frame_rate", 30);
    if (fps > 0 && fps <= 30) return StartGame();
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
                {"Back"}, 0, [this](int) { waiting_for_pack_ = false; });
      modal_.progress = [this] { return PackProgress(); };
      return;
    }
    played_ = true;
    SaveSettings(paths_.config_path);
    bool needs_restart = false;
    for (size_t i = 0; i < std::size(kRestartCvars); ++i)
      if (Get(kRestartCvars[i]) != restart_baseline_[i]) needs_restart = true;
    auto action = needs_restart ? cb_.restart_and_play : cb_.play;
    input_.Release();  // the game opens the controllers itself
    Close();  // deletes this dialog after the current draw
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
    FinishInstallIfDone();
    FinishArtIfDone();
    RefreshPack();
    TickUpdate();
    TickSteam();
    CaptureKey();
    if (open_whats_new_ && !modal_.open) {
      open_whats_new_ = false;
      OpenWhatsNew();
    }
    if (waiting_for_pack_ && !pack_.busy) {
      waiting_for_pack_ = false;
      modal_.open = false;
      StartGame();
    }
    if (preview_at_ > 0 && now >= preview_at_ && !ImGui::IsMouseDown(0)) {
      preview_at_ = 0;
      PlayAchievementSound(paths_.user_dir);
    }
    if (g_changes != seen_changes_) {
      seen_changes_ = g_changes;
      changed_time_ = now;
    }
    if (seen_changes_ != saved_changes_ && now - changed_time_ > 0.8 && !ImGui::IsMouseDown(0)) {
      if (SaveSettings(paths_.config_path)) saved_time_ = now;
      saved_changes_ = seen_changes_;
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
        {kCheatsPage, kNoSubPage, "cheats"}, {kAchievements, kNoSubPage, "achievements"},
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
  // ("setup:N" shows a stage of the setup screen) presses buttons (a, b, x, y,
  // lb, rb, start, up, down, left, right, lt, rt)
  // one at a time as a controller ("pad:", "ps:") or the keyboard ("kb:", the
  // default), logging "KK dev: input <step>"; "shot:<name>" logs
  // "KK dev: shot <name>" and waits for a capture. The mouse, at 1280 x 720
  // layout positions: "move:X:Y", "click:X:Y", "down:X:Y", "up", "wheel:N".
  void DevInput() {
    ImGuiIO& io = ImGui::GetIO();
    static bool release = false;
    if (release) {
      io.AddMouseButtonEvent(0, false);
      release = false;
    }
    static std::vector<std::string> steps = [] {
      std::vector<std::string> out;
      const char* v = std::getenv("KK_DEV_LAUNCHER_INPUT");
      std::stringstream in(v ? v : "");
      for (std::string t; std::getline(in, t, ',');)
        if (!t.empty()) out.push_back(t);
      return out;
    }();
    static size_t next = 0;
    static double at = 2.5;
    const double now = ImGui::GetTime();
    if (next >= steps.size() || now < at) return;
    std::string t = steps[next++];
    at = now + 0.45;
    REXLOG_INFO("KK dev: input {}", t);
    if (t.rfind("shot:", 0) == 0) {
      REXLOG_INFO("KK dev: shot {}", t.substr(5));
      at = now + 1.6;
      return;
    }
    auto at_point = [&](const std::string& xy) {
      const size_t colon = xy.find(':');
      const float x = std::stof(xy.substr(0, colon)), y = std::stof(xy.substr(colon + 1));
      io.AddMousePosEvent(o_.x + x * m_.s, o_.y + y * m_.s);
    };
    if (t.rfind("setup:", 0) == 0) {  // show a setup stage: 0 your copy, 1 install, 2 artwork; -1 back
      dev_setup_ = std::atoi(t.c_str() + 6);
      return;
    }
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
  LauncherCallbacks cb_;
  ui::Input input_;
  ui::Glyphs glyphs_;
  ui::Metrics m_;
  ui::Modal modal_;
  ImVec2 o_{0, 0};
  float w_ = 1280, h_ = 720;
  Page page_ = kHome;
  SubPage sub_ = kNoSubPage;
  std::map<int, ui::ListState> lists_;
  std::vector<Item> focused_items_;
  const Item* focused_ = nullptr;
  Action pending_click_ = Action::kCount;
  int home_focus_ = 0;
  std::vector<float> home_offset_;
  float home_mix_ = 1.0f;
  float tab_x_ = -1, tab_w_ = 0;

  std::vector<std::string> restart_baseline_;
  bool files_ok_ = false;
  bool played_ = false;
  std::shared_ptr<UpdateStatus> update_;
  std::shared_ptr<SteamState> steam_;
  bool update_manual_ = false, update_prompted_ = false, installing_update_ = false, relaunched_ = false;
  bool open_whats_new_ = false;
  std::string whats_new_from_;  // version the player had before this one
  ShaderPackStatus pack_;
  std::thread pack_thread_;
  int pack_installed_ = -1;        // -1: not read yet
  bool waiting_for_pack_ = false;  // Play was pressed during the download
  int autoplay_frames_ = 0;
  std::string status_;
  double status_time_ = -100, saved_time_ = -100;
  int seen_changes_ = 0, saved_changes_ = 0;
  double changed_time_ = 0;
  double preview_at_ = 0;
  std::string capturing_;
  std::vector<MonitorInfo> monitors_;

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
  int setup_focus_ = 0;
#if defined(KK_DEV_TOOLS)
  int dev_setup_ = -1;  // KK_DEV_LAUNCHER_SETUP: show a setup stage (screenshots)
#endif
  rex::ui::ImmediateTexture* title_icon_ = nullptr;
  std::vector<Achievement> achievements_;
  bool have_achievement_names_ = false;
  std::unique_ptr<AchievementToast> toast_;
  std::unique_ptr<LauncherMusic> music_;  // once the game is installed
  std::unique_ptr<LauncherSounds> ui_sounds_;
  size_t test_index_ = 0;
  std::vector<std::filesystem::path> sounds_;
  double sounds_scanned_ = -10.0;

  iso::Progress progress_;
  std::thread install_thread_;
  std::atomic<bool> install_done_{false};
  bool installing_ = false;
  std::string install_result_;
};

}  // namespace

bool GameFilesPresent(const std::filesystem::path& game_dir) {
  std::error_code ec;
  return !game_dir.empty() && std::filesystem::exists(game_dir / "default.xex", ec) &&
         std::filesystem::exists(game_dir / "KKMaps.bf", ec) && std::filesystem::exists(game_dir / "KKTextures.bf", ec);
}

void PreloadGpuPlugin() {
#if defined(_WIN32)
  const auto dir = rex::filesystem::GetExecutableFolder();
  for (const char* name : {"rexgpu-xenosrd.dll", "rexgpu-xenos.dll", "rexgpu-xenosd.dll"}) {
    if (std::filesystem::exists(dir / name) && LoadLibraryW((dir / name).c_str())) return;
  }
  REXLOG_WARN("KK: GPU plugin not found for preload; graphics settings unavailable in launcher");
#else
  const auto dir = rex::filesystem::GetExecutableFolder();
  for (const char* name : {"librexgpu-xenosrd.so", "librexgpu-xenos.so", "librexgpu-xenosd.so"}) {
    for (const auto& path : {dir / name, dir / ".." / "lib" / name})
      if (std::filesystem::exists(path) && dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL)) return;
  }
  REXLOG_WARN("KK: GPU plugin not found for preload; graphics settings unavailable in launcher");
#endif
}

void ShowLauncher(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, LauncherPaths paths,
                  LauncherCallbacks callbacks) {
  new Launcher(drawer, immediate, std::move(paths), std::move(callbacks));
}

}  // namespace kk
