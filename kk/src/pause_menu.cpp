#include "pause_menu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include "ingame_settings.h"
#include "launcher_art.h"
#include "launcher_sounds.h"
#include "launcher_text.h"
#include "launcher_ui.h"
#include "play_session.h"
#include "toast.h"

namespace kk {
namespace {

using text::F;
using text::T;
using ui::Action;
using ui::Item;
using ui::ItemType;
namespace color = ui::color;

struct FedPad {
  std::mutex lock;
  uint16_t buttons = 0;
  int16_t lx = 0, ly = 0;
  uint8_t lt = 0, rt = 0;
  bool fresh = false;
};
FedPad g_pad;

bool GetBool(const char* name) { return rex::cvar::GetFlagByName(name) == "true"; }
int GetInt(const char* name, int fallback) {
  try {
    return std::stoi(rex::cvar::GetFlagByName(name));
  } catch (...) {
    return fallback;
  }
}

// The pause menu: a column of large entries (the launcher's Home menu) over the dimmed frame, and
// Settings: the launcher's settings pages (tabs, list, description) with the live rows only.
class PauseMenu final : public rex::ui::ImGuiDialog {
 public:
  PauseMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, std::filesystem::path config_path,
            std::filesystem::path user_dir, std::filesystem::path game_dir)
      : ImGuiDialog(drawer),
        immediate_(immediate),
        user_dir_(std::move(user_dir)),
        game_dir_(std::move(game_dir)),
        settings_(CreateInGameSettings(immediate, std::move(config_path), user_dir_)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const bool open = play::PauseMenuOpen();
    if (open != open_) {
      open_ = open;
      if (open) Opened();
      else Closed();
    }
    if (!open_) return;

    ui::Input& in = settings_->input();
    {
      std::lock_guard lock(g_pad.lock);
      if (g_pad.fresh) in.FeedPad(g_pad.buttons, g_pad.lx, g_pad.ly, g_pad.lt, g_pad.rt);
      g_pad.fresh = false;
    }
    in.set_keyboard_enabled(!settings_->Capturing());
    in.Update();
    // The press that opened the menu (Start, Esc) is not a press on it.
    if (ImGui::GetFrameCount() <= opened_frame_ + 1) in.ConsumeAll();
    if (pending_click_ != Action::kCount) {
      in.Inject(pending_click_, ui::Device::kKeyboard);
      pending_click_ = Action::kCount;
    }
#if defined(KK_DEV_TOOLS)
    DevInput();
#endif
    settings_->Tick();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    o_ = vp->Pos;
    w_ = vp->Size.x;
    h_ = vp->Size.y;
    const float s = std::clamp(std::min(w_ / 1280.0f, h_ / 720.0f), 0.7f, 3.0f);
    m_.s = s;
    m_.row_h = 46 * s;
    m_.tall_h = 74 * s;
    m_.section_h = 52 * s;

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##kk_pause_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    DrawShade(dl);
    ui::Modal& modal = settings_->modal();
    if (view_ == View::kList) DrawList(dl);
    else DrawSettings(dl);
    ui::DrawModal(dl, modal, o_, ImVec2(o_.x + w_, o_.y + h_), m_, in);
    DrawFooter(dl);
    ImGui::End();
    (void)io;
  }

 private:
  enum class View { kList, kSettings };
  struct Entry {
    std::string label;
    std::function<void()> action;
  };

  float Margin() const { return 64 * m_.s; }

  void Opened() {
    opened_frame_ = ImGui::GetFrameCount();
    view_ = View::kList;
    focus_ = 0;
    offsets_.clear();
    settings_->modal().open = false;
    settings_->LeaveSubPage();
    settings_->input().ConsumeAll();
    open_time_ = ImGui::GetTime();
    LoadLogo();
    // The launcher's menu sounds, while they're on (the launcher's own handler went with it).
    ui::SetSoundHandler([this](ui::Sound sound, float value) {
      if (!GetBool("kk_launcher_sounds")) return;
      if (!sounds_) sounds_ = std::make_unique<LauncherSounds>(game_dir_);
      const float v = GetInt("kk_launcher_sounds_volume", 50) / 100.0f;
      sounds_->Play(sound, value, v * v);
    });
    REXLOG_INFO("KK: pause menu opened");
  }

