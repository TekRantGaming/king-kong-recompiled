// Photo mode (task 208): see photo_mode.h and native-renderer/docs/photo-mode.md.
//
// How it works:
//   - The freeze is the game's own pause: photo mode opens from the pause menu and the game stays paused
//     (its pause page keeps running, the world is not stepped, timers and sounds stay paused as in the
//     pause menu). The world display still draws every frame while paused (sub_82857E80 -> sub_828732E8,
//     task 208 phase 0); the game only covers it with one black quad at the end of the frame, which the
//     renderer leaves out in photo mode (backend/photo_config.h). No frame time or time scale is touched.
//   - The free camera (freecam.cpp) flies the scene view within kRadius metres of where photo mode began;
//     roll, field of view and speed come from here (fov.cpp asks for the field of view).
//   - The HUD, the overlays and the first-person weapon pass are hidden (freecam::HideHud / HideGameHud);
//     Jack's first-person arms and weapon are left out by the renderer, by their meshes.
//   - The photo grade, depth of field and picture settings are the renderer's (plugin/native_photo.h),
//     sent each frame; nothing is saved and the player's settings are never changed.
//   - Take picture saves the game's frame (the guest output: no UI) as a PNG in <user data>/screenshots.

#include "photo_mode.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include "art.h"
#include "freecam.h"
#include "glyphs.h"
#include "key_glyphs.h"
#include "hooks/kk_native_photo.h"
#include "launcher_art.h"
#include "launcher_text.h"
#include "launcher_ui.h"
#include "photo_mode_logic.h"
#include "play_internal.h"
#include "ui_paint.h"

#if defined(KK_DEV_TOOLS)
namespace kk {
double DevSecondsSinceMenu();
}
#endif

