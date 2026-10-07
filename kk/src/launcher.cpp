#include "launcher.h"
#include "shader_pack.h"
#include "update.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

constexpr uint32_t kTitleId = 0x555307D3;

// ------------------------------------------------------------------ theme ---
// Skull Island at night: warm near-black, weathered stone and torchlight amber.
const ImVec4 kBg = ImVec4(0.035f, 0.031f, 0.027f, 1.0f);
const ImVec4 kPanel = ImVec4(0.075f, 0.067f, 0.059f, 1.0f);
const ImVec4 kFrame = ImVec4(0.125f, 0.110f, 0.094f, 1.0f);
const ImVec4 kFrameHot = ImVec4(0.169f, 0.149f, 0.125f, 1.0f);
const ImVec4 kFrameActive = ImVec4(0.204f, 0.180f, 0.149f, 1.0f);
const ImVec4 kAccent = ImVec4(0.910f, 0.639f, 0.235f, 1.0f);
const ImVec4 kAccentHot = ImVec4(0.957f, 0.725f, 0.353f, 1.0f);
const ImVec4 kOnAccent = ImVec4(0.090f, 0.055f, 0.016f, 1.0f);
const ImVec4 kDim = ImVec4(0.690f, 0.643f, 0.580f, 1.0f);
const ImVec4 kGood = ImVec4(0.118f, 0.420f, 0.239f, 1.0f);
const ImVec4 kWarn = ImVec4(0.490f, 0.318f, 0.090f, 1.0f);

ImU32 Col(const ImVec4& c, float alpha_mul = 1.0f) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha_mul));
}

void ApplyTheme(float scale) {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowPadding = ImVec2(0, 0);
  s.WindowBorderSize = 0;
  s.ChildBorderSize = 0;
  s.PopupBorderSize = 0;
  s.FrameBorderSize = 0;
  s.WindowRounding = 0;
  s.ChildRounding = 12 * scale;
  s.FrameRounding = 7 * scale;
  s.PopupRounding = 8 * scale;
  s.GrabRounding = 7 * scale;
  s.ScrollbarRounding = 8 * scale;
  s.ScrollbarSize = 10 * scale;
  s.FramePadding = ImVec2(12 * scale, 8 * scale);
  s.ItemSpacing = ImVec2(10 * scale, 10 * scale);
  s.CellPadding = ImVec2(0, 12 * scale);
  s.GrabMinSize = 14 * scale;
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = ImVec4(0.95f, 0.97f, 1.0f, 1.0f);
  c[ImGuiCol_TextDisabled] = kDim;
  c[ImGuiCol_WindowBg] = kBg;
  c[ImGuiCol_ChildBg] = kPanel;
  c[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.110f, 0.212f, 0.99f);
  c[ImGuiCol_FrameBg] = kFrame;
  c[ImGuiCol_FrameBgHovered] = kFrameHot;
  c[ImGuiCol_FrameBgActive] = kFrameActive;
  c[ImGuiCol_Button] = kFrame;
  c[ImGuiCol_ButtonHovered] = kFrameHot;
  c[ImGuiCol_ButtonActive] = kFrameActive;
  c[ImGuiCol_Header] = kFrame;
  c[ImGuiCol_HeaderHovered] = kFrameHot;
  c[ImGuiCol_HeaderActive] = kFrameActive;
  c[ImGuiCol_SliderGrab] = kAccent;
  c[ImGuiCol_SliderGrabActive] = kAccentHot;
  c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = kFrameHot;
  c[ImGuiCol_ScrollbarGrabHovered] = kFrameActive;
  c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.07f);
  c[ImGuiCol_TableBorderLight] = ImVec4(1, 1, 1, 0.06f);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_NavCursor] = kAccent;
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

// ---------------------------------------------------------- cvar helpers ---
std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
bool GetBool(const char* name) { return Get(name) == "true"; }
int GetInt(const char* name, int fallback = 0) {
  try {
    return std::stoi(Get(name));
  } catch (...) {
    return fallback;
  }
}
void Set(const char* name, const std::string& value) { rex::cvar::SetFlagByName(name, value); }
void SetBool(const char* name, bool v) { Set(name, v ? "true" : "false"); }
void SetInt(const char* name, int v) { Set(name, std::to_string(v)); }

const rex::cvar::FlagEntry* FindFlag(std::string_view name) {
  for (auto& e : rex::cvar::GetRegistry())
    if (e.name == name) return &e;
  return nullptr;
}

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

// Settings that the presenter/window read before the launcher runs; changing
// them needs a relaunch to take effect.
constexpr const char* kRestartCvars[] = {"present_effect", "window_width", "window_height", "monitor"};

enum Page { kPlay, kDisplay, kGraphics, kGameplay, kControls, kCheatsPage, kAchievements, kAbout, kPageCount };
constexpr const char* kPageNames[kPageCount] = {"Play",     "Display", "Graphics",     "Gameplay",
                                                "Controls", "Cheats",  "Achievements", "About"};
constexpr const char* kPageBlurbs[kPageCount] = {
    "Install the game from your own disc image and start playing.",
    "Window, monitor and how the picture fits your screen.",
    "Render resolution, anti-aliasing and texture filtering.",
    "Frame rate, language and the frame counter.",
    "Camera, sticks, vibration, button remapping and keyboard play.",
    "The game's cheats, switched on for you when the game starts.",
    "Your progress on the game's 9 achievements.",
    "About this port, and where your saves and settings live.",
};

struct Achievement {
  uint32_t id = 0;
  std::string label, description, unachieved;
  uint32_t gamerscore = 0;
  bool unlocked = false;
  rex::ui::ImmediateTexture* icon = nullptr;
};

struct Option {
  const char* label;
  std::string value;
};

ImTextureRef Tex(rex::ui::ImmediateTexture* t) { return ImTextureRef(reinterpret_cast<ImTextureID>(t)); }

class Launcher final : public rex::ui::ImGuiDialog {
 public:
  Launcher(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, LauncherPaths paths,
           LauncherCallbacks callbacks)
      : ImGuiDialog(drawer), immediate_(immediate), paths_(std::move(paths)), cb_(std::move(callbacks)) {
    saved_style_ = ImGui::GetStyle();
    for (const char* name : kRestartCvars) restart_baseline_.push_back(Get(name));
    files_ok_ = GameFilesPresent(paths_.game_dir);
    LoadArt();
    toast_ = std::make_unique<AchievementToast>(drawer, immediate_, paths_.game_dir, paths_.user_dir);
    CleanUpAfterUpdate();
    const bool testing = std::getenv("KK_AUTOPLAY") != nullptr;
    if (GetBool("kk_check_updates") && !testing) StartUpdateCheck(false);
    open_poster_ = GetBool("kk_share_poster") && !testing;
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
  }

  ~Launcher() override {
    ImGui::GetStyle() = saved_style_;
    if (install_thread_.joinable()) {
      progress_.cancel = true;
      install_thread_.join();
    }
    if (pack_thread_.joinable()) pack_thread_.join();
    for (auto& texture : textures_) KeepTextureAlive(std::move(texture));
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    // Testing aid: KK_AUTOPLAY=1 presses Play after a couple of seconds.
    static const bool autoplay = std::getenv("KK_AUTOPLAY") != nullptr;
    if (autoplay && files_ok_ && ++autoplay_frames_ == 120) StartGame();
#if defined(KK_DEV_TOOLS)
    DevTour();
#endif
    s_ = ImGui::GetFontSize() / 18.0f;
    ApplyTheme(s_);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##kk_launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    const float w = vp->Size.x, h = vp->Size.y;
    const float header = std::clamp(h * 0.2f, 120 * s_, 190 * s_);
    const float footer = 78 * s_;
    const float margin = 22 * s_;

    DrawHeader(vp->Pos, w, header);

    const float body_top = header + margin * 0.6f;
    const float body_h = h - body_top - footer;
    const float sidebar_w = std::clamp(w * 0.17f, 170 * s_, 230 * s_);

    ImGui::SetCursorPos(ImVec2(margin, body_top));
    DrawSidebar(ImVec2(sidebar_w, body_h));
    ImGui::SetCursorPos(ImVec2(margin * 2 + sidebar_w, body_top));
    DrawContent(ImVec2(w - sidebar_w - margin * 3, body_h));

    DrawFooter(ImVec2(vp->Pos.x + margin, vp->Pos.y + h - footer), w - margin * 2, footer);
    HandleHotkeys();
    DrawFrameRateWarning();
    DrawWhatsNew();
    DrawSharePoster();
    DrawUpdatePrompt();
    DrawPackPrompt();
    ImGui::End();
    (void)io;
  }

 private:
  // ---------------------------------------------------------------- art ---
  rex::ui::ImmediateTexture* MakeTexture(const art::Image& img) {
    if (!img || !immediate_) return nullptr;
    textures_.push_back(immediate_->CreateTexture(uint32_t(img.width), uint32_t(img.height),
                                                  rex::ui::ImmediateTextureFilter::kLinear, false,
                                                  img.rgba.data()));
    return textures_.back().get();
  }