  void Closed() {
    settings_->SaveNow();
    settings_->modal().open = false;
    REXLOG_INFO("KK: pause menu closed");
  }

  void LoadLogo() {
    if (logo_tried_ || !immediate_) return;
    logo_tried_ = true;
    const auto dir = art::LauncherArtDir(user_dir_);
    if (!art::HasLauncherArt(dir)) return;
    const art::LauncherArt a = art::LoadLauncherArt(dir);
    if (!a.logo) return;
    logo_ = immediate_->CreateTexture(uint32_t(a.logo.width), uint32_t(a.logo.height),
                                      rex::ui::ImmediateTextureFilter::kLinear, false, a.logo.rgba.data());
    logo_aspect_ = float(a.logo.width) / float(a.logo.height);
  }

  // The frozen frame, dimmed (more behind Settings), darker on the left under the menu.
  void DrawShade(ImDrawList* dl) {
    const float t = std::clamp(float(ImGui::GetTime() - open_time_) / 0.18f, 0.0f, 1.0f);
    shade_ = ui::Approach(shade_ * 100.0f, view_ == View::kSettings ? 100.0f : 0.0f, 12.0f) / 100.0f;
    const ImVec2 p1(o_.x + w_, o_.y + h_);
    dl->AddRectFilled(o_, p1, ui::WithAlpha(color::kBase, t * (0.55f + 0.38f * shade_)));
    const ImU32 c0 = ui::WithAlpha(color::kBase, t * 0.75f), c1 = ui::WithAlpha(color::kBase, 0.0f);
    dl->AddRectFilledMultiColor(o_, ImVec2(o_.x + w_ * 0.62f, p1.y), c0, c1, c1, c0);
    const ImU32 b0 = ui::WithAlpha(color::kBase, t * 0.7f);
    dl->AddRectFilledMultiColor(ImVec2(o_.x, p1.y - 160 * m_.s), p1, c1, c1, b0, b0);
    dl->AddRectFilledMultiColor(o_, ImVec2(p1.x, o_.y + 160 * m_.s), b0, b0, c1, c1);
  }

  std::vector<Entry> Entries() {
    std::vector<Entry> e;
    e.push_back({"Resume", [] { play::RunPauseAction(play::PauseAction::kResume); }});
    if (play::PauseActionAvailable(play::PauseAction::kStartAgain))
      e.push_back({"Start again", [this] {
                     Confirm("Start again", "Start this chapter again from its beginning?", "Start again",
                             [] { play::RunPauseAction(play::PauseAction::kStartAgain); });
                   }});
    if (play::PauseActionAvailable(play::PauseAction::kScore))
      e.push_back({"Score", [] { play::RunPauseAction(play::PauseAction::kScore); }});
    // The game's own Options pages (Audio: subtitles and volumes; Video; Controls), drawn by the game.
    if (play::PauseActionAvailable(play::PauseAction::kGameOptions))
      e.push_back({"Options", [] { play::RunPauseAction(play::PauseAction::kGameOptions); }});
    e.push_back({"Settings", [this] { OpenSettings(InGameSettings::kDisplay); }});
    // Story runs: the game's own Save (its save pages show, then this menu comes back).
    if (play::PauseActionAvailable(play::PauseAction::kSave))
      e.push_back({"Save", [] { play::RunPauseAction(play::PauseAction::kSave); }});
    // The launcher is the game's main menu now: two ways out, both through the game's quit (and its saving).
    if (play::PauseActionAvailable(play::PauseAction::kQuitToLauncher))
      e.push_back({"Return to main menu", [this] {
                     Confirm("Return to main menu", "Leave the game and go back to the main menu?", "Leave",
                             [] { play::RunPauseAction(play::PauseAction::kQuitToLauncher); });
                   }});
    if (play::PauseActionAvailable(play::PauseAction::kQuitToDesktop))
      e.push_back({"Quit game", [this] {
                     Confirm("Quit game", "Leave the game and close it?", "Quit",
                             [] { play::RunPauseAction(play::PauseAction::kQuitToDesktop); });
                   }});
    return e;
  }