namespace kk::photo {
namespace {

using text::F;
using text::T;
using ui::Action;
using ui::Item;
using ui::ItemType;
namespace color = ui::color;

std::atomic<bool> g_active{false};
std::atomic<uint8_t*> g_base{nullptr};  // guest memory (Frame), for the developer steps' readings
std::atomic<bool> g_exit_requested{false}, g_resume_requested{false};
std::atomic<bool> g_quiet{false};  // task 354: the comparison capture holds photo mode (no panel, no input)
std::mutex g_values_mutex;
Values g_values;

struct FedPad {
  std::mutex lock;
  uint16_t buttons = 0;
  bool fresh = false;
} g_pad;

std::string Flag(const char* name) { return rex::cvar::GetFlagByName(name); }
double Number(const char* name, double fallback) {
  const std::string text = Flag(name);
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  return end && end != text.c_str() ? v : fallback;
}
int PlayerSharpness() {
  const std::string s = Flag("native_sharpness");
  return s == "low" ? 1 : s == "medium" ? 2 : s == "high" ? 3 : 0;
}

void SendToRenderer() {
  Values v;
  {
    std::lock_guard lock(g_values_mutex);
    v = g_values;
  }
  const bool active = g_active.load();
  NrPhotoSettings s = ToRenderer(v, active);
  // The paused game dims its world (task 272): taken back first, so the view and the pictures look like play.
  if (active) s.undim = play::PhotoUndim();
  nr::kk::SetPhoto(s);
}

}  // namespace

bool Active() { return g_active.load(); }

bool Available() { return !g_active.load() && play::PauseMenuOpen(); }

namespace {
// Task 272: chosen in the first moments of a pause, photo mode opens once the paused game's dimming has been
// measured (pause_hooks.cpp, 1.5 s into the pause, only while the pause menu shows), at most this long after.
constexpr int64_t kDimWaitMs = 2500;
std::atomic<int64_t> g_enter_waiting_since{-1};
int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void EnterNow();
// The pause menu's frame (the panel's OnDraw, before photo mode is on): open it when the measurement is in.
void ServiceWaitingEnter() {
  const int64_t since = g_enter_waiting_since.load();
  if (since < 0 || g_active.load()) return;
  if (!Available()) {
    g_enter_waiting_since = -1;  // the pause menu went away meanwhile
  } else if (play::PhotoDimKnown() || SteadyMs() - since > kDimWaitMs) {
    g_enter_waiting_since = -1;
    EnterNow();
  }
}
}  // namespace

void Enter() {
  if (!Available()) {
    REXLOG_WARN("KK photo mode: not available now");
    return;
  }
  if (!play::PhotoDimKnown()) {
    int64_t none = -1;
    if (g_enter_waiting_since.compare_exchange_strong(none, SteadyMs()))
      REXLOG_INFO("KK photo mode: opens once the paused game's brightness is measured");
    return;
  }
  g_enter_waiting_since = -1;
  EnterNow();
}

namespace {
void EnterNow() {
  std::random_device rd;
  {
    std::lock_guard lock(g_values_mutex);
    g_values = Fresh(Flag("native_cel_shading") == "true" ? 1 : 0, PlayerSharpness(), float(Number("kk_brightness", 0)),
                     float(Number("kk_gamma", 1)), rd());
  }
  g_exit_requested = g_resume_requested = false;
  g_active = true;
  freecam::PhotoStart(kRadius);
  freecam::PhotoParams(0, g_values.speed, false);
  SendToRenderer();
  RequestUiPaint();
  REXLOG_INFO("KK photo mode: opened (the game stays paused; its dimming taken back x{:.2f})", play::PhotoUndim());
}
}  // namespace

void Exit(bool resume_game) {
  if (!g_active.exchange(false)) return;
  freecam::PhotoStop();
  nr::kk::SetPhoto(ToRenderer(Values{}, false));
  RequestUiPaint();
  REXLOG_INFO("KK photo mode: closed{}", resume_game ? "; the game resumes" : "; back to the pause menu");
  if (resume_game) play::RunPauseAction(play::PauseAction::kResume);
}

void SetQuiet(bool quiet) {
  g_quiet = quiet;
  RequestUiPaint();
}

void OnPad(uint16_t buttons, int16_t, int16_t, int16_t, int16_t, uint8_t, uint8_t) {
  std::lock_guard lock(g_pad.lock);
  g_pad.buttons = buttons;
  g_pad.fresh = true;
}

float SceneFieldOfView(float game_radians) {
  if (!g_active.load()) return game_radians;
  std::lock_guard lock(g_values_mutex);
  if (g_values.default_fov <= 0) {
    g_values.default_fov = std::round(game_radians * 180.f / 3.14159265f);
    if (g_values.fov <= 0) g_values.fov = g_values.default_fov;
  }
  return FieldOfViewRadians(g_values, game_radians);
}

void Frame(uint8_t* base) {
  g_base = base;
  // The pause ended under photo mode (a level change, the game's own page): photo mode ends with it. The
  // pause page counts as gone after 300 ms without running, which a long frame (a stall) also looks like
  // at the start of the next frame: only a pause gone for 10 frames in a row ends photo mode.
  static int gone = 0;
  if (!g_active.load()) {
    gone = 0;
    return;
  }
  if (!freecam::PhotoActive() || !freecam::Active()) gone = 10;  // a level change reset the camera
  else if (!play::GamePausePageUp()) ++gone;
  else gone = 0;
  if (gone >= 10) {
    REXLOG_INFO("KK photo mode: the pause ended; photo mode closes");
    gone = 0;
    Exit(false);
  }
}

namespace {

// The panel: the launcher's style (tabs, list, description, prompts) in a column on the right, over the
// live scene. A takes the picture, B goes back to the pause menu, Y hides the panel, LB / RB change tabs.
class Panel final : public rex::ui::ImGuiDialog {
 public:
  Panel(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, std::filesystem::path user_dir)
      : ImGuiDialog(drawer),
        user_dir_(std::move(user_dir)),
        input_(false),
        glyphs_(immediate, rex::filesystem::GetExecutableFolder() / "glyphs") {
    glyphs_.AddSet("xbox360", art::LauncherArtDir(user_dir_) / "glyphs" / "xbox360");
  }