  void LoadArt() {
    if (auto img = art::LoadImage(art::TitleCapturePath(paths_.user_dir))) {
      title_art_ = MakeTexture(img);
      title_art_aspect_ = float(img.width) / float(img.height);
    }
    title_icon_ = MakeTexture(art::LoadImage(art::TitleIconPath(paths_.game_dir)));
    if (auto img = art::LoadImage(rex::filesystem::GetExecutableFolder() / "launcher_art" / "shaders_poster.png")) {
      poster_ = MakeTexture(img);
      poster_aspect_ = float(img.width) / float(img.height);
    }
    LoadAchievements();
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

  // ------------------------------------------------------------- header ---
  void DrawStarfield(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    const float t = float(ImGui::GetTime());
    uint32_t seed = 0x5841u;
    auto rnd = [&seed] {
      seed = seed * 1664525u + 1013904223u;
      return float(seed >> 8) / float(1 << 24);
    };
    for (int i = 0; i < 150; ++i) {
      const ImVec2 p(p0.x + rnd() * (p1.x - p0.x), p0.y + rnd() * (p1.y - p0.y));
      const float size = (0.6f + rnd() * 1.4f) * s_;
      const float speed = 0.6f + rnd() * 1.8f, phase = rnd() * 6.28f;
      const float twinkle = 0.55f + 0.45f * std::sin(t * speed + phase);
      dl->AddCircleFilled(p, size, IM_COL32(255, 255, 255, int(200 * twinkle)));
    }
  }

  void DrawHeader(ImVec2 origin, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(origin.x + w, origin.y + h);
    if (title_art_) {
      // The game's save menu backdrop (captured on first play): the sky, moon
      // and cliffs to the right of the game's logo and above its menu text.
      constexpr float kU0 = 0.30f, kV0 = 0.12f;
      const float band = std::min(1.0f - kV0, (h / w) * title_art_aspect_ * (1.0f - kU0));
      dl->AddImage(Tex(title_art_), origin, p1, ImVec2(kU0, kV0), ImVec2(1, kV0 + band));
    } else {
      // Night sky over Skull Island: a misty moon behind jagged peaks.
      dl->AddRectFilledMultiColor(origin, p1, IM_COL32(18, 24, 30, 255), IM_COL32(26, 30, 34, 255),
                                  IM_COL32(12, 12, 12, 255), IM_COL32(10, 10, 10, 255));
      DrawStarfield(dl, origin, ImVec2(p1.x, origin.y + h * 0.6f));
      const ImVec2 moon(origin.x + w * 0.78f, origin.y + h * 0.42f);
      for (int i = 6; i > 0; --i)
        dl->AddCircleFilled(moon, h * (0.22f + 0.09f * i), IM_COL32(220, 210, 180, 6), 64);
      dl->AddCircleFilled(moon, h * 0.22f, IM_COL32(232, 224, 200, 235), 64);
      dl->AddCircleFilled(ImVec2(moon.x + h * 0.06f, moon.y - h * 0.04f), h * 0.05f, IM_COL32(205, 196, 170, 200), 32);
      // Two layers of peaks, the nearer one darker.
      auto ridge = [&](float base, float height, uint32_t seed, ImU32 color) {
        float x = origin.x;
        while (x < p1.x) {
          seed = seed * 1664525u + 1013904223u;
          const float span = h * (0.35f + float(seed >> 24) / 255.0f * 0.5f);
          const float peak = base - height * (0.4f + float((seed >> 16) & 255) / 255.0f * 0.6f);
          dl->AddTriangleFilled(ImVec2(x, p1.y), ImVec2(x + span * 0.5f, peak), ImVec2(x + span, p1.y), color);
          dl->AddRectFilled(ImVec2(x, base), ImVec2(x + span, p1.y), color);
          x += span * 0.6f;
        }
      };
      ridge(origin.y + h * 0.85f, h * 0.45f, 7u, IM_COL32(28, 32, 30, 255));
      ridge(origin.y + h * 0.95f, h * 0.35f, 23u, IM_COL32(14, 15, 14, 255));
      // Mist drifting over the ridges.
      dl->AddRectFilledMultiColor(ImVec2(origin.x, origin.y + h * 0.55f), p1, IM_COL32(150, 160, 160, 0),
                                  IM_COL32(150, 160, 160, 0), IM_COL32(150, 160, 160, 40), IM_COL32(150, 160, 160, 40));
    }
    // Shade the left side for the title and fade the bottom into the page.
    dl->AddRectFilledMultiColor(origin, ImVec2(origin.x + w * 0.65f, p1.y), Col(kBg, 0.82f), Col(kBg, 0.0f),
                                Col(kBg, 0.0f), Col(kBg, 0.82f));
    dl->AddRectFilledMultiColor(ImVec2(origin.x, p1.y - h * 0.35f), p1, Col(kBg, 0.0f), Col(kBg, 0.0f),
                                Col(kBg, 1.0f), Col(kBg, 1.0f));

    float x = origin.x + 30 * s_;
    if (title_icon_) {
      const float icon = h * 0.46f;
      const ImVec2 i0(x, origin.y + (h - icon) * 0.45f);
      dl->AddImageRounded(Tex(title_icon_), i0, ImVec2(i0.x + icon, i0.y + icon), ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32_WHITE, 10 * s_);
      x += icon + 20 * s_;
    }
    const UiFonts& f = GetUiFonts();
    const float title_size = std::clamp(h * 0.27f, 30 * s_, 46 * s_);
    const float title_y = origin.y + h * 0.5f - title_size * 0.85f;
    const char* title = "KING KONG";
    dl->AddText(f.bold, title_size, ImVec2(x + 2 * s_, title_y + 3 * s_), IM_COL32(0, 0, 0, 150), title);
    dl->AddText(f.bold, title_size, ImVec2(x, title_y), IM_COL32_WHITE, title);
    dl->AddText(f.semibold, 15 * s_, ImVec2(x + 2 * s_, title_y + title_size * 1.15f), Col(kAccent),
                "PETER JACKSON'S   \xC2\xB7   PC PORT");
  }

  // ------------------------------------------------------------ sidebar ---
  void DrawSidebar(ImVec2 size) {
    ImGui::BeginChild("##sidebar", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    const float pad = 10 * s_;
    // Shrink the entries when the window is short, so every page (About is last) stays reachable.
    const float gap = 2 * s_ + ImGui::GetStyle().ItemSpacing.y * 2;
    const float item_h = std::clamp((size.y - pad * 2) / kPageCount - gap, 30 * s_, 46 * s_);
    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImGui::PushFont(GetUiFonts().semibold, 0.0f);
    for (int i = 0; i < kPageCount; ++i) {
      const bool selected = page_ == i;
      ImGui::SetCursorPosX(pad);
      ImGui::PushID(i);
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      const ImVec2 item(size.x - pad * 2, item_h);
      if (ImGui::InvisibleButton("##page", item)) page_ = Page(i);
      const bool hot = ImGui::IsItemHovered();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      if (selected || hot)
        dl->AddRectFilled(pos, ImVec2(pos.x + item.x, pos.y + item.y), Col(selected ? kAccent : kFrame), 8 * s_);
      dl->AddText(ImVec2(pos.x + 16 * s_, pos.y + (item_h - ImGui::GetFontSize()) * 0.5f),
                  Col(selected ? kOnAccent : ImVec4(0.92f, 0.95f, 1, 1)), kPageNames[i]);
      if (i == kPlay && !files_ok_)
        dl->AddCircleFilled(ImVec2(pos.x + item.x - 18 * s_, pos.y + item_h * 0.5f), 4.5f * s_,
                            Col(ImVec4(1.0f, 0.65f, 0.25f, 1)));
      ImGui::PopID();
      ImGui::Dummy(ImVec2(0, 2 * s_));
    }
    ImGui::PopFont();
    ImGui::EndChild();
  }

  // ------------------------------------------------------------ content ---
  void DrawContent(ImVec2 size) {
    ImGui::BeginChild("##content", size, ImGuiChildFlags_None);
    const float pad = 26 * s_;
    ImGui::SetCursorPos(ImVec2(pad, pad * 0.8f));
    ImGui::BeginGroup();
    ImGui::PushFont(GetUiFonts().semibold, 26.0f);
    ImGui::TextUnformatted(kPageNames[page_]);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(kPageBlurbs[page_]);
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    if (!PageSettings(page_).empty()) {
      // "Reset page" at the top right, with a confirmation.
      const char* label = "Reset page";
      const float bw = ImGui::CalcTextSize(label).x + 28 * s_;
      const float below = ImGui::GetCursorPosY();
      ImGui::SetCursorPos(ImVec2(size.x - pad - bw, pad * 0.8f + 4 * s_));
      if (ImGui::Button(label, ImVec2(bw, 0))) ImGui::OpenPopup("Reset page?");
      ImGui::SetCursorPosY(below);
      if (BeginModal("Reset page?", 480)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::Text("Put the %s settings back to their defaults?", kPageNames[page_]);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("Only this page changes. Settings on the other pages stay as they are.");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 10 * s_));
        const float half = (ImGui::GetContentRegionAvail().x - 8 * s_) / 2;
        if (AccentButton("Reset", ImVec2(half, 0))) {
          ResetPage(page_);
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, 8 * s_);
        if (ImGui::Button("Cancel", ImVec2(half, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
    }

    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + 8 * s_));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##page_body", ImVec2(size.x - pad * 2, size.y - ImGui::GetCursorPosY() - pad * 0.6f),
                      ImGuiChildFlags_None);
    switch (page_) {
      case kPlay: PagePlay(); break;
      case kDisplay: PageDisplay(); break;
      case kGraphics: PageGraphics(); break;
      case kGameplay: PageGameplay(); break;
      case kControls: PageControls(); break;
      case kCheatsPage: PageCheats(); break;
      case kAchievements: PageAchievements(); break;
      case kAbout: PageAbout(); break;
      default: break;
    }
    ImGui::Dummy(ImVec2(0, 8 * s_));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndChild();
  }

  // Rows: a label and description on the left, the control on the right.
  bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    return true;
  }
  void EndRows() { ImGui::EndTable(); }