  void Confirm(std::string title, std::string body, std::string yes, std::function<void()> action) {
    ui::Modal m;
    m.open = true;
    m.title = std::move(title);
    m.body = std::move(body);
    m.buttons = {std::move(yes), "Cancel"};
    m.cancel = 1;
    m.focus = 1;
    m.width = 560;
    m.on_button = [action = std::move(action)](int i) {
      if (i == 0) action();
    };
    settings_->modal() = std::move(m);
  }

  void OpenSettings(InGameSettings::Page page) {
    view_ = View::kSettings;
    page_ = page;
    settings_->LeaveSubPage();
  }

  // ---------------------------------------------------------------- list ---
  void DrawList(ImDrawList* dl) {
    const float s = m_.s;
    const ui::Fonts f = ui::GetFonts();
    ui::Input& in = settings_->input();
    const bool active = !settings_->modal().open;
    std::vector<Entry> entries = Entries();
    const int n = int(entries.size());
    focus_ = std::clamp(focus_, 0, n - 1);
    const int focus_before = focus_;

    const float x = o_.x + Margin();
    const float item_h = 46 * s, fs = 26 * s, tr = 3 * s;
    // The game's logo, the caption and the entries, as one block in the middle of the left side.
    const float lw = logo_ ? std::min(300 * s, w_ * 0.26f) : 0.0f, lh = logo_ ? lw / logo_aspect_ : 0.0f;
    const float logo_gap = logo_ ? 40 * s : 0.0f, caption_h = 44 * s;
    const float block = lh + logo_gap + caption_h + item_h * float(n);
    float y = o_.y + std::max(40 * s, (h_ - block) * 0.5f);
    if (logo_) {
      dl->AddImage(ui::Tex(logo_.get()), ImVec2(x - 8 * s, y), ImVec2(x - 8 * s + lw, y + lh));
      y += lh + logo_gap;
    }
    ui::DrawTracked(dl, f.display, 15 * s, ImVec2(x + 2 * s, y), color::kAccent, text::Upper(T("Paused")), 5 * s);
    dl->AddRectFilled(ImVec2(x + 2 * s, y + 24 * s), ImVec2(x + 42 * s, y + 26 * s), ui::WithAlpha(color::kAccent, 0.6f));
    y += caption_h;

    if (active && n > 1) {
      if (in.Pressed(Action::kUp)) focus_ = (focus_ + n - 1) % n;
      if (in.Pressed(Action::kDown)) focus_ = (focus_ + 1) % n;
    }
    offsets_.resize(size_t(n), 0.0f);
    int chosen = -1;
    for (int i = 0; i < n; ++i) {
      const std::string label = text::Upper(T(entries[size_t(i)].label));
      const float iy = y + item_h * float(i);
      const float tw = ui::TrackedWidth(f.display, fs, label, tr);
      const ImVec2 r0(x - 12 * s, iy), r1(x + 40 * s + tw, iy + item_h);
      if (active && ImGui::IsMouseHoveringRect(r0, r1, false)) {
        if (in.mouse_moved()) focus_ = i;
        if (ImGui::IsMouseClicked(0)) focus_ = chosen = i;
      }
      const bool on = i == focus_;
      offsets_[size_t(i)] = ui::Approach(offsets_[size_t(i)], on ? 22 * s : 0.0f, 18.0f);
      const float off = offsets_[size_t(i)], k = off / (22 * s);
      if (k > 0.02f)
        dl->AddRectFilled(ImVec2(x, iy + item_h * 0.5f - 12 * s), ImVec2(x + 3 * s, iy + item_h * 0.5f + 12 * s),
                          ui::WithAlpha(color::kAccent, k));
      ui::DrawTracked(dl, f.display, fs, ImVec2(x + off, iy + (item_h - fs) * 0.5f), on ? color::kText : color::kTextDim,
                      label, tr);
    }
    if (focus_ != focus_before) ui::Cue(ui::Sound::kMove);
    if (active && in.Pressed(Action::kAccept)) chosen = focus_;
    if (active && (in.Pressed(Action::kBack) || in.Pressed(Action::kPlay))) {
      in.ConsumeAll();
      ui::Cue(ui::Sound::kBack);
      play::RunPauseAction(play::PauseAction::kResume);
      return;
    }
    if (chosen >= 0) {
      in.ConsumeAll();
      ui::Cue(ui::Sound::kSelect);
      entries[size_t(chosen)].action();
    }
  }