  bool NeedsContinuousPaint() override { return g_active.load(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const bool active = g_active.load();
    if (active != open_) {
      open_ = active;
      if (active) Opened();
    }
#if defined(KK_DEV_TOOLS)
    DevSteps();
#endif
    ServiceWaitingEnter();
    if (!g_active.load()) return;
    if (g_quiet.load()) {  // task 354: frozen for the comparison capture; nothing drawn, nothing read
      SendToRenderer();
      return;
    }
    {
      std::lock_guard lock(g_pad.lock);
      if (g_pad.fresh) input_.FeedPad(g_pad.buttons, 0, 0, 0, 0);  // the sticks and triggers fly the camera
      g_pad.fresh = false;
    }
    input_.set_keyboard_enabled(false);  // Space, Q and E fly; the panel's keys are below
    input_.Update();
    if (ImGui::GetFrameCount() <= opened_frame_ + 1) input_.ConsumeAll();  // the press that opened it
    Keys();
#if defined(KK_DEV_TOOLS)
    for (const Action a : dev_inject_) input_.Inject(a, ui::Device::kXbox);
    dev_inject_.clear();
#endif
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    o_ = vp->Pos;
    w_ = vp->Size.x;
    h_ = vp->Size.y;
    const float s = std::clamp(std::min(w_ / 1280.0f, h_ / 720.0f), 0.7f, 3.0f);
    m_.s = s;
    m_.row_h = 40 * s;
    m_.tall_h = 64 * s;
    m_.section_h = 44 * s;

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##kk_photo_mode", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // A (Enter) always takes the picture: the list never sees it (it would change a choice row instead).
    take_ = input_.Pressed(Action::kAccept);
    input_.Consume(Action::kAccept);
    // X (Delete) on an action row chooses it; on other rows the list puts the row back to its default.
    activate_ = false;
    if (input_.Pressed(Action::kDefault) && has_focused_ && focused_.type == ItemType::kAction) {
      activate_ = true;
      input_.Consume(Action::kDefault);
    }
    const bool over_panel = !hidden_ && ImGui::IsMouseHoveringRect(panel0_, panel1_, false);
    // The mouse turns the camera with the panel hidden, or while the right button is held off the panel.
    const bool look = hidden_ || (ImGui::IsMouseDown(1) && (!over_panel || looking_));
    looking_ = look && !hidden_;
    if (!hidden_) DrawPanel(dl);
    DrawFooter(dl);
    DrawSaved(dl);
    Handle();
    {
      std::lock_guard lock(g_values_mutex);
      freecam::PhotoParams(g_values.roll * 3.14159265f / 180.f, g_values.speed, look);
    }
    SendToRenderer();
    ImGui::End();
    (void)io;
  }

 private:
  enum Tab { kCamera, kColour, kEffects, kPicture, kTabCount };

  void Opened() {
    opened_frame_ = ImGui::GetFrameCount();
    tab_ = kCamera;
    hidden_ = false;
    looking_ = false;
    lists_.clear();
    saved_until_ = 0;
    input_.ConsumeAll();
  }

  // The panel's keys (the keyboard's actions are injected so the prompts show the keyboard's pictures).
  void Keys() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeyAlt) return;
    static constexpr std::pair<ImGuiKey, Action> kRepeat[] = {
        {ImGuiKey_UpArrow, Action::kUp}, {ImGuiKey_DownArrow, Action::kDown}, {ImGuiKey_LeftArrow, Action::kLeft},
        {ImGuiKey_RightArrow, Action::kRight}, {ImGuiKey_PageUp, Action::kPageUp}, {ImGuiKey_PageDown, Action::kPageDown}};
    for (const auto& [key, a] : kRepeat)
      if (ImGui::IsKeyPressed(key, true)) input_.Inject(a, ui::Device::kKeyboard);
    static constexpr std::pair<ImGuiKey, Action> kOnce[] = {
        {ImGuiKey_Enter, Action::kAccept},    {ImGuiKey_KeypadEnter, Action::kAccept}, {ImGuiKey_Escape, Action::kBack},
        {ImGuiKey_Backspace, Action::kBack},  {ImGuiKey_Delete, Action::kDefault},     {ImGuiKey_R, Action::kResetPage},
        {ImGuiKey_Q, Action::kPrevTab},       {ImGuiKey_E, Action::kNextTab},          {ImGuiKey_P, Action::kPlay}};
    for (const auto& [key, a] : kOnce)
      if (ImGui::IsKeyPressed(key, false)) input_.Inject(a, ui::Device::kKeyboard);
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) input_.Inject(io.KeyShift ? Action::kPrevTab : Action::kNextTab,
                                                                ui::Device::kKeyboard);
  }

  std::string PromptSet() const {
    if (input_.device() == ui::Device::kKeyboard) return "keyboard";
    const std::string p = Flag("kk_button_prompts");
    if (p == "auto" || p.empty()) return input_.pad_style().empty() ? DetectedPadPrompts() : input_.pad_style();
    if (p == "xbox360" || p == "xbox_series" || p == "ps5" || p == "ps2" || p == "steam_deck") return p;
    return input_.pad_device() == ui::Device::kPlayStation ? "ps5" : "xbox_series";
  }

  static const char* TabName(int t) {
    static const char* const kNames[] = {"Camera", "Colour", "Effects", "Picture"};
    return t >= 0 && t < kTabCount ? kNames[t] : "";
  }

  // ---------------------------------------------------------------- rows ---
  static std::string Fmt(float v, int decimals, const char* unit = "") {
    char b[48];
    std::snprintf(b, sizeof(b), "%.*f%s", decimals, double(v), unit);
    return b;
  }
  static std::string Percent(float v) { return Fmt(v, 0, "%"); }
  static std::string Signed(float v, int decimals) {
    return (v > 0 ? "+" : "") + Fmt(v, decimals);
  }

