#include "pause_menu.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include "ingame_settings.h"
#include "launcher_sounds.h"
#include "launcher_text.h"
#include "launcher_ui.h"
#include "pause_menu_logic.h"
#include "photo_mode.h"
#include "play_session.h"
#include "settings_help.h"

namespace kk {
namespace {

using text::F;
using text::T;
using ui::Action;
using ui::Item;
using ui::ItemType;
using pause::Entry;
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

// The pause menu, option A (owner, 18 Oct; task 222): a slim list on the left over the paused game (Resume,
// Photo mode, Display, Graphics, Audio, Controls, Gameplay, then the game's items and Quit to main menu). Each
// settings entry opens its page in the same column ("PAUSED / DISPLAY"); Back returns to the list on the same
// entry. A page shows the rows that change while playing first, then the greyed "Change from the main menu"
// group. A description strip at the bottom left says what the focused entry or row does; the game stays
// visible on the right, so a change like Brightness shows at once.
class PauseMenu final : public rex::ui::ImGuiDialog {
 public:
  PauseMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, std::filesystem::path config_path,
            std::filesystem::path user_dir, std::filesystem::path game_dir)
      : ImGuiDialog(drawer),
        game_dir_(std::move(game_dir)),
        settings_(CreateInGameSettings(immediate, std::move(config_path), std::move(user_dir))) {}