  // ------------------------------------------------------------ settings ---
  void DrawSettings(ImDrawList* dl) {
    const float s = m_.s;
    ui::Input& in = settings_->input();
    const bool active = !settings_->modal().open;
    if (active && (in.Pressed(Action::kPrevTab) || in.Pressed(Action::kNextTab))) {
      const int dir = in.Pressed(Action::kNextTab) ? 1 : -1;
      page_ = InGameSettings::Page((int(page_) + dir + InGameSettings::kPageCount) % InGameSettings::kPageCount);
      settings_->LeaveSubPage();
      ui::Cue(ui::Sound::kTab);
      in.Consume(Action::kPrevTab);
      in.Consume(Action::kNextTab);
    }
    // The tabs, as on the launcher's settings pages, and the menu's name at the right.
    std::vector<std::string> names;
    for (int p = 0; p < InGameSettings::kPageCount; ++p)
      names.push_back(text::Upper(T(InGameSettings::PageName(InGameSettings::Page(p)))));
    float ux = 0, uw = 0;
    const int clicked = ui::DrawTabs(dl, settings_->glyphs(), settings_->PromptSet(), names, int(page_),
                                     ImVec2(o_.x + Margin(), o_.y + 44 * s), m_, active, &ux, &uw);
    if (clicked >= 0 && clicked != int(page_)) {
      ui::Cue(ui::Sound::kTab);
      page_ = InGameSettings::Page(clicked);
      settings_->LeaveSubPage();
    }
    if (tab_x_ < 0) tab_x_ = ux, tab_w_ = uw;
    tab_x_ = ui::Approach(tab_x_, ux, 22.0f);
    tab_w_ = ui::Approach(tab_w_, uw, 22.0f);
    const float ly = std::round(o_.y + 74 * s);
    dl->AddLine(ImVec2(o_.x + Margin(), ly), ImVec2(o_.x + w_ - Margin(), ly), color::kHairline, 1.0f);
    dl->AddRectFilled(ImVec2(tab_x_, ly - 2 * s), ImVec2(tab_x_ + tab_w_, ly), color::kAccent);
    {
      const ui::Fonts f = ui::GetFonts();
      const std::string name = text::Upper(T("Paused"));
      const float fs = 15 * s, tr = 4 * s, tw = ui::TrackedWidth(f.display, fs, name, tr);
      ui::DrawTracked(dl, f.display, fs, ImVec2(o_.x + w_ - Margin() - tw, o_.y + 44 * s - fs * 0.5f), color::kTextDim,
                      name, tr);
    }

    items_ = settings_->Items(page_);
    const float top = o_.y + 104 * s, bottom = o_.y + h_ - 96 * s;
    const float x0 = o_.x + Margin(), avail = w_ - Margin() * 2, list_w = std::round(avail * 0.56f);
    ui::ListState& st = lists_[int(page_) * 4 + settings_->SubPage()];
    const Item* focused = ui::DrawList(dl, items_, st, ImVec2(x0, top), ImVec2(x0 + list_w, bottom), m_, in, active);
    ui::DrawDescription(dl, focused, ImVec2(x0 + list_w + 76 * s, top + 18 * s), ImVec2(o_.x + w_ - Margin(), bottom),
                        m_);
    focused_ = focused ? *focused : Item{};
    has_focused_ = focused != nullptr;
    if (active && in.Pressed(Action::kBack)) {
      in.Consume(Action::kBack);
      ui::Cue(ui::Sound::kBack);
      if (settings_->SubPage() != 0) settings_->LeaveSubPage();
      else view_ = View::kList;
    }
    if (active && in.Pressed(Action::kPlay)) {  // Start resumes from anywhere in the menu
      in.ConsumeAll();
      ui::Cue(ui::Sound::kBack);
      play::RunPauseAction(play::PauseAction::kResume);
    }
  }