  static Item Slider(const char* label, const char* desc, float* value, float lo, float hi, float step, float def,
                     std::function<std::string(float)> format) {
    Item it;
    it.type = ItemType::kSlider;
    it.label = label;
    it.description = desc;
    it.lo = lo;
    it.hi = hi;
    it.step = step;
    it.value = std::clamp(*value, lo, hi);
    it.value_text = format(it.value);
    it.default_text = format(def);
    it.on_value = [value, lo, hi](float v) { *value = std::clamp(v, lo, hi); };
    it.on_default = [value, def] { *value = def; };
    return it;
  }
  static Item Choice(const char* label, const char* desc, int* value, std::vector<std::string> options, int def) {
    Item it;
    it.type = ItemType::kChoice;
    it.label = label;
    it.description = desc;
    it.index = std::clamp(*value, 0, int(options.size()) - 1);
    it.default_text = options[size_t(std::clamp(def, 0, int(options.size()) - 1))];
    it.options = std::move(options);
    it.on_choice = [value](int i) { *value = i; };
    it.on_default = [value, def] { *value = def; };
    return it;
  }
  static Item Toggle(const char* label, const char* desc, bool* value, bool def) {
    Item it;
    it.type = ItemType::kChoice;
    it.label = label;
    it.description = desc;
    it.options = {"Off", "On"};
    it.index = *value ? 1 : 0;
    it.default_text = def ? "On" : "Off";
    it.on_choice = [value](int i) { *value = i == 1; };
    it.on_default = [value, def] { *value = def; };
    return it;
  }
  static Item ActionRow(const char* label, const char* desc, std::function<void()> fn) {
    Item it;
    it.type = ItemType::kAction;
    it.label = label;
    it.description = desc;
    it.on_activate = std::move(fn);
    return it;
  }

  // Rows point into `edit_`, the frame's copy of the values (written back after the list ran).
  std::vector<Item> Rows(int tab) {
    Values& v = edit_;
    std::vector<Item> items;
    switch (tab) {
      case kCamera: {
        const float def_fov = v.default_fov > 0 ? v.default_fov : 69;
        items.push_back(Slider("Field of view", "How wide the camera sees. Lower zooms in, higher shows more around it.",
                               &v.fov, 20, 110, 1, def_fov, [](float x) { return Fmt(x, 0, "°"); }));
        items.push_back(Slider("Roll", "Tilts the camera to the left or right.", &v.roll, -45, 45, 1, 0,
                               [](float x) { return Fmt(x, 0, "°"); }));
        items.push_back(Slider("Camera speed", "How fast the camera moves. Hold the left stick button (Shift) to go "
                               "faster and the right stick button (Alt) to go slower.",
                               &v.speed, 0.5f, 12, 0.5f, 3, [](float x) { return Fmt(x, 1, " m/s"); }));
        items.push_back(Toggle("Hide Jack's arms", "Hides Jack's floating arms while you take pictures. "
                               "Nothing changes in the game.",
                               &v.hide_arms, true));
        items.push_back(ActionRow("Reset camera", "Puts the camera back where photo mode started.",
                               [] { freecam::PhotoResetView(); }));
        break;
      }
      case kColour: {
        items.push_back(Choice("Look", "A colour style for the whole picture.", &v.look,
                               {"None", "Black and white", "Sepia", "Warm", "Cool", "Green", "Rose"}, 0));
        items.push_back(Slider("Look strength", "How strongly the look is applied.", &v.look_strength, 0, 100, 5, 100,
                               Percent));
        items.push_back(Slider("Exposure", "Brightens or darkens the whole picture, in camera stops.", &v.exposure, -2, 2,
                               0.1f, 0, [](float x) { return Signed(x, 1); }));
        items.push_back(Slider("Contrast", "The difference between dark and bright parts.", &v.contrast, 50, 150, 5, 100,
                               Percent));
        items.push_back(Slider("Saturation", "How strong the colours are. 0% is black and white.", &v.saturation, 0, 200,
                               5, 100, Percent));
        items.push_back(Slider("White balance", "Makes the picture cooler (blue) or warmer (orange).", &v.temperature,
                               -100, 100, 5, 0, [](float x) { return Signed(x, 0); }));
        break;
      }
      case kEffects: {
        items.push_back(Slider("Depth of field", "Blurs what is nearer or farther than the focus distance, like a camera "
                               "lens. 0% is off.",
                               &v.dof, 0, 100, 5, 0, Percent));
        items.push_back(Slider("Focus distance", "How far from the camera the picture is sharp.", &v.focus, 0.5f, 60, 0.5f,
                               5, [](float x) { return Fmt(x, 1, " m"); }));
        items.push_back(Slider("Focus range", "How deep the sharp area is, in front of and behind the focus distance.",
                               &v.focus_range, 0.25f, 20, 0.25f, 2, [](float x) { return Fmt(x, 2, " m"); }));
        items.push_back(Slider("Vignette", "Darkens the corners of the picture.", &v.vignette, 0, 100, 5, 0, Percent));
        items.push_back(Slider("Film grain", "Adds the fine grain of photographic film.", &v.grain, 0, 100, 5, 0, Percent));
        items.push_back(Choice("Letterbox", "Black bars that give the picture a cinema shape.", &v.letterbox,
                               {"Off", "1.85:1", "2:1", "2.39:1", "4:3", "1:1"}, 0));
        items.push_back(Toggle("Mirror", "Flips the picture from left to right.", &v.flip, false));
        break;
      }
      case kPicture: {
        items.push_back(Choice("Cel shading", "Ink outlines and flat colour bands, as in Settings. Only while photo mode "
                               "is open.",
                               &v.cel, {"Off", "On"}, v.player_cel));
        items.push_back(Choice("Sharpening", "Sharpens the picture, as in Settings. Only while photo mode is open.",
                               &v.sharpness, {"Off", "Low", "Medium", "High"}, v.player_sharpness));
        items.push_back(Slider("Brightness", "Raises or lowers the darkest parts of the picture, as in Settings. Only "
                               "while photo mode is open.",
                               &v.brightness, -50, 50, 1, v.player_brightness, [](float x) { return Signed(x, 0); }));
        items.push_back(Slider("Gamma", "Brightens or darkens the middle tones, as in Settings. Only while photo mode is "
                               "open.",
                               &v.gamma, 0.5f, 1.5f, 0.05f, v.player_gamma, [](float x) { return Fmt(x, 2); }));
        items.push_back(ActionRow("Reset all", "Puts every photo mode setting back to its default.", [this] {
          const Values fresh = Fresh(edit_.player_cel, edit_.player_sharpness, edit_.player_brightness,
                                     edit_.player_gamma, edit_.seed);
          const float fov = edit_.default_fov;
          edit_ = fresh;
          edit_.default_fov = edit_.fov = fov;
        }));
        break;
      }
    }
    return items;
  }