 public:
  bool NeedsContinuousPaint() override { return play::PauseMenuOpen(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const bool open = play::PauseMenuOpen();
    if (open != open_) {
      open_ = open;
#if defined(KK_DEV_TOOLS)
      REXLOG_INFO("KK dev pause panel: {}", open ? "open" : "closed");
#endif
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
    if (sounds_) {
      const float volume = GetBool("kk_launcher_sounds") ? GetInt("kk_launcher_sounds_volume", 50) / 100.0f : 0.0f;
      sounds_->SetVolume(volume * volume);
    }
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
    m_.row_h = 42 * s;
    m_.tall_h = 70 * s;
    m_.section_h = 46 * s;

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
    if (view_ == View::kList) DrawMainList(dl);
    else DrawPage(dl);
    ui::DrawModal(dl, modal, o_, ImVec2(o_.x + w_, o_.y + h_), m_, in);
    DrawFooter(dl);
    ImGui::End();
    (void)io;
  }

 private:
  enum class View { kList, kPage };

  // The left column: where it starts, how wide the main list and a page are, and the strip's top.
  float X() const { return o_.x + 72 * m_.s; }
  float ListWidth() const { return std::min(400 * m_.s, w_ * 0.42f); }
  float PageWidth() const { return std::min(500 * m_.s, w_ * 0.5f - 72 * m_.s); }
  float StripTop() const { return o_.y + h_ - 134 * m_.s; }
  float StripBottom() const { return o_.y + h_ - 22 * m_.s; }

  void Opened() {
    opened_frame_ = ImGui::GetFrameCount();
    view_ = View::kList;
    focus_entry_ = Entry::kResume;
    offsets_.clear();
    settings_->modal().open = false;
    settings_->LeaveSubPage();
    settings_->input().ConsumeAll();
    open_time_ = ImGui::GetTime();
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

  // The frozen frame: a light dim over all of it (the game stays visible on the right, so picture changes show
  // at once) and a dark band on the left under the menu.
  void DrawShade(ImDrawList* dl) {
    const float t = std::clamp(float(ImGui::GetTime() - open_time_) / 0.18f, 0.0f, 1.0f);
    const ImVec2 p1(o_.x + w_, o_.y + h_);
    dl->AddRectFilled(o_, p1, ui::WithAlpha(color::kBase, t * 0.28f));
    const float solid = X() - o_.x + PageWidth() + 40 * m_.s;
    const ImU32 c0 = ui::WithAlpha(color::kBase, t * 0.82f), c1 = ui::WithAlpha(color::kBase, 0.0f);
    dl->AddRectFilled(o_, ImVec2(o_.x + solid, p1.y), c0);
    dl->AddRectFilledMultiColor(ImVec2(o_.x + solid, o_.y), ImVec2(o_.x + solid + w_ * 0.18f, p1.y), c0, c1, c1, c0);
    const ImU32 b0 = ui::WithAlpha(color::kBase, t * 0.55f);
    dl->AddRectFilledMultiColor(ImVec2(o_.x, p1.y - 120 * m_.s), p1, c1, c1, b0, b0);
  }

  pause::Available Availability() const {
    pause::Available a;
    a.photo = photo::Available();  // photo_mode.cpp (task 208)
    a.cheats = GetBool("kk_cheats");
#if defined(KK_DEV_TOOLS)
    // Keep the original pages reachable in test builds for profile compatibility checks.
    a.game_options = play::PauseActionAvailable(play::PauseAction::kGameOptions);
#endif
    a.restart = play::PauseActionAvailable(play::PauseAction::kStartAgain);
    a.score = play::PauseActionAvailable(play::PauseAction::kScore);
    a.save = play::PauseActionAvailable(play::PauseAction::kSave);
    a.quit_to_main_menu = play::PauseActionAvailable(play::PauseAction::kQuitToLauncher);
    a.quit_game = play::PauseActionAvailable(play::PauseAction::kQuitToDesktop);
    return a;
  }

  void Run(Entry e) {
    using play::PauseAction;
    switch (e) {
      case Entry::kResume: return play::RunPauseAction(PauseAction::kResume);
      case Entry::kPhotoMode: return photo::Enter();
      case Entry::kGameOptions: return play::RunPauseAction(PauseAction::kGameOptions);
      case Entry::kRestart:
        return Confirm("Restart chapter", "Start this chapter again from its beginning?", "Restart chapter",
                       [] { play::RunPauseAction(PauseAction::kStartAgain); });
      case Entry::kScore: return play::RunPauseAction(PauseAction::kScore);
      case Entry::kSave: return play::RunPauseAction(PauseAction::kSave);
      // The launcher is the game's main menu now: two ways out, both through the game's quit (and its saving).
      case Entry::kQuitToMainMenu:
        return Confirm("Quit to main menu", "Leave the game and go back to the main menu?", "Leave",
                       [] { play::RunPauseAction(PauseAction::kQuitToLauncher); });
      case Entry::kQuitGame:
        return Confirm("Quit game", "Leave the game and close it?", "Quit",
                       [] { play::RunPauseAction(PauseAction::kQuitToDesktop); });
      default:
        if (pause::PageOf(e) >= 0) OpenPage(InGameSettings::Page(pause::PageOf(e)));
        return;
    }
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

  void OpenPage(InGameSettings::Page page) {
    view_ = View::kPage;
    page_ = page;
    settings_->LeaveSubPage();
  }

  // Back from a page: to the list, on the entry that opened it.
  void BackToList() {
    view_ = View::kList;
    for (const pause::Listed& l : pause::Entries(Availability()))
      if (pause::PageOf(l.entry) == int(page_)) focus_entry_ = l.entry;
  }

  // ----------------------------------------------------------------------------------------- main list ---
  void DrawMainList(ImDrawList* dl) {
    const float s = m_.s;
    const ui::Fonts f = ui::GetFonts();
    ui::Input& in = settings_->input();
    const bool active = !settings_->modal().open;
    const std::vector<pause::Listed> list = pause::Entries(Availability());
    const int n = int(list.size());
    int focus = 0;
    for (int i = 0; i < n; ++i)
      if (list[size_t(i)].entry == focus_entry_) focus = i;
    const int focus_before = focus;

    const float x = X();
    const float cy = o_.y + 72 * s;
    ui::DrawTracked(dl, f.display, 15 * s, ImVec2(x, cy - 7.5f * s), color::kAccent, text::Upper(T("Paused")), 5 * s);
    dl->AddRectFilled(ImVec2(x, cy + 17 * s), ImVec2(x + 44 * s, cy + 19 * s), ui::WithAlpha(color::kAccent, 0.7f));

    // The entries share the room between the caption and the strip; the gap before the game's items is 0.4 rows.
    int gaps = 0;
    for (const pause::Listed& l : list) gaps += l.gap_before ? 1 : 0;
    const float y0 = o_.y + 118 * s, room = StripTop() - 20 * s - y0;
    const float item_h = std::clamp(room / (float(n) + 0.4f * float(gaps)), 28 * s, 40 * s);
    const float fs = std::min(23 * s, item_h * 0.62f), tr = 1.6f * s;

    if (active && n > 1) {
      if (in.Pressed(Action::kUp)) focus = (focus + n - 1) % n;
      if (in.Pressed(Action::kDown)) focus = (focus + 1) % n;
    }
    offsets_.resize(size_t(n), 0.0f);
    int chosen = -1;
    float iy = y0;
    for (int i = 0; i < n; ++i) {
      if (list[size_t(i)].gap_before) iy += 0.4f * item_h;
      const std::string label = text::Upper(T(pause::Label(list[size_t(i)].entry)));
      const ImVec2 r0(x - 8 * s, iy), r1(x + ListWidth(), iy + item_h);
      if (active && ImGui::IsMouseHoveringRect(r0, r1, false)) {
        if (settings_->input().mouse_moved()) focus = i;
        if (ImGui::IsMouseClicked(0)) focus = chosen = i;
      }
      const bool on = i == focus;
      offsets_[size_t(i)] = ui::Approach(offsets_[size_t(i)], on ? 1.0f : 0.0f, 14.0f);
      const float k = offsets_[size_t(i)];
      if (k > 0.02f)
        dl->AddRectFilled(ImVec2(x, iy + item_h * 0.5f - fs * 0.55f), ImVec2(x + 4 * s, iy + item_h * 0.5f + fs * 0.55f),
                          ui::WithAlpha(color::kAccent, k));
      ui::DrawTracked(dl, f.display, fs, ImVec2(x + 20 * s + 6 * s * k, iy + (item_h - fs) * 0.5f),
                      on ? color::kText : color::kTextDim, label, tr);
      iy += item_h;
    }
    if (focus != focus_before) ui::Cue(ui::Sound::kMove);
    if (n > 0) focus_entry_ = list[size_t(std::clamp(focus, 0, n - 1))].entry;
    DrawStrip(dl, nullptr, T(pause::What(focus_entry_)), ListWidth(), true);

    if (active && in.Pressed(Action::kAccept)) chosen = focus;
    if (active && (in.Pressed(Action::kBack) || in.Pressed(Action::kPlay))) {
      in.ConsumeAll();
      ui::Cue(ui::Sound::kBack);
      play::RunPauseAction(play::PauseAction::kResume);
      return;
    }
    if (chosen >= 0) {
      in.ConsumeAll();
      ui::Cue(ui::Sound::kSelect);
      Run(list[size_t(chosen)].entry);
    }
  }

  // --------------------------------------------------------------------------------------------- pages ---
  // The page's rows in the pause menu's order (pause_menu_logic.h): the live rows under their sections, then the
  // quiet "Change from the main menu" group of rows that can't change while playing.
  static std::vector<Item> Arrange(std::vector<Item> items) {
    std::vector<pause::RowKind> kinds;
    for (const Item& it : items) {
      if (it.type == ItemType::kSection) kinds.push_back(pause::RowKind::kSection);
      else if (it.disabled && it.applies && std::string_view(it.applies) == help::kNotWhilePlaying)
        kinds.push_back(pause::RowKind::kGreyed);
      else kinds.push_back(pause::RowKind::kLive);
    }
    std::vector<Item> out;
    for (int i : pause::PageOrder(kinds)) {
      if (i == pause::kQuietHeading) {
        Item caption;
        caption.type = ItemType::kSection;
        caption.label = pause::kQuietCaption;
        caption.value_color = color::kTextFaint;
        out.push_back(std::move(caption));
      } else {
        out.push_back(std::move(items[size_t(i)]));
      }
    }
    return out;
  }

  void DrawPage(ImDrawList* dl) {
    const float s = m_.s;
    const ui::Fonts f = ui::GetFonts();
    ui::Input& in = settings_->input();
    const bool active = !settings_->modal().open;
    // LB / RB: the previous or next settings page of the list.
    if (active && (in.Pressed(Action::kPrevTab) || in.Pressed(Action::kNextTab))) {
      const int dir = in.Pressed(Action::kNextTab) ? 1 : -1;
      const int next = pause::NextPage(pause::Entries(Availability()), int(page_), dir);
      if (next != int(page_)) {
        page_ = InGameSettings::Page(next);
        settings_->LeaveSubPage();
        ui::Cue(ui::Sound::kTab);
      }
      in.Consume(Action::kPrevTab);
      in.Consume(Action::kNextTab);
    }

    // The breadcrumb: PAUSED (click it to go back) / the page's name, underlined.
    const float x = X(), cy = o_.y + 72 * s;
    bool back = false;
    {
      const std::string root = text::Upper(T("Paused"));
      const float rs = 15 * s, rtr = 5 * s, rw = ui::TrackedWidth(f.display, rs, root, rtr);
      const bool hover = active && ImGui::IsMouseHoveringRect(ImVec2(x - 6 * s, cy - 16 * s), ImVec2(x + rw + 6 * s, cy + 16 * s), false);
      if (hover && ImGui::IsMouseClicked(0)) back = true;
      ui::DrawTracked(dl, f.display, rs, ImVec2(x, cy - rs * 0.5f), hover ? color::kText : color::kTextDim, root, rtr);
      const float sx = x + rw + 22 * s;
      dl->AddText(f.display, 22 * s, ImVec2(sx, cy - 11 * s), color::kTextFaint, "/");
      const std::string name = text::Upper(T(InGameSettings::PageName(page_)));
      const float ns = 22 * s, ntr = 1.5f * s, nx = sx + 26 * s, nw = ui::TrackedWidth(f.display, ns, name, ntr);
      ui::DrawTracked(dl, f.display, ns, ImVec2(nx, cy - ns * 0.5f), color::kAccent, name, ntr);
      dl->AddRectFilled(ImVec2(nx, cy + 17 * s), ImVec2(nx + nw, cy + 19 * s), color::kAccent);
    }

    items_ = Arrange(settings_->Items(page_));
    const float top = o_.y + 112 * s, bottom = StripTop() - 14 * s;
    ui::ListState& st = lists_[int(page_) * 4 + settings_->SubPage()];
    const Item* focused = ui::DrawList(dl, items_, st, ImVec2(x, top), ImVec2(x + PageWidth(), bottom), m_, in, active);
    DrawStrip(dl, focused, std::string(), PageWidth(), true);
    focused_ = focused ? *focused : Item{};
    has_focused_ = focused != nullptr;
    if (active && ImGui::IsMouseClicked(1)) back = true;  // the right mouse button goes back too
    if (active && (in.Pressed(Action::kBack) || back)) {
      in.Consume(Action::kBack);
      ui::Cue(ui::Sound::kBack);
      if (settings_->SubPage() != 0) settings_->LeaveSubPage();
      else BackToList();
    }
    if (active && in.Pressed(Action::kResetPage) && settings_->CanResetPage(page_)) {
      in.Consume(Action::kResetPage);
      const auto page = page_;
      Confirm("Reset page", "Put these settings back to their defaults?", "Reset",
              [this, page] { settings_->ResetPage(page); });
    }
    if (active && in.Pressed(Action::kPlay)) {  // Start resumes from anywhere in the menu
      in.ConsumeAll();
      ui::Cue(ui::Sound::kBack);
      play::RunPauseAction(play::PauseAction::kResume);
    }
  }

  // ------------------------------------------------------------------------------------------- the strip ---
  // The description strip at the bottom left: a hairline, what the entry or row does (task 220's description),
  // the chosen option's note, why a row is greyed, then one facts line (when a change takes effect, the
  // performance and video memory labels, the default). Too tall: smaller text, then without the option's note.
  struct StripPart {
    std::string text;
    ImU32 color;
  };
  struct StripText {
    std::string what, note, warn;
    std::vector<StripPart> facts;
  };
  static StripText Compose(const Item* item, const std::string& what) {
    StripText t;
    t.what = what;
    if (!item || item->type == ItemType::kSection) return t;
    if (!item->description.empty()) t.what = T(item->description);
    else if (item->help && item->help->how) t.what = T(item->help->how);
    if (item->help && item->type == ItemType::kChoice && item->index >= 0 && item->index < int(item->options.size())) {
      const std::string& chosen = item->options[size_t(item->index)];
      if (const help::OptionNote* n = help::NoteFor(*item->help, chosen)) t.note = T(chosen) + ": " + T(n->note);
    }
    const bool greyed_note = item->disabled && !item->disabled_note.empty();
    if (greyed_note) t.warn = T(item->disabled_note);
    if (item->applies) {
      const bool not_now = std::string_view(item->applies) == help::kNotWhilePlaying;
      if (!(not_now && greyed_note)) t.facts.push_back({T(item->applies), not_now ? color::kWarn : color::kAccent});
    }
    if (item->help) {
      if (const char* p = help::PerfLabel(item->help->perf)) t.facts.push_back({T(p), color::kAccent});
      if (const char* v = help::VramLabel(item->help->vram)) t.facts.push_back({T(v), color::kAccent});
    }
    if (!item->default_text.empty()) t.facts.push_back({F("Default: {0}", {T(item->default_text)}), color::kAccent});
    return t;
  }

  // Lays the strip out at (x, y) and width w with text scale k; draws it when `dl` is set. Returns its height.
  float LayoutStrip(ImDrawList* dl, const StripText& t, float x, float y, float w, float k, bool note) const {
    const ui::Fonts f = ui::GetFonts();
    const float s = m_.s, y0 = y;
    auto para = [&](const std::string& text, float size, ImU32 col, float gap) {
      if (text.empty()) return;
      if (dl) dl->AddText(f.text, size, ImVec2(x, y), col, text.c_str(), nullptr, w);
      y += ui::TextSize(f.text, size, text, w).y + gap;
    };
    para(t.what, 16 * s * k, color::kText, 6 * s * k);
    if (note) para(t.note, 14 * s * k, color::kTextDim, 6 * s * k);
    para(t.warn, 14 * s * k, color::kWarn, 6 * s * k);
    if (!t.facts.empty()) {
      const float fs = 13.5f * s * k, line = fs + 6 * s * k;
      const std::string sep = "|";
      const float sep_w = ui::TextSize(f.text, fs, sep).x + 20 * s * k;
      float fx = x;
      for (size_t i = 0; i < t.facts.size(); ++i) {
        const float tw = ui::TextSize(f.text, fs, t.facts[i].text).x;
        if (i > 0) {
          if (fx + sep_w + tw > x + w) {
            fx = x;
            y += line;
          } else {
            if (dl) dl->AddText(f.text, fs, ImVec2(fx + 10 * s * k, y), color::kTextFaint, sep.c_str());
            fx += sep_w;
          }
        }
        if (dl) dl->AddText(f.text, fs, ImVec2(fx, y), t.facts[i].color, t.facts[i].text.c_str());
        fx += tw;
      }
      y += line;
    }
    return y - y0;
  }

  // How the strip fits: 0 at full size, 1 or 2 with smaller text, 3 without the option's note, 4 too tall even
  // then (drawn anyway, clipped). `k` and `note` get the layout to draw with.
  int FitStrip(const StripText& t, float w, float* k, bool* note) const {
    static constexpr float kScales[] = {1.0f, 0.93f, 0.86f};
    const float avail = StripBottom() - (StripTop() + 14 * m_.s);
    for (int i = 0; i < 3; ++i)
      if (LayoutStrip(nullptr, t, 0, 0, w, kScales[i], true) <= avail) {
        *k = kScales[i], *note = true;
        return i;
      }
    *k = kScales[2], *note = false;
    return LayoutStrip(nullptr, t, 0, 0, w, *k, false) <= avail ? 3 : 4;
  }

  void DrawStrip(ImDrawList* dl, const Item* item, const std::string& what, float w, bool line) {
    const float s = m_.s, x = X(), y = StripTop();
    if (line) dl->AddLine(ImVec2(x, std::round(y) + 0.5f), ImVec2(x + w, std::round(y) + 0.5f), color::kHairline, 1.0f);
    const StripText t = Compose(item, what);
    float k = 1.0f;
    bool note = true;
    FitStrip(t, w, &k, &note);
    dl->PushClipRect(ImVec2(x - 2 * s, y + 2 * s), ImVec2(x + w + 2 * s, StripBottom()), true);
    LayoutStrip(dl, t, x, y + 14 * s, w, k, note);
    dl->PopClipRect();
  }

  // --------------------------------------------------------------------------------------------- footer ---
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
    if (settings_->CanResetPage(page_)) p.push_back({Action::kResetPage, "Reset page"});
    p.push_back({Action::kPlay, "Resume"});
    p.push_back({Action::kBack, "Back"});
    return p;
  }

  void DrawFooter(ImDrawList* dl) {
    const float s = m_.s, cy = o_.y + h_ - 48 * s, right = o_.x + w_ - 72 * s;
    const std::string status = settings_->StatusLine();
    if (!status.empty()) {
      const ui::Fonts f = ui::GetFonts();
      const std::string line = T(status);
      const float tw = ui::TextSize(f.text, 15 * s, line).x;
      dl->AddText(f.text, 15 * s, ImVec2(right - tw, cy - 44 * s), color::kTextDim, line.c_str());
    }
    const Action clicked =
        ui::DrawPrompts(dl, settings_->glyphs(), settings_->PromptSet(), Prompts(), right, cy, m_, true);
    if (clicked != Action::kCount) pending_click_ = clicked;
  }

#if defined(KK_DEV_TOOLS)
  // Developer-only: KK_DEV_PAUSE_INPUT="pad:down,pad:a,shot:name,..." presses buttons once the
  // menu is open, as the launcher's KK_DEV_LAUNCHER_INPUT does (pad:/ps:/kb: a b x y lb rb start
  // up down left right lt rt; move:X:Y and click:X:Y at 1280 x 720 positions), logging
  // "KK dev: pause input <step>" and, for shot:<name>, "KK dev: shot <name>" (then holds for capture).
  // page:<name> (or settings:<name>) opens a page and logs its rows in the pause menu's order; focus:<row>
  // focuses a row; language:<n> sets user_language; strip-fit logs how every row's strip fits in the five
  // languages (task 222, like the launcher's desc-fit).
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
      rex::FlushLogging();
      at = now + 4.0;
      return;
    }
    // Deterministic integration checks still drive the actual list with pad
    // actions; selecting a page/row here avoids depending on optional entries.
    if (t.rfind("settings:", 0) == 0 || t.rfind("page:", 0) == 0) {
      const std::string want = t.substr(t.find(':') + 1);
      for (int p = 0; p < InGameSettings::kPageCount; ++p) {
        std::string name = InGameSettings::PageName(InGameSettings::Page(p));
        for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name != want) continue;
        OpenPage(InGameSettings::Page(p));
        for (const auto& row : Arrange(settings_->Items(page_)))
          REXLOG_INFO("KK dev: pause page {} row {}{}", want, row.label,
                      row.type == ItemType::kSection ? " (section)" : row.disabled ? " (greyed)" : "");
        return;
      }
      REXLOG_INFO("KK dev: pause page FAIL {}", want);
      return;
    }
    if (t.rfind("focus:", 0) == 0) {
      const auto rows = Arrange(settings_->Items(page_));
      for (size_t i = 0; i < rows.size(); ++i) if (rows[i].label == t.substr(6) && rows[i].type != ItemType::kSection) {
        auto& state = lists_[int(page_) * 4 + settings_->SubPage()];
        state.focus = int(i);
        state.focus_label = rows[i].label;
        REXLOG_INFO("KK dev: pause focused {} value {}", rows[i].label, rows[i].value_text);
        return;
      }
      REXLOG_INFO("KK dev: pause focus FAIL {}", t.substr(6));
      return;
    }
    if (t.rfind("language:", 0) == 0) {
      rex::cvar::SetFlagByName("user_language", t.substr(9));
      return;
    }
    if (t == "strip-fit") return LogStripFit();
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

  // Every row's strip and every list entry's sentence, in the five languages, at this window size: logs the rows
  // that need smaller text or lose their option note ("KK dev: stripfit <lang> <page> '<row>' fit <n>") and a
  // summary per language ("KK dev: stripfit <lang> rows <n> full <a> smaller <b> no-note <c> too-tall <d>").
  void LogStripFit() {
    const std::string language = rex::cvar::GetFlagByName("user_language");
    settings_->LeaveSubPage();
    for (const char* lang : {"1", "3", "5", "4", "6"}) {
      rex::cvar::SetFlagByName("user_language", lang);
      int rows = 0, counts[5] = {};
      auto check = [&](const Item* item, const std::string& what, float w, const char* page, const std::string& label) {
        float k = 1.0f;
        bool note = true;
        const int fit = FitStrip(Compose(item, what), w, &k, &note);
        ++rows;
        ++counts[std::clamp(fit, 0, 4)];
        if (fit > 0) REXLOG_INFO("KK dev: stripfit {} {} '{}' fit {}", lang, page, label, fit);
      };
      pause::Available all;
      all.photo = all.cheats = all.restart = all.score = all.save = true;
      for (const pause::Listed& l : pause::Entries(all))
        check(nullptr, T(pause::What(l.entry)), ListWidth(), "list", pause::Label(l.entry));
      for (const pause::Listed& l : pause::Entries(all)) {
        const int p = pause::PageOf(l.entry);
        if (p < 0) continue;
        for (const Item& it : Arrange(settings_->Items(InGameSettings::Page(p))))
          if (it.type != ItemType::kSection) check(&it, std::string(), PageWidth(), InGameSettings::PageName(InGameSettings::Page(p)), it.label);
      }
      REXLOG_INFO("KK dev: stripfit {} rows {} full {} smaller {} no-note {} too-tall {}", lang, rows, counts[0],
                  counts[1] + counts[2], counts[3], counts[4]);
    }
    rex::cvar::SetFlagByName("user_language", language);
  }
#endif

  std::filesystem::path game_dir_;
  std::unique_ptr<InGameSettings> settings_;
  std::unique_ptr<LauncherSounds> sounds_;
  ui::Metrics m_;
  ImVec2 o_{0, 0};
  float w_ = 1280, h_ = 720;
  bool open_ = false;
  int opened_frame_ = -10;
  double open_time_ = 0;
  View view_ = View::kList;
  Entry focus_entry_ = Entry::kResume;
  std::vector<float> offsets_;
  InGameSettings::Page page_ = InGameSettings::kDisplay;
  std::map<int, ui::ListState> lists_;
  std::vector<Item> items_;
  Item focused_;
  bool has_focused_ = false;
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