  // --------------------------------------------------------------- footer ---
  std::vector<ui::Prompt> Prompts() const {
    const ui::Modal& modal = settings_->modal();
    if (modal.open) {
      if (settings_->Capturing()) return {{Action::kBack, "Cancel"}};
      std::vector<ui::Prompt> p;
      if (!modal.buttons.empty()) p.push_back({Action::kAccept, "Select"});
      if (modal.cancel >= 0) p.push_back({Action::kBack, "Back"});
      return p;
    }
    if (view_ == View::kList) return {{Action::kAccept, "Select"}, {Action::kBack, "Resume"}};
    std::vector<ui::Prompt> p;
    if (has_focused_) {
      if ((focused_.type == ItemType::kChoice || focused_.type == ItemType::kSlider) && !focused_.disabled)
        p.push_back({Action::kLeft, "Change"});
      if ((focused_.type == ItemType::kAction || focused_.type == ItemType::kLink) && !focused_.disabled &&
          focused_.on_activate)
        p.push_back({Action::kAccept, "Select"});
      if (focused_.on_default && !focused_.disabled) p.push_back({Action::kDefault, "Default"});
    }
    p.push_back({Action::kPlay, "Resume"});
    p.push_back({Action::kBack, "Back"});
    return p;
  }

  void DrawFooter(ImDrawList* dl) {
    const float s = m_.s, cy = o_.y + h_ - 42 * s;
    const std::string status = settings_->StatusLine();
    if (!status.empty())
      dl->AddText(ui::GetFonts().text, 15 * s, ImVec2(o_.x + Margin(), cy - 9 * s), color::kTextDim, T(status).c_str());
    const Action clicked = ui::DrawPrompts(dl, settings_->glyphs(), settings_->PromptSet(), Prompts(),
                                           o_.x + w_ - Margin(), cy, m_, true);
    if (clicked != Action::kCount) pending_click_ = clicked;
  }

#if defined(KK_DEV_TOOLS)
  // Developer-only: KK_DEV_PAUSE_INPUT="pad:down,pad:a,shot:name,..." presses buttons once the
  // menu is open, as the launcher's KK_DEV_LAUNCHER_INPUT does (pad:/ps:/kb: a b x y lb rb start
  // up down left right lt rt; move:X:Y and click:X:Y at 1280 x 720 positions), logging
  // "KK dev: pause input <step>" and, for shot:<name>, "KK dev: shot <name>" (then waits 1.6 s).
  void DevInput() {
    ImGuiIO& io = ImGui::GetIO();
    static bool release = false;
    if (release) {
      io.AddMouseButtonEvent(0, false);
      release = false;
    }
    static std::vector<std::string> steps = [] {
      std::vector<std::string> out;
      const char* v = std::getenv("KK_DEV_PAUSE_INPUT");
      std::stringstream in(v ? v : "");
      for (std::string t; std::getline(in, t, ',');)
        if (!t.empty()) out.push_back(t);
      return out;
    }();
    static size_t next = 0;
    static double at = -1;
    const double now = ImGui::GetTime();
    if (at < 0) at = now + 1.5;
    if (next >= steps.size() || now < at) return;
    std::string t = steps[next++];
    at = now + 0.45;
    REXLOG_INFO("KK dev: pause input {}", t);
    if (t.rfind("shot:", 0) == 0) {
      REXLOG_INFO("KK dev: shot {}", t.substr(5));
      at = now + 1.6;
      return;
    }
    auto at_point = [&](const std::string& xy) {
      const size_t colon = xy.find(':');
      io.AddMousePosEvent(o_.x + std::stof(xy.substr(0, colon)) * m_.s, o_.y + std::stof(xy.substr(colon + 1)) * m_.s);
    };
    if (t.rfind("move:", 0) == 0) return at_point(t.substr(5));
    if (t.rfind("click:", 0) == 0) {
      at_point(t.substr(6));
      io.AddMouseButtonEvent(0, true);
      release = true;
      return;
    }
    ui::Device d = ui::Device::kKeyboard;
    if (t.rfind("pad:", 0) == 0) d = ui::Device::kXbox, t = t.substr(4);
    else if (t.rfind("ps:", 0) == 0) d = ui::Device::kPlayStation, t = t.substr(3);
    else if (t.rfind("kb:", 0) == 0) t = t.substr(3);
    static const std::map<std::string, Action> kNames = {
        {"up", Action::kUp},       {"down", Action::kDown},    {"left", Action::kLeft},       {"right", Action::kRight},
        {"lt", Action::kPageUp},   {"rt", Action::kPageDown},  {"a", Action::kAccept},        {"b", Action::kBack},
        {"x", Action::kDefault},   {"y", Action::kResetPage},  {"lb", Action::kPrevTab},      {"rb", Action::kNextTab},
        {"start", Action::kPlay}};
    if (auto it = kNames.find(t); it != kNames.end()) settings_->input().Inject(it->second, d);
  }
#endif