  // ---------------------------------------------------------------- draw ---
  void DrawPanel(ImDrawList* dl) {
    const float s = m_.s;
    const ui::Fonts f = ui::GetFonts();
    const float margin = 28 * s, width = std::min(470 * s, w_ * 0.42f);
    panel0_ = ImVec2(o_.x + w_ - margin - width, o_.y + margin);
    panel1_ = ImVec2(o_.x + w_ - margin, o_.y + h_ - 76 * s);
    dl->AddRectFilled(panel0_, panel1_, ui::WithAlpha(color::kBase, 0.84f), 14 * s);
    dl->AddRect(panel0_, panel1_, color::kHairline, 14 * s);
    const float x0 = panel0_.x + 24 * s, x1 = panel1_.x - 24 * s;
    float y = panel0_.y + 20 * s;
    ui::DrawTracked(dl, f.display, 15 * s, ImVec2(x0, y), color::kAccent, text::Upper(T("Photo mode")), 5 * s);
    dl->AddRectFilled(ImVec2(x0, y + 24 * s), ImVec2(x0 + 40 * s, y + 26 * s), ui::WithAlpha(color::kAccent, 0.6f));
    y += 52 * s;
    std::vector<std::string> names;
    for (int t = 0; t < kTabCount; ++t) names.push_back(text::Upper(T(TabName(t))));
    float ux = 0, uw = 0;
    const int clicked = ui::DrawTabs(dl, glyphs_, PromptSet(), names, tab_, ImVec2(x0, y), m_, true, &ux, &uw, x1 - x0);
    if (clicked >= 0 && clicked != tab_) {
      ui::Cue(ui::Sound::kTab);
      tab_ = clicked;
    }
    if (tab_x_ < 0) tab_x_ = ux, tab_w_ = uw;
    tab_x_ = ui::Approach(tab_x_, ux, 22.0f);
    tab_w_ = ui::Approach(tab_w_, uw, 22.0f);
    const float ly = std::round(y + 30 * s);
    dl->AddLine(ImVec2(x0, ly), ImVec2(x1, ly), color::kHairline, 1.0f);
    dl->AddRectFilled(ImVec2(tab_x_, ly - 2 * s), ImVec2(tab_x_ + tab_w_, ly), color::kAccent);
    {
      std::lock_guard lock(g_values_mutex);
      edit_ = g_values;
    }
    items_ = Rows(tab_);
    const float list_top = ly + 16 * s, desc_h = 120 * s;
    ui::ListState& st = lists_[tab_];
    const Item* focused = ui::DrawList(dl, items_, st, ImVec2(x0, list_top), ImVec2(x1, panel1_.y - desc_h - 12 * s), m_,
                                       input_, true);
    dl->AddLine(ImVec2(x0, panel1_.y - desc_h), ImVec2(x1, panel1_.y - desc_h), color::kHairline, 1.0f);
    ui::DrawDescription(dl, focused, ImVec2(x0, panel1_.y - desc_h + 12 * s), ImVec2(x1, panel1_.y - 12 * s), m_);
    focused_ = focused ? *focused : Item{};
    has_focused_ = focused != nullptr;
    {
      std::lock_guard lock(g_values_mutex);
      // Keep what the game set meanwhile (the default field of view).
      if (edit_.default_fov <= 0) edit_.default_fov = g_values.default_fov;
      if (edit_.fov <= 0) edit_.fov = g_values.fov;
      g_values = edit_;
    }
  }