  void Row(const char* label, const char* desc) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushFont(GetUiFonts().semibold, 0.0f);
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
    if (desc && *desc) {
      ImGui::PushStyleColor(ImGuiCol_Text, kDim);
      ImGui::PushFont(nullptr, 15.0f);
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 24 * s_);
      ImGui::TextUnformatted(desc);
      ImGui::PopTextWrapPos();
      ImGui::PopFont();
      ImGui::PopStyleColor();
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
  }

  // Segmented buttons; returns the index clicked (or -1).
  int Segmented(const char* id, const std::vector<std::string>& labels, int selected) {
    ImGui::PushID(id);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 6 * s_;
    const float bw = (avail - gap * float(labels.size() - 1)) / float(labels.size());
    int clicked = -1;
    for (size_t i = 0; i < labels.size(); ++i) {
      if (i) ImGui::SameLine(0, gap);
      const bool sel = int(i) == selected;
      if (sel) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHot);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHot);
        ImGui::PushStyleColor(ImGuiCol_Text, kOnAccent);
      }
      ImGui::PushID(int(i));
      if (ImGui::Button(labels[i].c_str(), ImVec2(bw, 0))) clicked = int(i);
      ImGui::PopID();
      if (sel) ImGui::PopStyleColor(4);
    }
    ImGui::PopID();
    return clicked;
  }

  // Segmented control bound to a cvar's string value; options the cvar refuses
  // in this build are hidden.
  void ChoiceCvar(const char* cvar, std::vector<Option> options) {
    if (const auto* flag = FindFlag(cvar); flag && !flag->constraints.allowed_values.empty()) {
      const auto& allowed = flag->constraints.allowed_values;
      std::erase_if(options, [&](const Option& o) {
        return std::find(allowed.begin(), allowed.end(), o.value) == allowed.end();
      });
    }
    const std::string cur = Get(cvar);
    std::vector<std::string> labels;
    int sel = -1;
    for (size_t i = 0; i < options.size(); ++i) {
      labels.push_back(options[i].label);
      if (options[i].value == cur) sel = int(i);
    }
    if (int i = Segmented(cvar, labels, sel); i >= 0) Set(cvar, options[size_t(i)].value);
  }

  void ToggleCvar(const char* cvar, const char* off = "Off", const char* on = "On", bool invert = false) {
    const bool v = GetBool(cvar) != invert;
    if (int i = Segmented(cvar, {off, on}, v ? 1 : 0); i >= 0) SetBool(cvar, (i == 1) != invert);
  }

  void ComboCvar(const char* cvar, const std::vector<Option>& options) {
    const std::string cur = Get(cvar);
    const char* preview = options.empty() ? "" : options.front().label;
    for (auto& o : options)
      if (o.value == cur) preview = o.label;
    ImGui::PushID(cvar);
    if (ImGui::BeginCombo("##c", preview, ImGuiComboFlags_HeightLarge)) {
      for (auto& o : options)
        if (ImGui::Selectable(o.label, o.value == cur)) Set(cvar, o.value);
      ImGui::EndCombo();
    }
    ImGui::PopID();
  }

  void SliderCvar(const char* cvar, int lo, int hi, const char* fmt) {
    int v = GetInt(cvar, lo);
    ImGui::PushID(cvar);
    if (ImGui::SliderInt("##s", &v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) SetInt(cvar, v);
    ImGui::PopID();
  }

  bool AccentButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHot);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHot);
    ImGui::PushStyleColor(ImGuiCol_Text, kOnAccent);
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
  }

  // --------------------------------------------------------------- Play ---
  void StatusCard() {
    const bool ok = files_ok_ && !installing_;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 76 * s_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(installing_ ? kFrame : ok ? kGood : kWarn), 12 * s_);
    dl->AddCircleFilled(ImVec2(p.x + 34 * s_, p.y + h * 0.5f), 12 * s_,
                        Col(ok ? ImVec4(0.55f, 0.95f, 0.7f, 1) : ImVec4(1.0f, 0.75f, 0.35f, 1)));
    ImGui::SetCursorScreenPos(ImVec2(p.x + 64 * s_, p.y + 14 * s_));
    ImGui::BeginGroup();
    ImGui::PushFont(GetUiFonts().semibold, 0.0f);
    ImGui::TextUnformatted(installing_ ? "Installing..." : ok ? "Ready to play" : "Game files needed");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.9f, 0.95f, 1));
    if (installing_) {
      const double total = std::max<double>(1.0, double(progress_.bytes_total.load()));
      ImGui::ProgressBar(float(progress_.bytes_done.load() / total), ImVec2(w - 100 * s_, 6 * s_), "");
    } else {
      ImGui::TextUnformatted(ok ? paths_.game_dir.string().c_str()
                                : "Install from your King Kong Xbox 360 disc image below.");
    }
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 14 * s_));
  }

  void PagePlay() {
    FinishInstallIfDone();
    StatusCard();
    if (!install_message_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kDim);
      ImGui::TextWrapped("%s", install_message_.c_str());
      ImGui::PopStyleColor();
    }
    if (!BeginRows("##play")) return;
    Row("Disc image",
        "Your own Peter Jackson's King Kong Xbox 360 disc image (.iso). Its files (about 6.3 GB) are copied "
        "next to the game.");
    ImGui::BeginDisabled(installing_);
    if (files_ok_ ? ImGui::Button("Reinstall from disc image...", ImVec2(-FLT_MIN, 0))
                  : AccentButton("Install from disc image...", ImVec2(-FLT_MIN, 0)))
      StartInstall();
    ImGui::EndDisabled();
    if (installing_ && ImGui::Button("Cancel install", ImVec2(-FLT_MIN, 0))) progress_.cancel = true;
    ShaderPackRow();
    Row("Share my shaders",
        "Played a good part of the game? Share your shaders and they go into future shader packs, so other "
        "players' first play-through runs smoother. Only shader data goes into one small file, and a GitHub page "
        "opens to drop it in (posting needs a free GitHub account).");
    if (ImGui::Button("Share my shaders", ImVec2(-FLT_MIN, 0))) {
      std::string error;
      const auto zip = PackShadersForSharing(CacheDir(), paths_.user_dir, error);
      if (zip.empty()) {
        share_message_ = error;
      } else {
        RevealInExplorer(zip);
        OpenUrl(kShareShadersUrl);
        share_message_ = "Saved " + zip.filename().string() +
                         ". Drag it into the GitHub page that just opened, then press Submit. Thank you!";
      }
    }
    if (!share_message_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kDim);
      ImGui::TextWrapped("%s", share_message_.c_str());
      ImGui::PopStyleColor();
    }
    Row("Show this launcher", "Off starts the game directly. Hold Shift while starting to bring it back.");
    ToggleCvar("kk_launcher", "Off", "At startup");
    EndRows();
  }

  std::filesystem::path CacheDir() const {
    const std::string root = Get("cache_root");
    return root.empty() ? paths_.user_dir / "cache" : std::filesystem::path(root);
  }

  void ShaderPackRow() {
    Row("Shader pack",
        "Effects already prepared by playing through every chapter. With it the game prepares them all as it "
        "starts, so it never pauses for a new effect. Downloads from the port's GitHub page.");
    if (pack_installed_ < 0) pack_installed_ = InstalledShaderPackVersion(CacheDir());
    if (pack_.busy) {
      const float total = float(pack_.total.load()), got = float(pack_.bytes.load());
      ImGui::ProgressBar(total > 0 ? got / total : 0.0f, ImVec2(-FLT_MIN, 0),
                         total > 0 ? nullptr : "Connecting...");
      return;
    }
    if (pack_thread_.joinable()) {
      pack_thread_.join();
      pack_installed_ = InstalledShaderPackVersion(CacheDir());
    }
    const bool installed = pack_installed_ > 0;
    if (installed ? ImGui::Button("Check for a newer pack", ImVec2(-FLT_MIN, 0))
                  : AccentButton("Download shader pack", ImVec2(-FLT_MIN, 0)))
      StartPackDownload();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (pack_.done || pack_.failed) ImGui::TextWrapped("%s", pack_.message.c_str());
    else if (installed) ImGui::Text("Pack %d installed.", pack_installed_);
    ImGui::PopStyleColor();
  }

  void StartPackDownload() {
    if (pack_.busy) return;
    if (pack_thread_.joinable()) pack_thread_.join();
    pack_.done = pack_.failed = false;
    pack_.busy = true;
    pack_thread_ = std::thread([this, dir = CacheDir()] { DownloadAndInstallShaderPack(dir, pack_); });
  }

  // When the launcher opens (with update checks on): if a newer shader pack is
  // published than the one installed, offer it. Players without a pack get the
  // Download button on the Play page instead. Waits for an app update offer
  // first, as updating restarts the launcher.
  void DrawPackPrompt() {
    constexpr const char* kTitle = "Shader pack update";
    const int published = pack_published_->load();
    if (!pack_prompted_ && published > 0 && !(update_ && (update_->busy || update_->found)) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
      pack_prompted_ = true;  // only ever ask once per launch
      if (pack_installed_ < 0) pack_installed_ = InstalledShaderPackVersion(CacheDir());
      if (pack_installed_ > 0 && published > pack_installed_) ImGui::OpenPopup(kTitle);
    }
    if (!BeginModal(kTitle, 560)) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::Text("Shader pack %d is out. You have pack %d.", published, pack_installed_);
    ImGui::Dummy(ImVec2(0, 6 * s_));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted("It adds effects prepared by playing further through the game, so it pauses for new "
                           "effects less often. It's a small download and keeps everything your game has already "
                           "prepared.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 10 * s_));
    if (pack_.busy) {
      const float total = float(pack_.total.load()), got = float(pack_.bytes.load());
      ImGui::ProgressBar(total > 0 ? got / total : 0.0f, ImVec2(-FLT_MIN, 0), total > 0 ? nullptr : "Downloading...");
    } else if (pack_.done || pack_.failed) {
      if (pack_thread_.joinable()) {
        pack_thread_.join();
        pack_installed_ = InstalledShaderPackVersion(CacheDir());
      }
      ImGui::TextWrapped("%s", pack_.message.c_str());
      ImGui::Dummy(ImVec2(0, 6 * s_));
      if (ImGui::Button("Close", ImVec2(-FLT_MIN, 0))) ImGui::CloseCurrentPopup();
    } else {
      const float bw = (ImGui::GetContentRegionAvail().x - 8 * s_) / 2;
      if (AccentButton("Download now", ImVec2(bw, 0))) StartPackDownload();
      ImGui::SameLine(0, 8 * s_);
      if (ImGui::Button("Later", ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  void StartInstall() {
    const auto pkg = BrowseForDiscImage();
    if (pkg.empty()) return;
    const uint32_t title = iso::ReadTitleId(pkg);
    if (title == 0) {
      install_message_ = "That file is not an Xbox 360 disc image.";
      return;
    }
    if (title != kTitleId) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "That disc is title %08X, not King Kong (%08X).", title, kTitleId);
      install_message_ = buf;
      return;
    }
    install_message_.clear();
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
    install_message_ = install_result_.empty()
                           ? (files_ok_ ? "Installed successfully." : "Extraction finished but default.xex is missing.")
                           : "Install failed: " + install_result_;
    if (files_ok_) {
      textures_.clear();
      title_art_ = title_icon_ = nullptr;
      LoadArt();
    }
  }

  // ------------------------------------------------------------ Display ---
  void PageDisplay() {
    if (!BeginRows("##display")) return;
    Row("Window mode", "Fullscreen uses a borderless window at your desktop resolution.");
    {
      const bool fs = GetBool("fullscreen");
      if (int i = Segmented("mode", {"Windowed", "Fullscreen"}, fs ? 1 : 0); i >= 0) SetFullscreen(i == 1);
    }
    Row("Window size", "Size of the window in windowed mode. Applies when the game starts.");
    WindowSizeCombo();
    Row("Monitor", "Which display the game opens on. Applies when the game starts.");
    MonitorCombo();
    Row("VSync", "On waits for the display for tear-free frames. Off has the lowest latency and lets "
                 "G-Sync/FreeSync displays run freely.");
    ToggleCvar("d3d12_allow_variable_refresh_rate_and_tearing", "Off", "On", /*invert=*/true);
    Row("Aspect ratio", "The game is 16:9. Letterbox keeps its shape on other screens; stretch fills them.");
    {
      const bool letterbox = GetBool("present_letterbox");
      if (int i = Segmented("aspect", {"Letterbox 16:9", "Stretch"}, letterbox ? 0 : 1); i >= 0)
        SetBool("present_letterbox", i == 0);
    }
    EndRows();
  }

  void SetFullscreen(bool on) {
    SetBool("fullscreen", on);
    if (cb_.set_fullscreen) cb_.set_fullscreen(on);
  }

  void WindowSizeCombo() {
    // window_width/height are in logical pixels (96 DPI); offer real pixel sizes
    // that fit on the screen and convert with the window's DPI scale.
    const double scale = cb_.dpi_scale ? cb_.dpi_scale() : 1.0;
    auto to_physical = [&](int logical) { return int(logical * scale + 0.5); };
    const int w = to_physical(GetInt("window_width")), h = to_physical(GetInt("window_height"));
    static const std::pair<int, int> kSizes[] = {{0, 0},       {1280, 720},  {1600, 900},
                                                 {1920, 1080}, {2560, 1440}, {3200, 1800}};
    auto label = [](int sw, int sh) {
      return sw == 0 ? std::string("Default") : std::to_string(sw) + " \xC3\x97 " + std::to_string(sh);
    };
    const auto [screen_w, screen_h] = cb_.screen_size ? cb_.screen_size() : std::pair<int, int>{1 << 16, 1 << 16};
    auto close_to = [](int a, int b) { return std::abs(a - b) <= 2; };
    ImGui::BeginDisabled(GetBool("fullscreen"));
    if (ImGui::BeginCombo("##winsize", label(w, h).c_str())) {
      for (auto [sw, sh] : kSizes) {
        if (sw >= screen_w || sh >= screen_h) continue;  // must fit with its frame
        if (ImGui::Selectable(label(sw, sh).c_str(), close_to(sw, w) && close_to(sh, h))) {
          SetInt("window_width", int(sw / scale + 0.5));
          SetInt("window_height", int(sh / scale + 0.5));
        }
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
  }

  void MonitorCombo() {
    if (monitors_.empty()) monitors_ = ListMonitors();
    std::vector<std::string> names;
    for (size_t i = 0; i < monitors_.size(); ++i) {
      const auto& m = monitors_[i];
      names.push_back("Display " + std::to_string(i + 1) + (m.primary ? " (primary)" : "") + "   " +
                      std::to_string(m.width) + " \xC3\x97 " + std::to_string(m.height));
    }
    std::vector<Option> opts = {{"Default", "0"}};
    for (size_t i = 0; i < names.size(); ++i) opts.push_back({names[i].c_str(), std::to_string(i + 1)});
    ComboCvar("monitor", opts);
  }

  // ----------------------------------------------------------- Graphics ---
  void PageGraphics() {
    const auto [out_w, out_h] = cb_.output_size ? cb_.output_size() : std::pair<int, int>{1280, 720};
    if (!BeginRows("##graphics")) return;
    Row("Render quality",
        "How sharply the game is drawn compared with your screen. Native matches it; Quality, Balanced and the "
        "Performance modes draw fewer pixels and scale up; Supersample draws more for the cleanest edges. The "
        "game renders in steps of its original 720p.");
    {
      const std::string cur = Get("kk_render_quality");
      std::vector<std::string> labels, values;
      int sel = -1;
      for (const auto& p : RenderPresets()) {
        const int scale = RenderScaleFor(p.id, out_h);
        labels.push_back(std::string(p.label) + "   " + std::to_string(1280 * scale) + " \xC3\x97 " +
                         std::to_string(720 * scale));
        values.push_back(p.id);
      }
      labels.push_back("Custom");
      values.push_back("custom");
      for (size_t i = 0; i < values.size(); ++i)
        if (values[i] == cur) sel = int(i);
      ImGui::PushID("quality");
      if (ImGui::BeginCombo("##q", sel >= 0 ? labels[size_t(sel)].c_str() : cur.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (size_t i = 0; i < labels.size(); ++i)
          if (ImGui::Selectable(labels[i].c_str(), int(i) == sel)) Set("kk_render_quality", values[i]);
        ImGui::EndCombo();
      }
      ImGui::PopID();
      ImGui::PushStyleColor(ImGuiCol_Text, kDim);
      ImGui::PushFont(nullptr, 15.0f);
      ImGui::Text("%s: %d \xC3\x97 %d", GetBool("fullscreen") ? "Your screen" : "Game window", out_w, out_h);
      ImGui::PopFont();
      ImGui::PopStyleColor();
    }
    if (Get("kk_render_quality") == "custom") {
      Row("Internal resolution", "Draw the game at an exact multiple of its native 1280 \xC3\x97 720.");
      ComboCvar("resolution_scale", {{"1\xC3\x97   1280 \xC3\x97 720 (original)", "1"},
                                     {"2\xC3\x97   2560 \xC3\x97 1440", "2"},
                                     {"3\xC3\x97   3840 \xC3\x97 2160 (4K)", "3"},
                                     {"4\xC3\x97   5120 \xC3\x97 2880", "4"},
                                     {"5\xC3\x97   6400 \xC3\x97 3600", "5"},
                                     {"6\xC3\x97   7680 \xC3\x97 4320 (8K)", "6"}});
    }
    Row("Anti-aliasing", "Smooths jagged edges after the frame is drawn. Extreme is softer but cleaner.");
    ChoiceCvar("swap_post_effect", {{"Off", "none"}, {"FXAA", "fxaa"}, {"FXAA Extreme", "fxaa_extreme"}});
    // 2x MSAA is not offered as a choice: it is how the Xbox 360 drew those
    // surfaces (native_2x_msaa, on by default), and the shader pack is built with it.
    Row("Texture filtering", "Keeps the ground and distant textures sharp at steep angles.");
    ChoiceCvar("anisotropic_override",
               {{"Game", "-1"}, {"Off", "0"}, {"2\xC3\x97", "2"}, {"4\xC3\x97", "3"}, {"8\xC3\x97", "4"}, {"16\xC3\x97", "5"}});
    Row("Motion blur", "The trail the game blends over fast moments, mostly in Kong's sequences and some transitions.");
    ToggleCvar("kk_motion_blur", "Off", "On");
    Row("Shader preparing",
        "Each new effect is prepared the first time it appears, then saved for next time. Wait draws it "
        "correctly with a short pause, the first time only. Background avoids the pause, but objects can "
        "briefly vanish, turn into silhouettes or flash bright.");
    ToggleCvar("async_shader_compilation", "Wait", "Background");
    EndRows();
  }

  // ----------------------------------------------------------- Gameplay ---
  void PageGameplay() {
    if (!BeginRows("##gameplay")) return;
    Row("Frame rate",
        "30 matches the Xbox 360 and keeps every animation right. Higher is smoother, but some character "
        "animations are not right above 30 yet.");
    {
      std::vector<std::string> labels;
      for (int f : kFrameRateChoices) labels.push_back(f <= 0 ? "Unlimited" : std::to_string(f));
      std::vector<Option> opts;
      for (size_t i = 0; i < labels.size(); ++i) opts.push_back({labels[i].c_str(), std::to_string(kFrameRateChoices[i])});
      ChoiceCvar("kk_frame_rate", opts);
    }
    Row("Field of view", "How wide the camera sees. 69\xC2\xB0 is the original for Jack; Kong, cutscene and other "
                         "cameras widen by the same amount. Jack's gun keeps its usual size.");
    SliderCvar("kk_fov", 69, 110, GetInt("kk_fov", 69) <= 69 ? "%d\xC2\xB0 (original)" : "%d\xC2\xB0");
    Row("Frame counter", "Shows the game's frame rate in the corner. F2 toggles it while playing.");
    ToggleCvar("kk_show_fps", "Hidden", "Shown");
    Row("Startup logos", "The Ubisoft, Universal and WingNut movies before the title screen. Story movies still play.");
    ToggleCvar("kk_skip_intros", "Play", "Skip");
    Row("Language", "The game's language, where the game includes it.");
    // The disc's languages that the Xbox 360 system language can select.
    ComboCvar("user_language", {{"English", "1"},
                                {"Deutsch", "3"},
                                {"Espa\xC3\xB1ol", "5"},
                                {"Fran\xC3\xA7" "ais", "4"},
                                {"Italiano", "6"}});
    EndRows();
  }

  // ----------------------------------------------------------- Controls ---
  void PageControls() {
    if (BeginRows("##controls")) {
      Row("Input", "Controllers work automatically. Keyboard & mouse emulates a controller; keys are below.");
      ToggleCvar("mnk_mode", "Controller", "Keyboard & mouse");
      Row("Button prompts", "Which controller's buttons the game shows in menus and hints.");
      ComboCvar("kk_button_prompts", {{"Xbox 360 (original)", "xbox360"},
                                      {"Xbox Series", "xbox_series"},
                                      {"PlayStation 5", "ps5"},
                                      {"PlayStation 2", "ps2"},
                                      {"Keyboard (your keys)", "keyboard"}});
      Row("Controller sensitivity", "How fast the right stick turns the camera and moves your aim.");
      SliderCvar("kk_camera_sensitivity", 25, 300, "%d%%");
      Row("Camera response",
          "Modern turns the camera the same way in every direction, so circles and diagonals feel even. Original is "
          "the Xbox 360's own: small pushes up and down turn much more slowly than small pushes sideways.");
      ToggleCvar("kk_camera_modern", "Original", "Modern");
      {
        // Mouse settings apply to Keyboard & mouse input; shown either way so they're easy to find.
        const bool mnk = GetBool("mnk_mode");
        Row("Mouse sensitivity", mnk ? "How fast the mouse turns the camera."
                                     : "How fast the mouse turns the camera. Used when Input is Keyboard & mouse.");
        ImGui::BeginDisabled(!mnk);
        float sens = 1.0f;
        try {
          sens = std::stof(Get("mnk_sensitivity"));
        } catch (...) {
        }
        if (ImGui::SliderFloat("##ms", &sens, 0.1f, 5.0f, "%.2f\xC3\x97")) Set("mnk_sensitivity", std::to_string(sens));
        ImGui::EndDisabled();
        Row("Mouse camera", "Move the camera (right stick) with the mouse.");
        ImGui::BeginDisabled(!mnk);
        ToggleCvar("mnk_mouse");
        ImGui::EndDisabled();
      }
      Row("Camera horizontal", "Which way the camera turns when you push the right stick left or right.");
      ToggleCvar("kk_invert_rs_x", "Normal", "Inverted");
      Row("Camera vertical", "Which way the right stick moves the camera up and down.");
      ToggleCvar("kk_invert_rs_y", "Normal", "Inverted");
      Row("Aim", "Hold: keep the left trigger held to raise the gun, as on the console. Toggle: press it once "
                 "to raise the gun and again to lower it. Pausing lowers it.");
      ToggleCvar("kk_toggle_aim", "Hold", "Toggle");
      Row("Stick deadzone", "Ignores small stick movements. Raise it if the camera or cursor drifts.");
      SliderCvar("kk_deadzone", 0, 40, "%d%%");
      Row("Vibration", "Controller rumble.");
      ToggleCvar("kk_vibration");
      if (GetBool("kk_vibration")) {
        Row("Vibration strength", "");
        SliderCvar("kk_vibration_strength", 10, 100, "%d%%");
      }
      Row("Left stick", "Invert the left stick's axes.");
      {
        const bool x = GetBool("kk_invert_ls_x"), y = GetBool("kk_invert_ls_y");
        const int sel = x && y ? 3 : x ? 1 : y ? 2 : 0;
        if (int i = Segmented("ls", {"Normal", "Invert X", "Invert Y", "Both"}, sel); i >= 0) {
          SetBool("kk_invert_ls_x", i == 1 || i == 3);
          SetBool("kk_invert_ls_y", i == 2 || i == 3);
        }
      }
      EndRows();
    }
    ImGui::Dummy(ImVec2(0, 6 * s_));
    if (ImGui::CollapsingHeader("Button remapping")) DrawRemapTable();
    if (ImGui::CollapsingHeader("Keyboard bindings")) DrawKeyboardTable();
  }

  void DrawRemapTable() {
    if (ImGui::Button("Reset to defaults##remap"))
      for (size_t i = 0; i < kPadCount; ++i) SetMapping(static_cast<Pad>(i), static_cast<Pad>(i));
    if (!BeginRows("##remap")) return;
    for (size_t i = 0; i < kPadCount; ++i) {
      const auto physical = static_cast<Pad>(i);
      const Pad target = GetMapping(physical);
      Row(GetPadInfo(physical).label, nullptr);
      ImGui::PushID(int(i));
      const char* preview = target == Pad::kNone ? "(nothing)" : GetPadInfo(target).label;
      if (ImGui::BeginCombo("##t", preview, ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("(nothing)", target == Pad::kNone)) SetMapping(physical, Pad::kNone);
        for (size_t j = 0; j < kPadCount; ++j)
          if (ImGui::Selectable(GetPadInfo(static_cast<Pad>(j)).label, target == static_cast<Pad>(j)))
            SetMapping(physical, static_cast<Pad>(j));
        ImGui::EndCombo();
      }
      ImGui::PopID();
    }
    EndRows();
  }

  void DrawKeyboardTable() {
    std::vector<const rex::cvar::FlagEntry*> binds;
    for (auto& e : rex::cvar::GetRegistry())
      if (e.category == "Input/Keybinds/Controller") binds.push_back(&e);
    if (ImGui::Button("Reset to defaults##keys"))
      for (auto* e : binds) rex::cvar::ResetToDefault(e->name);
    ImGui::SameLine();
    ImGui::TextDisabled("Click a binding, then press a key (Esc cancels).");
    if (!BeginRows("##keys")) return;
    for (auto* e : binds) {
      Row(e->description.c_str(), nullptr);
      ImGui::PushID(e->name.c_str());
      const bool capturing = capturing_ == e->name;
      const std::string value = e->getter();
      if (capturing ? AccentButton("Press a key...", ImVec2(-FLT_MIN, 0))
                    : ImGui::Button(value.empty() ? "(none)" : value.c_str(), ImVec2(-FLT_MIN, 0)))
        capturing_ = e->name;
      ImGui::PopID();
      if (capturing) CaptureKey(e->name);
    }
    EndRows();
  }

  void CaptureKey(const std::string& cvar) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      capturing_.clear();
      return;
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
      const auto key = static_cast<ImGuiKey>(k);
      if (key == ImGuiKey_Escape || !ImGui::IsKeyPressed(key, false)) continue;
      const int vk = ImGuiKeyToVk(key);
      if (!vk) continue;
      const std::string name = rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(vk));
      if (!name.empty()) rex::cvar::SetFlagByName(cvar, name);
      capturing_.clear();
      return;
    }
  }

  // ------------------------------------------------------- Achievements ---
  // ------------------------------------------------------------- Cheats ---
  void PageCheats() {
    if (BeginRows("##cheats")) {
      Row("Cheats", "Switches on the cheats you pick below as soon as the game reaches its main menu, the same "
                    "as typing their codes on the game's Cheat screen. They last until you quit the game.");
      ToggleCvar("kk_cheats", "Off", "On");
      if (GetBool("kk_cheats")) {
        for (const auto& cheat : kCheats) {
          const std::string cvar = std::string("kk_cheat_") + cheat.id;
          const std::string desc = std::string("Code: ") + cheat.code;
          Row(cheat.label, desc.c_str());
          ToggleCvar(cvar.c_str(), "Off", "On");
        }
      }
      EndRows();
    }
  }

  void PageAchievements() {
    if (BeginRows("##ach_settings")) {
      Row("Notifications", "An Xbox 360-style pop-up when you unlock an achievement in game.");
      ToggleCvar("kk_achievement_toasts", "Off", "On");
      Row("Sound", "The sound that plays with each pop-up.");
      ToggleCvar("kk_achievement_sound", "Off", "On");
      if (GetBool("kk_achievement_sound")) {
        Row("Sound to play",
            "Pick the built-in chime or a sound you added. Put .wav files in the sounds folder to see them here.");
        SoundCombo();
        if (ImGui::Button("Open sounds folder", ImVec2(-FLT_MIN, 0))) {
          OpenInExplorer(SoundsDir(paths_.user_dir));
          sounds_.clear();  // rescan when the list is next drawn
        }
        Row("Volume", "");
        int v = GetInt("kk_achievement_volume", 80);
        if (ImGui::SliderInt("##vol", &v, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp))
          SetInt("kk_achievement_volume", v);
        if (ImGui::IsItemDeactivatedAfterEdit()) PlayAchievementSound(paths_.user_dir);
      }
      Row("Test", "Shows a sample notification right now.");
      if (AccentButton("Test notification", ImVec2(-FLT_MIN, 0))) {
        if (!GetBool("kk_achievement_toasts")) status_ = "Notifications are off; switch them on to see the test.";
        const uint32_t icon = achievements_.empty() ? 0 : achievements_[test_index_++ % achievements_.size()].id;
        toast_->Show("Test achievement", 10, icon);
      }
      EndRows();
    }
    ImGui::Dummy(ImVec2(0, 10 * s_));

    ImGui::PushFont(GetUiFonts().semibold, 0.0f);
    ImGui::TextUnformatted("PC port");
    ImGui::PopFont();
    for (const auto& pa : PortAchievements()) {
      Achievement card;
      card.label = pa.title;
      card.description = pa.description;
      card.unlocked = IsPortAchievementUnlocked(paths_.user_dir, pa.id);
      card.icon = title_icon_;
      DrawAchievementCard(card, ImGui::GetContentRegionAvail().x);
    }
    ImGui::Dummy(ImVec2(0, 10 * s_));

    if (achievements_.empty()) {
      ImGui::TextDisabled("Install the game to see its achievements.");
      return;
    }
    uint32_t unlocked = 0, score = 0, total_score = 0;
    for (auto& a : achievements_) {
      total_score += a.gamerscore;
      if (a.unlocked) {
        ++unlocked;
        score += a.gamerscore;
      }
    }
    ImGui::PushFont(GetUiFonts().semibold, 0.0f);
    if (have_achievement_names_)
      ImGui::Text("%u / %zu unlocked     %u / %u G", unlocked, achievements_.size(), score, total_score);
    else
      ImGui::Text("%u / %zu unlocked", unlocked, achievements_.size());
    ImGui::PopFont();
    if (!have_achievement_names_)
      ImGui::TextDisabled("Names and descriptions appear after you have played the game once.");
    ImGui::Dummy(ImVec2(0, 4 * s_));

    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = avail > 700 * s_ ? 2 : 1;
    const float gap = 12 * s_;
    const float card_w = (avail - gap * float(cols - 1)) / float(cols);
    for (size_t i = 0; i < achievements_.size(); ++i) {
      if (i % cols) ImGui::SameLine(0, gap);
      DrawAchievementCard(achievements_[i], card_w);
    }
  }

  void SoundCombo() {
    if (sounds_.empty() || ImGui::GetTime() - sounds_scanned_ > 3.0) {
      sounds_ = ListSounds(paths_.user_dir);
      sounds_.insert(sounds_.begin(), std::filesystem::path());  // built-in chime
      sounds_scanned_ = ImGui::GetTime();
    }
    const std::string cur = Get("kk_achievement_sound_file");
    auto label = [](const std::filesystem::path& p) { return p.empty() ? std::string("Original chime (built in)") : SoundLabel(p); };
    std::string preview = "Original chime (built in)";
    for (auto& p : sounds_)
      if (p.string() == cur) preview = label(p);
    if (ImGui::BeginCombo("##sound", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
      for (auto& p : sounds_) {
        if (ImGui::Selectable(label(p).c_str(), p.string() == cur)) {
          Set("kk_achievement_sound_file", p.string());
          PlayAchievementSound(paths_.user_dir);  // preview
        }
      }
      ImGui::EndCombo();
    }
  }

  void DrawAchievementCard(const Achievement& a, float card_w) {
    const float card_h = 86 * s_, icon = 60 * s_;
    const UiFonts& f = GetUiFonts();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(card_w, card_h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + card_w, p.y + card_h), Col(a.unlocked ? kFrameHot : kFrame), 10 * s_);
    if (a.unlocked)
      dl->AddRectFilled(p, ImVec2(p.x + 4 * s_, p.y + card_h), Col(kAccent), 10 * s_, ImDrawFlags_RoundCornersLeft);
    const ImVec2 i0(p.x + 14 * s_, p.y + (card_h - icon) * 0.5f);
    if (a.icon)
      dl->AddImageRounded(Tex(a.icon), i0, ImVec2(i0.x + icon, i0.y + icon), ImVec2(0, 0), ImVec2(1, 1),
                          a.unlocked ? IM_COL32_WHITE : IM_COL32(105, 110, 125, 200), 8 * s_);
    const float tx = i0.x + icon + 14 * s_;
    const float right = p.x + card_w - 14 * s_;
    const std::string title = a.label.empty() ? "Achievement " + std::to_string(a.id) : a.label;
    dl->AddText(f.semibold, 18 * s_, ImVec2(tx, p.y + 12 * s_), a.unlocked ? IM_COL32_WHITE : Col(kDim), title.c_str());
    if (a.gamerscore) {
      const std::string g = std::to_string(a.gamerscore) + " G";
      const ImVec2 gs = f.semibold->CalcTextSizeA(16 * s_, FLT_MAX, 0, g.c_str());
      dl->AddText(f.semibold, 16 * s_, ImVec2(right - gs.x, p.y + 13 * s_), Col(a.unlocked ? kAccent : kDim), g.c_str());
    }
    const std::string& desc = a.unlocked || a.unachieved.empty() ? a.description : a.unachieved;
    dl->AddText(f.regular, 15 * s_, ImVec2(tx, p.y + 38 * s_), Col(kDim), desc.c_str(), nullptr, right - tx);
    if (a.unlocked) dl->AddText(f.semibold, 13 * s_, ImVec2(tx, p.y + card_h - 22 * s_), Col(kAccent), "UNLOCKED");
  }

  // -------------------------------------------------------------- About ---
  void PageAbout() {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(
        "Peter Jackson's King Kong: The Official Game of the Movie (Ubisoft, 2005) running natively on PC: the "
        "original Xbox 360 game code statically recompiled to C++ with the ReXGlue SDK. No game code or assets are "
        "included; the game runs from your own disc image.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4 * s_));
    if (!BeginRows("##about")) return;
    Row("Save data", "Your saves, achievements and caches.");
    if (ImGui::Button("Open save folder", ImVec2(-FLT_MIN, 0))) OpenInExplorer(paths_.user_dir);
    Row("Game files", "Where the game is installed.");
    if (ImGui::Button("Open game folder", ImVec2(-FLT_MIN, 0))) OpenInExplorer(paths_.game_dir);
    Row("Settings file", "Every launcher setting, as plain text.");
    if (ImGui::Button("Open settings file", ImVec2(-FLT_MIN, 0))) {
      SaveSettings(paths_.config_path);
      OpenInExplorer(paths_.config_path);
    }
    Row("Launcher art", "The header shows the game's own menu screen, captured the first time you reach it.");
    if (ImGui::Button(title_art_ ? "Capture again next time I play" : "Captured on your first play", ImVec2(-FLT_MIN, 0))) {
      std::error_code ec;
      std::filesystem::remove(art::TitleCapturePath(paths_.user_dir), ec);
      status_ = "The menu screen will be captured again next time you play.";
    }
    Row("Reset settings", "Put every setting back to its default.");
    if (ImGui::Button("Reset all settings", ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup("Reset all settings?");
    if (ImGui::BeginPopupModal("Reset all settings?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Dummy(ImVec2(320 * s_, 4 * s_));
      ImGui::SetCursorPosX(16 * s_);
      ImGui::TextUnformatted("Every setting goes back to its default.");
      ImGui::Dummy(ImVec2(0, 4 * s_));
      ImGui::SetCursorPosX(16 * s_);
      if (AccentButton("Reset", ImVec2(140 * s_, 0))) {
        ResetAllSettings();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(140 * s_, 0))) ImGui::CloseCurrentPopup();
      ImGui::Dummy(ImVec2(0, 8 * s_));
      ImGui::EndPopup();
    }
    Row("Updates", "Checks GitHub for a newer version of the port, and of the shader pack, each time the launcher "
                   "opens. They come out separately, and you're offered each one.");
    ToggleCvar("kk_check_updates", "Off", "At startup");
    Row("", "");
    if (ImGui::Button(update_ && update_->busy ? "Checking..." : "Check for updates now", ImVec2(-FLT_MIN, 0)))
      StartUpdateCheck(true);
    Row("Version", "Port v" KK_VERSION "   \xC2\xB7   ReXGlue SDK 0.10.0   \xC2\xB7   title 555307D3, v0.0.0.1");
    ImGui::TextDisabled("github.com/TekRantGaming/king-kong-recompiled");
    EndRows();
    // Every version's notes, newest first.
    ImGui::Dummy(ImVec2(0, 10 * s_));
    ImGui::PushFont(GetUiFonts().semibold, 20.0f);
    ImGui::TextUnformatted("Changelog");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted("What each version of the port added, newest first.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4 * s_));
    ImGui::BeginChild("##changelog", ImVec2(0, 380 * s_), ImGuiChildFlags_Borders);
    bool first = true;
    for (const auto& e : Changelog()) {
      if (!first) {
        ImGui::Dummy(ImVec2(0, 6 * s_));
        ImGui::Separator();
      }
      DrawChangelogEntry(e);
      first = false;
    }
    ImGui::EndChild();
  }

  // The settings each page shows, for its "Reset page" button. "x_*" is a
  // prefix, "@category" a cvar category.
  static std::vector<std::string> PageSettings(Page page) {
    switch (page) {
      case kDisplay:
        return {"fullscreen", "window_width", "window_height", "monitor",
                "d3d12_allow_variable_refresh_rate_and_tearing", "present_letterbox"};
      case kGraphics:
        return {"kk_render_quality", "resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y",
                "swap_post_effect", "anisotropic_override", "kk_motion_blur", "async_shader_compilation"};
      case kGameplay:
        return {"kk_frame_rate", "kk_fov", "kk_show_fps", "kk_skip_intros", "user_language"};
      case kControls:
        return {"mnk_mode", "kk_button_prompts", "kk_camera_sensitivity", "kk_camera_modern", "mnk_sensitivity", "mnk_mouse",
                "kk_invert_rs_x", "kk_invert_rs_y", "kk_invert_ls_x", "kk_invert_ls_y", "kk_toggle_aim",
                "kk_deadzone", "kk_vibration", "kk_vibration_strength", "kk_map_*", "@Input/Keybinds/Controller"};
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
    if (page == kDisplay && cb_.set_fullscreen) cb_.set_fullscreen(GetBool("fullscreen"));
    status_ = std::string(kPageNames[page]) + " settings reset to defaults. Press Save to keep them.";
  }

  void ResetAllSettings() {
    for (auto& e : rex::cvar::GetRegistry()) {
      if (e.type == rex::cvar::FlagType::Command) continue;
      if (e.source == rex::cvar::Source::kCommandLine || e.source == rex::cvar::Source::kEnvironment) continue;
      rex::cvar::ResetToDefault(e.name);
    }
    if (cb_.set_fullscreen) cb_.set_fullscreen(GetBool("fullscreen"));
    status_ = "Settings reset to defaults.";
  }

  // ------------------------------------------------------------- footer ---
  void DrawFooter(ImVec2 origin, float w, float h) {
    const float bw = 130 * s_, play_w = 210 * s_;
    const float bh = 48 * s_;
    const float cy = origin.y + h * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const UiFonts& f = GetUiFonts();
    dl->AddText(f.regular, 15 * s_, ImVec2(origin.x, cy - 20 * s_), Col(kDim),
                "Enter  Play        Esc  Quit        Ctrl+S  Save");
    dl->AddText(f.regular, 15 * s_, ImVec2(origin.x, cy + 2 * s_), Col(kDim),
                status_.empty() ? "Settings are saved when you press Play." : status_.c_str());

    const float right = origin.x + w;
    ImGui::SetCursorScreenPos(ImVec2(right - play_w - bw * 2 - 24 * s_, cy - bh * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12 * s_);
    if (ImGui::Button("Quit", ImVec2(bw, bh)) && cb_.quit) cb_.quit();
    ImGui::SameLine(0, 12 * s_);
    if (ImGui::Button("Save", ImVec2(bw, bh))) Save();
    ImGui::SameLine(0, 12 * s_);
    ImGui::PopStyleVar();
    DrawPlayButton(ImVec2(play_w, bh));
  }

  void DrawPlayButton(ImVec2 size) {
    const bool can_play = files_ok_ && !installing_;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!can_play);
    const bool clicked = ImGui::InvisibleButton("##play", size);
    ImGui::EndDisabled();
    const bool hot = can_play && ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (can_play)
      dl->AddRectFilled(ImVec2(p.x - 3 * s_, p.y - 3 * s_), ImVec2(p.x + size.x + 3 * s_, p.y + size.y + 3 * s_),
                        Col(kAccent, hot ? 0.35f : 0.18f), size.y * 0.5f + 3 * s_);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), can_play ? Col(hot ? kAccentHot : kAccent) : Col(kFrame),
                      size.y * 0.5f);
    const UiFonts& f = GetUiFonts();
    const float fs = 22 * s_;
    const ImVec2 ts = f.bold->CalcTextSizeA(fs, FLT_MAX, 0, "PLAY");
    const float tri = fs * 0.5f;
    const float total = ts.x + 12 * s_ + tri;
    const float tx = p.x + (size.x - total) * 0.5f, ty = p.y + (size.y - ts.y) * 0.5f;
    const ImU32 ink = can_play ? Col(kOnAccent) : Col(kDim);
    dl->AddText(f.bold, fs, ImVec2(tx, ty), ink, "PLAY");
    const float ax = tx + ts.x + 12 * s_, ay = p.y + size.y * 0.5f;
    dl->AddTriangleFilled(ImVec2(ax, ay - tri * 0.6f), ImVec2(ax, ay + tri * 0.6f), ImVec2(ax + tri, ay), ink);
    if (clicked) Play();
  }

  void HandleHotkeys() {
    if (!capturing_.empty() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || ImGui::GetIO().WantTextInput)
      return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      if (files_ok_ && !installing_) Play();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      if (cb_.quit) cb_.quit();
    } else if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
      Save();
    }
  }

  void Save() { status_ = SaveSettings(paths_.config_path) ? "Settings saved." : "Could not save settings."; }

  // Above 30 FPS some animations are wrong (game logic stepped per frame), so
  // say so before starting and offer 30 or 60 instead.
  void Play() {
    if (played_) return;
    const int fps = GetInt("kk_frame_rate", 30);
    if (fps > 0 && fps <= 30) return StartGame();
    open_fps_warning_ = true;
  }

  // A pop-up in the launcher's own colours (the default popup style is for
  // dropdowns), centred, `width` wide. Call ImGui::EndPopup() when it returns true.
  bool BeginModal(const char* title, float width, bool title_bar = true) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(width * s_, vp->Size.x - 40 * s_), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s_, 20 * s_));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kFrame);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kFrame);
    ImGui::PushStyleColor(ImGuiCol_Border, kWarn);
    const bool open = ImGui::BeginPopupModal(
        title, nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | (title_bar ? 0 : ImGuiWindowFlags_NoTitleBar));
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
    return open;
  }

  // ------------------------------------------------------------- updates ---
  // The check runs on its own thread with its own status, so pressing Play
  // never waits for the network (the launcher may be gone when it finishes).
  // Checks for a newer port (DrawUpdatePrompt) and, separately, a newer shader
  // pack (DrawPackPrompt): they are released independently.
  void StartUpdateCheck(bool manual) {
    if (update_ && update_->busy) return;
    update_ = std::make_shared<UpdateStatus>();
    update_manual_ = manual;
    update_prompted_ = false;
    std::thread([s = update_] { CheckForUpdate(*s); }).detach();
    pack_published_ = std::make_shared<std::atomic<int>>(-1);  // checking
    pack_prompted_ = false;
    std::thread([v = pack_published_] { v->store(FetchShaderPackVersion()); }).detach();
  }

  void DrawUpdatePrompt() {
    constexpr const char* kTitle = "Update available";
    if (!update_) return;
    if (update_manual_ && !update_->busy && (update_->done || update_->failed) && !update_->found &&
        pack_published_->load() != -1) {
      if (update_->failed) {
        status_ = update_->message;
      } else {
        if (pack_installed_ < 0) pack_installed_ = InstalledShaderPackVersion(CacheDir());
        const int pack = pack_published_->load();
        status_ = "You have the newest version (v" KK_VERSION ")";
        if (pack > 0 && pack_installed_ >= pack) status_ += " and the newest shader pack (" + std::to_string(pack) + ")";
        status_ += ".";
      }
      update_manual_ = false;
    }
    if (update_->found && !update_prompted_ && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
      ImGui::OpenPopup(kTitle);
      update_prompted_ = true;
    }
    if (!BeginModal(kTitle, 560)) return;
    const UpdateInfo& info = *update_->found;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::Text("Version %s of the King Kong PC port is out. You have version %s.", info.version.c_str(), KK_VERSION);
    ImGui::Dummy(ImVec2(0, 6 * s_));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted("Updating replaces the port's own files and restarts the launcher. Your installed game, "
                           "saves and settings stay as they are.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 10 * s_));
    if (installing_update_) {
      const float total = float(update_->total.load()), got = float(update_->bytes.load());
      if (update_->busy) {
        ImGui::ProgressBar(total > 0 ? got / total : 0.0f, ImVec2(-FLT_MIN, 0),
                           total > 0 ? nullptr : "Downloading...");
      } else if (update_->failed) {
        ImGui::TextWrapped("%s", update_->message.c_str());
        if (ImGui::Button("Close", ImVec2(-FLT_MIN, 0))) {
          installing_update_ = false;
          ImGui::CloseCurrentPopup();
        }
      } else if (update_->done) {
        ImGui::TextUnformatted(update_->message.c_str());
        if (!relaunched_) {  // start the new version, then close this one
          relaunched_ = true;
          RelaunchSelf(L"");
          if (cb_.quit) cb_.quit();
        }
      }
      ImGui::EndPopup();
      return;
    }
    const float bw = (ImGui::GetContentRegionAvail().x - 16 * s_) / 3;
    if (AccentButton("Update now", ImVec2(bw, 0))) {
      installing_update_ = true;
      update_->done = update_->failed = false;
      std::thread([s = update_, info] { InstallUpdate(info, *s); }).detach();
    }
    ImGui::SameLine(0, 8 * s_);
    if (ImGui::Button("What's new", ImVec2(bw, 0))) OpenUrl(info.page_url);
    ImGui::SameLine(0, 8 * s_);
    if (ImGui::Button("Later", ImVec2(bw, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // ----------------------------------------------------------- changelog ---
  // Draws a version's notes: "### Heading", "- bullet" (two spaces more per
  // level), paragraphs. Inline **bold**, `code` and [text](link) show as text.
  static std::string PlainText(std::string s) {
    for (const char* mark : {"**", "`"})
      for (size_t at; (at = s.find(mark)) != std::string::npos;) s.erase(at, std::strlen(mark));
    for (size_t open; (open = s.find('[')) != std::string::npos;) {
      const size_t mid = s.find("](", open), close = mid == std::string::npos ? mid : s.find(')', mid);
      if (close == std::string::npos) break;
      s = s.substr(0, open) + s.substr(open + 1, mid - open - 1) + s.substr(close + 1);
    }
    return s;
  }

  void DrawMarkdown(const std::string& body) {
    std::stringstream in(body);
    for (std::string line; std::getline(in, line);) {
      if (line.empty()) {
        ImGui::Dummy(ImVec2(0, 4 * s_));
      } else if (line.rfind("### ", 0) == 0) {
        ImGui::Dummy(ImVec2(0, 4 * s_));
        ImGui::PushFont(GetUiFonts().semibold, 17.0f);
        ImGui::TextUnformatted(PlainText(line.substr(4)).c_str());
        ImGui::PopFont();
      } else {
        size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;
        const bool bullet = line.compare(indent, 2, "- ") == 0;
        const std::string text = PlainText(line.substr(indent + (bullet ? 2 : 0)));
        const float x = (indent / 2) * 18 * s_;
        if (x > 0) ImGui::Indent(x);
        if (bullet) {
          ImGui::Bullet();
          ImGui::SameLine();
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        if (x > 0) ImGui::Unindent(x);
      }
    }
  }

  void DrawChangelogEntry(const ChangelogEntry& e) {
    ImGui::PushFont(GetUiFonts().semibold, 20.0f);
    ImGui::Text("v%s", e.version.c_str());
    ImGui::PopFont();
    if (!e.date.empty()) {
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, kDim);
      ImGui::TextUnformatted(e.date.c_str());
      ImGui::PopStyleColor();
    }
    DrawMarkdown(e.body);
  }

  // Shown once after an update: the notes of every version since the last one run.
  void DrawWhatsNew() {
    constexpr const char* kTitle = "What's new";
    if (open_whats_new_ && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
      ImGui::OpenPopup(kTitle);
      open_whats_new_ = false;
    }
    if (!BeginModal(kTitle, 760)) return;
    ImGui::PushFont(GetUiFonts().semibold, 24.0f);
    ImGui::TextUnformatted("What's new in v" KK_VERSION);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 6 * s_));
    const float h = std::min(460 * s_, ImGui::GetMainViewport()->Size.y * 0.6f);
    ImGui::BeginChild("##whats_new", ImVec2(0, h), ImGuiChildFlags_Borders);
    bool any = false;
    for (const auto& e : Changelog()) {
      if (CompareVersions(e.version, KK_VERSION) > 0) continue;
      if (!whats_new_from_.empty() && CompareVersions(e.version, whats_new_from_) <= 0) break;
      if (any) ImGui::Separator();
      DrawChangelogEntry(e);
      any = true;
      if (whats_new_from_.empty()) break;  // only this version when we don't know the last one
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 8 * s_));
    const float half = (ImGui::GetContentRegionAvail().x - 8 * s_) / 2;
    if (AccentButton("Close", ImVec2(half, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine(0, 8 * s_);
    if (ImGui::Button("Every version", ImVec2(half, 0))) {
      page_ = kAbout;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

#if defined(KK_DEV_TOOLS)
  // Developer-only (README screenshots): KK_DEV_LAUNCHER_TOUR=<seconds> shows
  // each page in turn for that long, then the What's new pop-up, logging
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
    static const std::pair<Page, const char*> kSteps[] = {
        {kPlay, "play"},         {kDisplay, "display"}, {kGraphics, "graphics"},         {kGameplay, "gameplay"},
        {kControls, "controls"}, {kCheatsPage, "cheats"}, {kAchievements, "achievements"}, {kAbout, "about"}};
    constexpr int kCount = int(sizeof(kSteps) / sizeof(kSteps[0]));
    if (step < kCount) {
      page_ = kSteps[step].first;
      REXLOG_INFO("KK dev: tour {}", kSteps[step].second);
    } else if (step == kCount) {
      page_ = kPlay;
      whats_new_from_ = "";  // this version's notes, as after an update from 1.6.0
      open_whats_new_ = true;
      REXLOG_INFO("KK dev: tour whats_new");
    } else if (step == kCount + 1) {
      REXLOG_INFO("KK dev: tour done");
    }
  }
#endif

  // --------------------------------------------------- shaders poster ---
  // Shown each time the launcher opens until "Don't show this message again".
  void DrawSharePoster() {
    constexpr const char* kTitle = "##share_poster";
    if (open_poster_ && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
      ImGui::OpenPopup(kTitle);
      open_poster_ = false;
    }
    if (!BeginModal(kTitle, 1040, false)) return;
    const UiFonts& f = GetUiFonts();
    const float img_h = std::min(560 * s_, ImGui::GetMainViewport()->Size.y - 140 * s_);
    if (poster_) {
      ImGui::Image(Tex(poster_), ImVec2(img_h * poster_aspect_, img_h));
      ImGui::SameLine(0, 24 * s_);
    }
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushFont(f.bold, 26.0f * s_);
    ImGui::TextUnformatted("Help your fellow players");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 8 * s_));
    ImGui::TextUnformatted(
        "King Kong prepares each of its effects (a shader) the first time it appears, and that can cause a short "
        "pause. The shader pack removes those pauses, but only for the parts of the game it has already seen.");
    ImGui::Dummy(ImVec2(0, 6 * s_));
    ImGui::TextUnformatted(
        "That's where you come in. Play the game, and once you've played a good part of it, share your shaders. "
        "They go into future shader packs and releases, so everyone who plays after you gets a smoother first "
        "play-through.");
    ImGui::Dummy(ImVec2(0, 10 * s_));
    ImGui::PushFont(f.semibold, 0.0f);
    ImGui::TextUnformatted("Where to find it");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(
        "On the Play page, under Share my shaders. It packs your shaders into one small file and opens a GitHub page "
        "where you drop it in. Only shader data is shared: nothing personal, no saves or settings. Posting needs a "
        "free GitHub account.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 14 * s_));
    ImGui::Checkbox("Don't show this message again", &poster_dont_show_);
    ImGui::Dummy(ImVec2(0, 10 * s_));
    const float bw = 180 * s_;
    bool close = false;
    if (AccentButton("Got it", ImVec2(bw, 0))) close = true;
    ImGui::SameLine(0, 8 * s_);
    if (ImGui::Button("Show me", ImVec2(bw, 0))) {
      page_ = kPlay;
      close = true;
    }
    ImGui::EndGroup();
    if (close) {
      if (poster_dont_show_) {
        SetBool("kk_share_poster", false);
        Save();
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  void DrawFrameRateWarning() {
    constexpr const char* kTitle = "Frame rate above 30 FPS";
    if (open_fps_warning_) {
      ImGui::OpenPopup(kTitle);
      open_fps_warning_ = false;
    }
    if (!BeginModal(kTitle, 600)) return;
    const int fps = GetInt("kk_frame_rate", 30);
    const std::string current = fps <= 0 ? "an unlimited frame rate" : std::to_string(fps) + " FPS";
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(("You have chosen " + current + ". King Kong was made to run at 30 FPS, and above that some "
                            "character animations can look wrong, such as the crew rowing the boat at the start.")
                               .c_str());
    ImGui::Dummy(ImVec2(0, 6 * s_));
    ImGui::TextUnformatted("We recommend 30 FPS. 60 FPS also works well: the issues are still there, but much less "
                           "noticeable. At an unlimited frame rate you may see some strange animations.");
    ImGui::Dummy(ImVec2(0, 6 * s_));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted("This will be fixed in a future update.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 10 * s_));
    const float bw = (ImGui::GetContentRegionAvail().x - 16 * s_) / 3;
    auto play_at = [&](int rate) {
      SetInt("kk_frame_rate", rate);
      ImGui::CloseCurrentPopup();
      StartGame();
    };
    if (AccentButton("Play at 30 FPS", ImVec2(bw, 0))) play_at(30);
    ImGui::SameLine(0, 8 * s_);
    if (fps != 60) {
      if (ImGui::Button("Play at 60 FPS", ImVec2(bw, 0))) play_at(60);
    } else if (ImGui::Button("Keep 60 FPS", ImVec2(bw, 0))) {
      play_at(60);
    }
    ImGui::SameLine(0, 8 * s_);
    if (fps != 60) {
      const std::string keep = fps <= 0 ? "Keep unlimited" : "Keep " + std::to_string(fps) + " FPS";
      if (ImGui::Button(keep.c_str(), ImVec2(bw, 0))) play_at(fps);
    } else if (ImGui::Button("Back", ImVec2(bw, 0))) {
      ImGui::CloseCurrentPopup();
    }
    if (fps != 60) {
      ImGui::Dummy(ImVec2(0, 2 * s_));
      if (ImGui::Button("Back", ImVec2(-FLT_MIN, 0))) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  void StartGame() {
    if (played_) return;
    played_ = true;
    SaveSettings(paths_.config_path);
    bool needs_restart = false;
    for (size_t i = 0; i < std::size(kRestartCvars); ++i)
      if (Get(kRestartCvars[i]) != restart_baseline_[i]) needs_restart = true;
    auto action = needs_restart ? cb_.restart_and_play : cb_.play;
    Close();  // deletes this dialog after the current draw
    if (action) action();
  }

  rex::ui::ImmediateDrawer* immediate_;
  LauncherPaths paths_;
  LauncherCallbacks cb_;
  ImGuiStyle saved_style_;
  float s_ = 1.0f;
  Page page_ = kPlay;
  std::vector<std::string> restart_baseline_;
  bool files_ok_ = false;
  bool played_ = false;
  bool open_fps_warning_ = false;
  std::shared_ptr<UpdateStatus> update_;
  bool update_manual_ = false, update_prompted_ = false, installing_update_ = false, relaunched_ = false;
  bool open_poster_ = false, poster_dont_show_ = false;
  bool open_whats_new_ = false;
  std::string whats_new_from_;  // version the player had before this one
  rex::ui::ImmediateTexture* poster_ = nullptr;
  float poster_aspect_ = 645.0f / 900.0f;
  std::string share_message_;
  ShaderPackStatus pack_;
  // Published shader pack version, fetched in the background (0 until known).
  std::shared_ptr<std::atomic<int>> pack_published_ = std::make_shared<std::atomic<int>>(0);  // -1 while checking
  bool pack_prompted_ = false;
  std::thread pack_thread_;
  int pack_installed_ = -1;  // -1: not read yet
  int autoplay_frames_ = 0;
  std::string status_;
  std::string capturing_;
  std::vector<MonitorInfo> monitors_;

  std::vector<std::unique_ptr<rex::ui::ImmediateTexture>> textures_;
  rex::ui::ImmediateTexture* title_art_ = nullptr;
  float title_art_aspect_ = 16.0f / 9.0f;
  rex::ui::ImmediateTexture* title_icon_ = nullptr;
  std::vector<Achievement> achievements_;
  bool have_achievement_names_ = false;
  std::unique_ptr<AchievementToast> toast_;
  size_t test_index_ = 0;
  std::vector<std::filesystem::path> sounds_;
  double sounds_scanned_ = -10.0;

  iso::Progress progress_;
  std::thread install_thread_;
  std::atomic<bool> install_done_{false};
  bool installing_ = false;
  std::string install_result_;
  std::string install_message_;
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