  rex::ui::ImmediateDrawer* immediate_;
  std::filesystem::path user_dir_, game_dir_;
  std::unique_ptr<InGameSettings> settings_;
  std::unique_ptr<LauncherSounds> sounds_;
  std::unique_ptr<rex::ui::ImmediateTexture> logo_;
  float logo_aspect_ = 1.6f;
  bool logo_tried_ = false;
  ui::Metrics m_;
  ImVec2 o_{0, 0};
  float w_ = 1280, h_ = 720;
  bool open_ = false;
  int opened_frame_ = -10;
  double open_time_ = 0;
  View view_ = View::kList;
  int focus_ = 0;
  std::vector<float> offsets_;
  float shade_ = 0;
  InGameSettings::Page page_ = InGameSettings::kDisplay;
  std::map<int, ui::ListState> lists_;
  std::vector<Item> items_;
  Item focused_;
  bool has_focused_ = false;
  float tab_x_ = -1, tab_w_ = 0;
  Action pending_click_ = Action::kCount;
};

PauseMenu* g_menu = nullptr;

}  // namespace

void CreatePauseMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                     const std::filesystem::path& config_path, const std::filesystem::path& user_dir,
                     const std::filesystem::path& game_dir) {
  if (g_menu || !drawer) return;
  g_menu = new PauseMenu(drawer, immediate, config_path, user_dir, game_dir);  // lives for the session
}

void PauseMenuPad(uint16_t buttons, int16_t lx, int16_t ly, uint8_t lt, uint8_t rt) {
  std::lock_guard lock(g_pad.lock);
  g_pad.buttons = buttons;
  g_pad.lx = lx;
  g_pad.ly = ly;
  g_pad.lt = lt;
  g_pad.rt = rt;
  g_pad.fresh = true;
}

}  // namespace kk