  std::vector<ui::Prompt> Prompts() const {
    if (hidden_) return {{Action::kResetPage, "Show panel"}, {Action::kAccept, "Take picture"}, {Action::kBack, "Back"}};
    std::vector<ui::Prompt> p;
    if (has_focused_ && (focused_.type == ItemType::kChoice || focused_.type == ItemType::kSlider))
      p.push_back({Action::kLeft, "Change"});
    if (has_focused_ && focused_.type == ItemType::kAction) p.push_back({Action::kDefault, "Select"});
    else if (has_focused_ && focused_.on_default) p.push_back({Action::kDefault, "Default"});
    p.push_back({Action::kResetPage, "Hide panel"});
    p.push_back({Action::kAccept, "Take picture"});
    p.push_back({Action::kBack, "Back"});
    return p;
  }

  void DrawFooter(ImDrawList* dl) {
    const float s = m_.s, cy = o_.y + h_ - 38 * s;
    // A soft band under the prompts so they read over any scene.
    const ImU32 c0 = ui::WithAlpha(color::kBase, 0.0f), c1 = ui::WithAlpha(color::kBase, 0.65f);
    dl->AddRectFilledMultiColor(ImVec2(o_.x, cy - 40 * s), ImVec2(o_.x + w_, o_.y + h_), c0, c0, c1, c1);
    if (!hidden_) {
      const std::string hint = T("Move: left stick or W A S D. Look: right stick, or hold the right mouse button. "
                                 "Down and up: triggers, or Ctrl and Space.");
      float x = o_.x + 28 * s;
      // Keyboard and mouse (Beta 12): the mouse's pictures for looking (hold the right button, move the mouse).
      if (PromptSet() == "keyboard") {
        for (const char* name : {key_glyphs::kMouseRight, key_glyphs::kMouseMove}) {
          float aspect = 1.0f;
          if (auto* t = glyphs_.Named("keyboard", name, &aspect)) {
            const float gh = 24 * s, gw = gh * aspect;
            dl->AddImage(ui::Tex(t), ImVec2(x, cy - gh * 0.5f), ImVec2(x + gw, cy + gh * 0.5f));
            x += gw + 4 * s;
          }
        }
        x += 6 * s;
      }
      dl->AddText(ui::GetFonts().text, 14 * s, ImVec2(x, cy - 8 * s), color::kTextDim, hint.c_str());
    }
    const Action clicked = ui::DrawPrompts(dl, glyphs_, PromptSet(), Prompts(), o_.x + w_ - 28 * s, cy, m_, true);
    if (clicked != Action::kCount) input_.Inject(clicked, ui::Device::kKeyboard);
  }

  void DrawSaved(ImDrawList* dl) {
    const double now = ImGui::GetTime();
    if (now > saved_until_) return;
    const float s = m_.s, a = float(std::clamp((saved_until_ - now) / 0.4, 0.0, 1.0));
    const ui::Fonts f = ui::GetFonts();
    const std::string line = saved_text_;
    const ImVec2 size = ui::TextSize(f.semibold, 17 * s, line);
    const ImVec2 p0(o_.x + 28 * s, o_.y + 28 * s), p1(p0.x + size.x + 36 * s, p0.y + size.y + 22 * s);
    dl->AddRectFilled(p0, p1, ui::WithAlpha(color::kBase, 0.88f * a), 10 * s);
    dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + 4 * s, p1.y), ui::WithAlpha(color::kAccent, a), 2 * s);
    dl->AddText(f.semibold, 17 * s, ImVec2(p0.x + 20 * s, p0.y + 11 * s), ui::WithAlpha(color::kText, a), line.c_str());
  }

  // ---------------------------------------------------------- the input ---
  void Handle() {
    if (input_.Pressed(Action::kPrevTab) || input_.Pressed(Action::kNextTab)) {
      if (!hidden_) {
        tab_ = (tab_ + (input_.Pressed(Action::kNextTab) ? 1 : kTabCount - 1)) % kTabCount;
        ui::Cue(ui::Sound::kTab);
        REXLOG_INFO("KK photo mode: panel tab {}", TabName(tab_));
      }
      input_.Consume(Action::kPrevTab);
      input_.Consume(Action::kNextTab);
    }
    if (input_.Pressed(Action::kResetPage)) {  // Y / R: hide or show the panel
      hidden_ = !hidden_;
      ui::Cue(hidden_ ? ui::Sound::kOff : ui::Sound::kOn);
      REXLOG_INFO("KK photo mode: panel {}", hidden_ ? "hidden" : "shown");
      input_.Consume(Action::kResetPage);
    }
    if (activate_ && !hidden_ && focused_.on_activate) {
      ui::Cue(ui::Sound::kSelect);
      edit_ = Snapshot();
      focused_.on_activate();
      std::lock_guard lock(g_values_mutex);
      g_values = edit_;
      REXLOG_INFO("KK photo mode: {} chosen", focused_.label);
    }
    if (take_) TakePicture();
    if (input_.Pressed(Action::kBack)) {
      input_.ConsumeAll();
      ui::Cue(ui::Sound::kBack);
      Exit(false);
      return;
    }
    if (input_.Pressed(Action::kPlay)) {  // Start: straight back to the game
      input_.ConsumeAll();
      ui::Cue(ui::Sound::kBack);
      Exit(true);
    }
  }

 public:
  void TakePicture(const std::string& dev_name = "") {
    std::error_code ec;
    const auto dir = user_dir_ / "screenshots";
    std::filesystem::create_directories(dir, ec);
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    const std::string name = dev_name.empty() ? PictureName(tm, ms) : dev_name + ".png";
    art::CaptureFrame(dir / name);
    ui::Cue(ui::Sound::kSelect);
    saved_text_ = F("Picture saved: {0} (screenshots folder)", {name});
    saved_until_ = ImGui::GetTime() + 3.0;
    REXLOG_INFO("KK photo mode: picture {}", (dir / name).string());
  }

 private:
#if defined(KK_DEV_TOOLS)
  // KK_DEV_PHOTO_STEPS="<s>:<step>,..." on the script clock (seconds since the chapter's level loaded):
  // enter, exit, resume, set:<row>=<value>, tab:<n>, panel:<0|1>, take, take:<name>, shot:<name> (the game's
  // frame into KK_DEV_PHOTO_DIR and "KK dev: window shot <name>" for a window capture with the panel),
  // pad:<a|b|x|y|lb|rb|up|down|left|right|start>, mouse:<dx>/<dy> (a mouse move for the camera), done.
  void DevSteps() {
    static std::vector<std::pair<double, std::string>> steps = [] {
      std::vector<std::pair<double, std::string>> out;
      const char* v = std::getenv("KK_DEV_PHOTO_STEPS");
      std::stringstream all(v ? v : "");
      for (std::string t; std::getline(all, t, ',');) {
        const size_t colon = t.find(':');
        if (colon != std::string::npos) out.push_back({std::atof(t.substr(0, colon).c_str()), t.substr(colon + 1)});
      }
      return out;
    }();
    static size_t next = 0;
    if (next >= steps.size()) return;
    const double now = DevSecondsSinceMenu();
    if (now < 0 || now < steps[next].first) return;
    const std::string w = steps[next++].second;
    REXLOG_INFO("KK photo test: step {:.1f} s {} (active {}, pause menu {})", now, w, g_active.load(), play::PauseMenuOpen());
    if (w == "enter") return Enter();
    if (w == "exit") return Exit(false);
    if (w == "resume") return Exit(true);
    if (w == "take") return TakePicture();
    if (w.rfind("take:", 0) == 0) return TakePicture(w.substr(5));
    if (w.rfind("mouse:", 0) == 0) {  // mouse:<dx>/<dy>, a mouse move for the free camera (Beta 12 check)
      const std::string v = w.substr(6);
      const size_t slash = v.find('/');
      return freecam::DevMouse(std::atof(v.c_str()), slash == std::string::npos ? 0 : std::atof(v.c_str() + slash + 1));
    }
    if (w == "pos") {  // the game's clock, its paused total and the player's position (freeze checks)
      if (uint8_t* b = g_base.load()) {
        auto f32 = [b](uint32_t a) {
          uint32_t v;
          std::memcpy(&v, b + a, 4);
          v = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
          float f;
          std::memcpy(&f, &v, 4);
          return f;
        };
        REXLOG_INFO("KK photo test: clock {:.3f} paused total {:.3f} game time {:.3f} player {:.3f} {:.3f} {:.3f}",
                    f32(0x82CCAE1C), f32(0x82CCC02C), f32(0x82CCAE1C) - f32(0x82CCC02C), f32(0x82D64250),
                    f32(0x82D64254), f32(0x82D64258));
      }
      return;
    }
    if (w == "done") {
      REXLOG_INFO("KK photo test: DONE");
      return;
    }
    if (w.rfind("tab:", 0) == 0) {
      tab_ = std::clamp(std::atoi(w.c_str() + 4), 0, kTabCount - 1);
      return;
    }
    if (w.rfind("panel:", 0) == 0) {
      hidden_ = w[6] == '0';
      return;
    }
    if (w.rfind("shot:", 0) == 0) {
      const char* dir = std::getenv("KK_DEV_PHOTO_DIR");
      art::CaptureFrame(std::filesystem::path(dir && *dir ? dir : ".") / (w.substr(5) + art::kDevCaptureExtension));
      REXLOG_INFO("KK dev: window shot {}", w.substr(5));
      return;
    }
    if (w.rfind("pad:", 0) == 0) {
      static const std::map<std::string, Action> kNames = {
          {"up", Action::kUp},       {"down", Action::kDown},       {"left", Action::kLeft},
          {"right", Action::kRight}, {"a", Action::kAccept},        {"b", Action::kBack},
          {"x", Action::kDefault},   {"y", Action::kResetPage},     {"lb", Action::kPrevTab},
          {"rb", Action::kNextTab},  {"start", Action::kPlay}};
      if (auto it = kNames.find(w.substr(4)); it != kNames.end()) dev_inject_.push_back(it->second);
      return;
    }
    if (w.rfind("set:", 0) == 0) {
      const size_t eq = w.find('=');
      if (eq == std::string::npos) return;
      const std::string name = w.substr(4, eq - 4);
      const float x = float(std::atof(w.c_str() + eq + 1));
      std::lock_guard lock(g_values_mutex);
      Values& v = g_values;
      const std::map<std::string, float*> floats = {
          {"fov", &v.fov},           {"roll", &v.roll},         {"speed", &v.speed},
          {"strength", &v.look_strength}, {"exposure", &v.exposure}, {"contrast", &v.contrast},
          {"saturation", &v.saturation}, {"temperature", &v.temperature}, {"dof", &v.dof},
          {"focus", &v.focus},       {"range", &v.focus_range}, {"vignette", &v.vignette},
          {"grain", &v.grain},       {"brightness", &v.brightness}, {"gamma", &v.gamma}};
      const std::map<std::string, int*> ints = {
          {"look", &v.look}, {"letterbox", &v.letterbox}, {"cel", &v.cel}, {"sharpness", &v.sharpness}};
      if (auto it = floats.find(name); it != floats.end()) *it->second = x;
      else if (auto jt = ints.find(name); jt != ints.end()) *jt->second = int(x);
      else if (name == "hide") v.hide_arms = x != 0;
      else if (name == "flip") v.flip = x != 0;
      else REXLOG_WARN("KK photo test: unknown setting {}", name);
      return;
    }
    REXLOG_WARN("KK photo test: unknown step {}", w);
  }
  std::vector<Action> dev_inject_;
#endif

  std::filesystem::path user_dir_;
  ui::Input input_;
  ui::Glyphs glyphs_;
  ui::Metrics m_;
  ImVec2 o_{0, 0}, panel0_{0, 0}, panel1_{0, 0};
  float w_ = 1280, h_ = 720;
  bool open_ = false, hidden_ = false, looking_ = false;
  int opened_frame_ = -10;
  int tab_ = kCamera;
  float tab_x_ = -1, tab_w_ = 0;
  std::map<int, ui::ListState> lists_;
  std::vector<Item> items_;
  Item focused_;
  bool has_focused_ = false;
  bool take_ = false, activate_ = false;
  Values edit_;
  static Values Snapshot() {
    std::lock_guard lock(g_values_mutex);
    return g_values;
  }
  std::string saved_text_;
  double saved_until_ = 0;
};

Panel* g_panel = nullptr;

}  // namespace

void CreatePanel(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                 const std::filesystem::path& user_dir) {
  if (g_panel || !drawer) return;
  g_panel = new Panel(drawer, immediate, user_dir);  // lives for the session
}

}  // namespace kk::photo
