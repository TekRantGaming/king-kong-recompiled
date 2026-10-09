#include "launcher_ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#include <SDL3/SDL.h>

#include <rex/ui/immediate_drawer.h>

#include "art.h"
#include "launcher_text.h"
#include "platform.h"
#include "toast.h"

namespace kk::ui {

using text::T;

namespace {

constexpr double kRepeatDelay = 0.34, kRepeatRate = 0.075;
// A controller press and a key press of the same action this close together
// are one press (Steam's desktop layout types keys for the controller).
constexpr double kSameInput = 0.25;

int Utf8Length(unsigned char c) {
  return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
}

bool MouseIn(ImVec2 a, ImVec2 b) { return ImGui::IsMouseHoveringRect(a, b, false); }

// Triangles in ImGui's winding (clockwise on screen).
void ArrowLeft(ImDrawList* dl, float x, float cy, float w, float h, ImU32 c) {
  dl->AddTriangleFilled(ImVec2(x, cy), ImVec2(x + w, cy - h * 0.5f), ImVec2(x + w, cy + h * 0.5f), c);
}
void ArrowRight(ImDrawList* dl, float x, float cy, float w, float h, ImU32 c) {
  dl->AddTriangleFilled(ImVec2(x, cy - h * 0.5f), ImVec2(x + w, cy), ImVec2(x, cy + h * 0.5f), c);
}

std::function<void(Sound, float)> g_sound;

}  // namespace

void SetSoundHandler(std::function<void(Sound, float)> handler) { g_sound = std::move(handler); }
void Cue(Sound sound, float value) {
  if (g_sound) g_sound(sound, value);
}

ImU32 WithAlpha(ImU32 c, float alpha) {
  const float a = float((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
  return (c & ~IM_COL32_A_MASK) | (ImU32(a + 0.5f) << IM_COL32_A_SHIFT);
}

ImTextureRef Tex(rex::ui::ImmediateTexture* t) { return ImTextureRef(reinterpret_cast<ImTextureID>(t)); }

Fonts GetFonts() {
  const UiFonts& f = GetUiFonts();
  Fonts out;
  out.text = f.regular ? f.regular : ImGui::GetFont();
  out.semibold = f.semibold ? f.semibold : out.text;
  out.display = f.display ? f.display : (f.bold ? f.bold : out.text);
  return out;
}

ImVec2 TextSize(ImFont* font, float size, const std::string& text, float wrap) {
  return font->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str(), text.c_str() + text.size());
}

float TrackedWidth(ImFont* font, float size, const std::string& text, float tracking) {
  float w = 0;
  int n = 0;
  for (size_t i = 0; i < text.size();) {
    const size_t len = std::min<size_t>(Utf8Length(uint8_t(text[i])), text.size() - i);
    w += font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str() + i, text.c_str() + i + len).x;
    i += len;
    ++n;
  }
  return n ? w + tracking * float(n - 1) : 0.0f;
}

void DrawTracked(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const std::string& text,
                 float tracking) {
  float x = pos.x;
  for (size_t i = 0; i < text.size();) {
    const size_t len = std::min<size_t>(Utf8Length(uint8_t(text[i])), text.size() - i);
    const char* s = text.c_str() + i;
    dl->AddText(font, size, ImVec2(std::round(x), pos.y), col, s, s + len);
    x += font->CalcTextSizeA(size, FLT_MAX, 0, s, s + len).x + tracking;
    i += len;
  }
}

std::string Ellipsize(ImFont* font, float size, const std::string& text, float width) {
  if (width <= 0) return {};
  if (TextSize(font, size, text).x <= width) return text;
  static const std::string kEllipsis = "\xE2\x80\xA6";
  std::string out = text;
  while (!out.empty()) {
    // Drop the last whole character.
    size_t cut = out.size() - 1;
    while (cut > 0 && (uint8_t(out[cut]) & 0xC0) == 0x80) --cut;
    out.erase(cut);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (TextSize(font, size, out + kEllipsis).x <= width) return out + kEllipsis;
  }
  return kEllipsis;
}

float Approach(float v, float target, float rate) {
  const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
  v += (target - v) * (1.0f - std::exp(-rate * dt));
  return std::abs(target - v) < 0.3f ? target : v;
}

// ------------------------------------------------------------------ input ---
// Controller buttons the launcher uses, in buttons_down_.
enum PadButton { kPadA, kPadB, kPadX, kPadY, kPadLB, kPadRB, kPadStart, kPadBack };

Input::Input() {
  // The program's own SDL (the game's input runs in the runtime's): only
  // while the launcher is open. Gamepads already connected arrive as events.
  sdl_ready_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  pad_held_at_.fill(-1e9);
  key_pressed_at_.fill(-1e9);
}

Input::~Input() { Release(); }

void Input::Release() {
  for (void* p : pads_) SDL_CloseGamepad(static_cast<SDL_Gamepad*>(p));
  pads_.clear();
  if (sdl_ready_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  sdl_ready_ = false;
}

bool Input::RepeatHeld(Repeat& r, bool down, double now) {
  if (!down) {
    r.held = false;
    return false;
  }
  if (!r.held) {
    r.held = true;
    r.since = now;
    r.next = now + kRepeatDelay;
    return true;
  }
  if (now >= r.next) {
    r.next = now + kRepeatRate;
    return true;
  }
  return false;
}

float Input::HeldFor(Action a) const {
  const size_t i = size_t(a);
  return i < held_.size() ? held_[i] : 0.0f;
}

void Input::Inject(Action a, Device d) {
  pressed_[size_t(a)] = true;
  device_ = d;
  if (d != Device::kKeyboard) pad_device_ = d;
}

void Input::UpdateGamepads(double now) {
  if (!sdl_ready_) return;
  SDL_Event e;
#if defined(_WIN32)
  // The program's own SDL (the runtime DLL has its own): everything queued is ours.
  auto next = [&e] { return SDL_PollEvent(&e); };
#else
  // Elsewhere the program and the runtime share one SDL, whose queue also holds
  // the window's events (its paint requests, keys and the mouse): take only the
  // controllers' and leave the rest for the window.
  SDL_PumpEvents();
  auto next = [&e] {
    return SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1) > 0;
  };
#endif
  while (next()) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
      if (SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which)) pads_.push_back(g);
    } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
      for (auto it = pads_.begin(); it != pads_.end(); ++it) {
        auto* g = static_cast<SDL_Gamepad*>(*it);
        if (SDL_GetGamepadID(g) == e.gdevice.which) {
          SDL_CloseGamepad(g);
          pads_.erase(it);
          break;
        }
      }
    }
  }
  std::array<bool, 8> down{};
  bool dir[6] = {};
  for (void* p : pads_) {
    auto* g = static_cast<SDL_Gamepad*>(p);
    auto b = [g](SDL_GamepadButton button) { return SDL_GetGamepadButton(g, button); };
    const float lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    const float ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    const float lt = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
    const float rt = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
    // A stick direction counts once it's well past the middle, and keeps
    // counting until it's nearly back (no flicker at the edge).
    auto past = [](float v, bool was) { return v > (was ? 0.35f : 0.55f); };
    const bool pad_dir[6] = {
        b(SDL_GAMEPAD_BUTTON_DPAD_UP) || past(-ly, dir_[0].held),
        b(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || past(ly, dir_[1].held),
        b(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || past(-lx, dir_[2].held),
        b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || past(lx, dir_[3].held),
        lt > 0.5f,
        rt > 0.5f,
    };
    const bool pad_down[8] = {b(SDL_GAMEPAD_BUTTON_SOUTH),         b(SDL_GAMEPAD_BUTTON_EAST),
                              b(SDL_GAMEPAD_BUTTON_WEST),          b(SDL_GAMEPAD_BUTTON_NORTH),
                              b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
                              b(SDL_GAMEPAD_BUTTON_START),         b(SDL_GAMEPAD_BUTTON_BACK)};
    bool any = false;
    for (int i = 0; i < 6; ++i) {
      dir[i] = dir[i] || pad_dir[i];
      any = any || pad_dir[i];
    }
    for (int i = 0; i < 8; ++i) {
      down[size_t(i)] = down[size_t(i)] || pad_down[i];
      any = any || (pad_down[i] && !buttons_down_[size_t(i)]);
    }
    if (any) {
      const SDL_GamepadType type = SDL_GetGamepadType(g);
      const bool ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
      pad_device_ = device_ = ps ? Device::kPlayStation : Device::kXbox;
    }
  }
  // A press the keyboard just reported (typed by Steam for this controller) already counted.
  auto press = [&](Action a) {
    if (now - key_pressed_at_[size_t(a)] >= kSameInput) pressed_[size_t(a)] = true;
  };
  static constexpr Action kDirActions[6] = {Action::kUp,   Action::kDown,   Action::kLeft,
                                            Action::kRight, Action::kPageUp, Action::kPageDown};
  for (size_t i = 0; i < 6; ++i) {
    if (RepeatHeld(dir_[i], dir[i], now)) press(kDirActions[i]);
    if (dir[i]) {
      held_[size_t(kDirActions[i])] = float(now - dir_[i].since);
      pad_held_at_[size_t(kDirActions[i])] = now;
    }
  }
  static constexpr Action kButtonActions[8] = {Action::kAccept,  Action::kBack,    Action::kDefault,
                                               Action::kResetPage, Action::kPrevTab, Action::kNextTab,
                                               Action::kPlay,    Action::kCount};
  for (size_t i = 0; i < 8; ++i) {
    if (!down[i] || kButtonActions[i] == Action::kCount) continue;
    if (!buttons_down_[i]) press(kButtonActions[i]);
    pad_held_at_[size_t(kButtonActions[i])] = now;
  }
  buttons_down_ = down;
}

void Input::Update() {
  pressed_ = {};
  held_ = {};
  const double now = ImGui::GetTime();
  const ImGuiIO& io = ImGui::GetIO();

  // The mouse: moving it or clicking switches the prompts to the keyboard's.
  mouse_moved_ = false;
  if (ImGui::IsMousePosValid(&io.MousePos)) {
    if (last_mouse_.x >= 0 &&
        (std::abs(io.MousePos.x - last_mouse_.x) > 1.5f || std::abs(io.MousePos.y - last_mouse_.y) > 1.5f)) {
      mouse_moved_ = true;
      device_ = Device::kKeyboard;
    }
    last_mouse_ = io.MousePos;
  }
  if (ImGui::IsMouseClicked(0) || io.MouseWheel != 0) device_ = Device::kKeyboard;

  UpdateGamepads(now);  // first: a key typed for a held controller button is skipped
  // A key for an action a controller is holding (or just let go of) is that controller's press.
  auto from_pad = [&](Action a) { return now - pad_held_at_[size_t(a)] < kSameInput; };
  if (keyboard_enabled_) {
    struct Key {
      ImGuiKey key;
      Action action;
      bool repeat;
    };
    static const Key kKeys[] = {
        {ImGuiKey_UpArrow, Action::kUp, true},          {ImGuiKey_DownArrow, Action::kDown, true},
        {ImGuiKey_LeftArrow, Action::kLeft, true},      {ImGuiKey_RightArrow, Action::kRight, true},
        {ImGuiKey_PageUp, Action::kPageUp, true},       {ImGuiKey_PageDown, Action::kPageDown, true},
        {ImGuiKey_Enter, Action::kAccept, false},       {ImGuiKey_KeypadEnter, Action::kAccept, false},
        {ImGuiKey_Space, Action::kAccept, false},       {ImGuiKey_Escape, Action::kBack, false},
        {ImGuiKey_Backspace, Action::kBack, false},     {ImGuiKey_Delete, Action::kDefault, false},
        {ImGuiKey_R, Action::kResetPage, false},        {ImGuiKey_Q, Action::kPrevTab, true},
        {ImGuiKey_E, Action::kNextTab, true},           {ImGuiKey_P, Action::kPlay, false},
    };
    const bool ctrl_or_alt = io.KeyCtrl || io.KeyAlt;
    for (const Key& k : kKeys) {
      if (ctrl_or_alt || !ImGui::IsKeyPressed(k.key, k.repeat) || from_pad(k.action)) continue;
      pressed_[size_t(k.action)] = true;
      key_pressed_at_[size_t(k.action)] = now;
      device_ = Device::kKeyboard;
    }
    const Action tab = io.KeyShift ? Action::kPrevTab : Action::kNextTab;
    if (!ctrl_or_alt && ImGui::IsKeyPressed(ImGuiKey_Tab, true) && !from_pad(tab)) {
      pressed_[size_t(tab)] = true;
      key_pressed_at_[size_t(tab)] = now;
      device_ = Device::kKeyboard;
    }
    static constexpr std::pair<ImGuiKey, Action> kHeld[] = {{ImGuiKey_UpArrow, Action::kUp},
                                                            {ImGuiKey_DownArrow, Action::kDown},
                                                            {ImGuiKey_LeftArrow, Action::kLeft},
                                                            {ImGuiKey_RightArrow, Action::kRight}};
    for (size_t i = 0; i < std::size(kHeld); ++i) {
      key_held_[i] = ImGui::IsKeyDown(kHeld[i].first) ? key_held_[i] + io.DeltaTime : 0.0f;
      held_[size_t(kHeld[i].second)] = std::max(held_[size_t(kHeld[i].second)], key_held_[i]);
    }
  }
}

// --------------------------------------------------------------- prompts ---
Glyphs::Glyphs(rex::ui::ImmediateDrawer* drawer, std::filesystem::path dir)
    : drawer_(drawer), dir_(std::move(dir)) {}

Glyphs::~Glyphs() { Reload(); }

void Glyphs::AddSet(const std::string& set, std::filesystem::path dir) { set_dirs_[set] = std::move(dir); }

void Glyphs::Reload() {
  for (auto& [name, entry] : cache_)
    if (entry.texture) KeepTextureAlive(std::move(entry.texture));
  cache_.clear();
}

rex::ui::ImmediateTexture* Glyphs::Named(const std::string& set, const std::string& name, float* aspect) {
  const std::string key = set + "/" + name;
  auto it = cache_.find(key);
  if (it == cache_.end()) {
    Entry entry;
    const auto custom = set_dirs_.find(set);
    const auto path = custom != set_dirs_.end() ? custom->second / (name + ".bmp") : dir_ / set / (name + ".png");
    if (const auto img = art::LoadImage(path); img && drawer_) {
      entry.texture = drawer_->CreateTexture(uint32_t(img.width), uint32_t(img.height),
                                             rex::ui::ImmediateTextureFilter::kLinear, false, img.rgba.data());
      entry.aspect = float(img.width) / float(img.height);
    }
    it = cache_.emplace(key, std::move(entry)).first;
  }
  if (aspect) *aspect = it->second.aspect;
  return it->second.texture.get();
}

rex::ui::ImmediateTexture* Glyphs::Get(Action a, const std::string& set, float* aspect) {
  if (set == "keyboard") {
    const char* name = nullptr;
    switch (a) {
      case Action::kUp: name = "Arrow_Up"; break;
      case Action::kDown: name = "Arrow_Down"; break;
      case Action::kLeft: name = "Arrow_Left"; break;
      case Action::kRight: name = "Arrow_Right"; break;
      case Action::kPageUp: name = "Page_Up"; break;
      case Action::kPageDown: name = "Page_Down"; break;
      case Action::kAccept: name = "Enter"; break;
      case Action::kBack: name = "Esc"; break;
      case Action::kDefault: name = "Del"; break;
      case Action::kResetPage: name = "R"; break;
      case Action::kPrevTab: name = "Q"; break;
      case Action::kNextTab: name = "E"; break;
      case Action::kPlay: name = "P"; break;
      default: return nullptr;
    }
    return Named("keyboard", name, aspect);
  }
  const char* name = nullptr;
  switch (a) {
    case Action::kUp: name = "dpad_up"; break;
    case Action::kDown:
    case Action::kLeft:
    case Action::kRight: name = "dpad"; break;
    case Action::kPageUp: name = "lt"; break;
    case Action::kPageDown: name = "rt"; break;
    case Action::kAccept: name = "a"; break;
    case Action::kBack: name = "b"; break;
    case Action::kDefault: name = "x"; break;
    case Action::kResetPage: name = "y"; break;
    case Action::kPrevTab: name = "lb"; break;
    case Action::kNextTab: name = "rb"; break;
    case Action::kPlay: name = "start"; break;
    default: return nullptr;
  }
  rex::ui::ImmediateTexture* t = Named(set, name, aspect);
  return t || set == "xbox_series" ? t : Named("xbox_series", name, aspect);
}

namespace {

// One button picture `h` tall at (x, cy); a key name in a box if the picture
// is missing. Returns its width.
float DrawGlyph(ImDrawList* dl, Glyphs& glyphs, Action a, const std::string& set, float x, float cy, float h,
                const Metrics& m, bool draw) {
  float aspect = 1.0f;
  if (auto* t = glyphs.Get(a, set, &aspect)) {
    const float w = h * aspect;
    if (draw) dl->AddImage(Tex(t), ImVec2(x, cy - h * 0.5f), ImVec2(x + w, cy + h * 0.5f));
    return w;
  }
  static const char* const kNames[] = {"Up", "Down", "Left", "Right", "PgUp", "PgDn", "Enter",
                                       "Esc", "Del", "R", "Q", "E", "P"};
  const Fonts f = GetFonts();
  const char* name = size_t(a) < std::size(kNames) ? kNames[size_t(a)] : "?";
  const float fs = h * 0.5f;
  const float w = TextSize(f.semibold, fs, name).x + 12 * m.s;
  if (draw) {
    dl->AddRect(ImVec2(x, cy - h * 0.42f), ImVec2(x + w, cy + h * 0.42f), WithAlpha(color::kText, 0.5f), 4 * m.s);
    dl->AddText(f.semibold, fs, ImVec2(x + 6 * m.s, cy - fs * 0.5f), color::kText, name);
  }
  return w;
}

// A prompt's pictures: left/right and up/down are two keys on a keyboard.
float DrawPromptGlyphs(ImDrawList* dl, Glyphs& glyphs, Action a, const std::string& set, float x, float cy, float h,
                       const Metrics& m, bool draw) {
  if (set == "keyboard" && (a == Action::kLeft || a == Action::kUp)) {
    const Action second = a == Action::kLeft ? Action::kRight : Action::kDown;
    const float w1 = DrawGlyph(dl, glyphs, a, set, x, cy, h, m, draw);
    return w1 + 2 * m.s + DrawGlyph(dl, glyphs, second, set, x + w1 + 2 * m.s, cy, h, m, draw);
  }
  return DrawGlyph(dl, glyphs, a, set, x, cy, h, m, draw);
}

}  // namespace

Action DrawPrompts(ImDrawList* dl, Glyphs& glyphs, const std::string& set, const std::vector<Prompt>& prompts, float right,
                   float center_y, const Metrics& m, bool clickable) {
  const Fonts f = GetFonts();
  const float s = m.s, gh = 28 * s, fs = 15 * s, gap = 8 * s, between = 26 * s;
  std::vector<float> widths;
  float total = 0;
  for (const Prompt& p : prompts) {
    const float w =
        DrawPromptGlyphs(dl, glyphs, p.action, set, 0, 0, gh, m, false) + gap + TextSize(f.text, fs, T(p.label)).x;
    widths.push_back(w);
    total += w;
  }
  total += between * float(prompts.empty() ? 0 : prompts.size() - 1);
  float x = right - total;
  Action clicked = Action::kCount;
  for (size_t i = 0; i < prompts.size(); ++i) {
    const Prompt& p = prompts[i];
    const ImVec2 a(x - 6 * s, center_y - gh * 0.5f - 4 * s), b(x + widths[i] + 6 * s, center_y + gh * 0.5f + 4 * s);
    const bool hot = clickable && MouseIn(a, b);
    const float gw = DrawPromptGlyphs(dl, glyphs, p.action, set, x, center_y, gh, m, true);
    dl->AddText(f.text, fs, ImVec2(x + gw + gap, center_y - fs * 0.5f - 1 * s), hot ? color::kText : color::kTextDim,
                T(p.label.c_str()));
    if (hot && ImGui::IsMouseClicked(0)) clicked = p.action;
    x += widths[i] + between;
  }
  return clicked;
}

int DrawTabs(ImDrawList* dl, Glyphs& glyphs, const std::string& set, const std::vector<std::string>& names, int current,
             ImVec2 pos, const Metrics& m, bool clickable, float* underline_x, float* underline_w) {
  const Fonts f = GetFonts();
  const float s = m.s, fs = 15 * s, tracking = 1.6f * s, gap = 30 * s, gh = 24 * s;
  float x = pos.x;
  const float cy = pos.y;
  x += DrawGlyph(dl, glyphs, Action::kPrevTab, set, x, cy, gh, m, true) + 20 * s;
  int clicked = -1;
  for (size_t i = 0; i < names.size(); ++i) {
    const float w = TrackedWidth(f.display, fs, names[i], tracking);
    const ImVec2 a(x - gap * 0.5f, cy - 22 * s), b(x + w + gap * 0.5f, cy + 22 * s);
    const bool hot = clickable && MouseIn(a, b);
    const bool on = int(i) == current;
    DrawTracked(dl, f.display, fs, ImVec2(x, cy - fs * 0.5f), on ? color::kText : hot ? color::kTextDim : color::kTextFaint,
                names[i], tracking);
    if (on) {
      *underline_x = x;
      *underline_w = w;
    }
    if (hot && ImGui::IsMouseClicked(0)) clicked = int(i);
    x += w + gap;
  }
  DrawGlyph(dl, glyphs, Action::kNextTab, set, x - gap + 20 * s, cy, gh, m, true);
  return clicked;
}

// ------------------------------------------------------------------ items ---
namespace {

struct RowGeom {
  float value_l = 0, value_r = 0;  // value text
  float bar_l = 0, bar_r = 0;      // slider bar
  float chevron_l = 0;             // left chevron's x
};

constexpr float kChevronW = 6, kChevronGap = 12;

// The value a row shows, in the launcher's language.
std::string ValueText(const Item& it) {
  if (it.type == ItemType::kChoice && it.index >= 0 && it.index < int(it.options.size()))
    return T(it.options[size_t(it.index)]);
  return T(it.value_text);
}

RowGeom Geometry(const Item& it, float x0, float x1, const Metrics& m, const Fonts& f) {
  const float s = m.s;
  RowGeom g;
  const float right = x1 - 18 * s;
  g.value_r = right - (kChevronW + kChevronGap) * s;
  if (it.type == ItemType::kChoice) {
    g.value_l = g.value_r - TextSize(f.semibold, 16 * s, ValueText(it)).x;
    g.chevron_l = g.value_l - (kChevronGap + kChevronW) * s;
  } else if (it.type == ItemType::kSlider) {
    g.value_l = g.value_r - 52 * s;
    g.bar_r = g.value_l - 16 * s;
    g.bar_l = g.bar_r - std::min(190 * s, (x1 - x0) * 0.32f);
  } else {
    // In line with the choices' values; a link's arrow sits where their right arrow does.
    g.value_l = g.value_r - TextSize(f.semibold, 16 * s, ValueText(it)).x;
  }
  return g;
}

void DrawItem(ImDrawList* dl, const Item& it, float x0, float y0, float x1, float h, bool band, bool focused,
              const Metrics& m, const Fonts& f) {
  const float s = m.s;
  if (it.type == ItemType::kSection) {
    const float fs = 13 * s, ty = y0 + h - 16 * s - fs;
    const std::string caption = text::Upper(T(it.label));
    const float w = TrackedWidth(f.display, fs, caption, 2.2f * s);
    DrawTracked(dl, f.display, fs, ImVec2(x0 + 2 * s, ty), color::kAccent, caption, 2.2f * s);
    dl->AddLine(ImVec2(x0 + w + 16 * s, std::round(ty + fs * 0.55f)), ImVec2(x1, std::round(ty + fs * 0.55f)),
                color::kHairline, 1.0f);
    return;
  }
  const ImU32 text = band ? (it.disabled ? color::kOnFocusDim : color::kOnFocus)
                          : (it.disabled ? color::kTextFaint : color::kText);
  const ImU32 dim = band ? color::kOnFocusDim : (it.disabled ? color::kTextFaint : color::kTextDim);
  const float cy = y0 + h * 0.5f;
  float lx = x0 + 18 * s;
  if (it.icon) {
    // Small icons sit centred in a column of their own, so the labels line up.
    const float ih = it.tall ? h - 22 * s : 26 * s, iw = ih * it.icon_aspect;
    const float slot = it.tall ? iw : std::max(iw, ih * 1.5f);
    const float ix = lx + (slot - iw) * 0.5f;
    dl->AddImage(Tex(it.icon), ImVec2(ix, cy - ih * 0.5f), ImVec2(ix + iw, cy + ih * 0.5f), ImVec2(0, 0),
                 ImVec2(1, 1), it.icon_dim ? IM_COL32(120, 124, 132, 190) : IM_COL32_WHITE);
    lx += slot + (it.tall ? 16 : 12) * s;
  }
  const RowGeom g = Geometry(it, x0, x1, m, f);
  const float label_room = (it.type == ItemType::kSlider ? g.bar_l : g.value_l) - 24 * s - lx;
  if (it.tall) {
    dl->AddText(f.semibold, 16.5f * s, ImVec2(lx, cy - 20 * s), text,
                Ellipsize(f.semibold, 16.5f * s, T(it.label), label_room).c_str());
    const std::string sub = T(it.subtitle.empty() ? it.description : it.subtitle);
    dl->AddText(f.text, 14.5f * s, ImVec2(lx, cy + 2 * s), dim, Ellipsize(f.text, 14.5f * s, sub, label_room).c_str());
  } else {
    dl->AddText(f.text, 17 * s, ImVec2(lx, cy - 17 * s * 0.5f - 1 * s), text,
                Ellipsize(f.text, 17 * s, T(it.label), label_room).c_str());
  }

  const float vs = 16 * s;
  const ImU32 value_col = band ? text : it.disabled ? color::kTextFaint : it.value_color ? it.value_color : color::kTextDim;
  switch (it.type) {
    case ItemType::kChoice: {
      const bool listed = it.index >= 0 && it.index < int(it.options.size());
      const std::string v = ValueText(it);
      const int n = int(it.options.size());
      const bool pips = n >= 2 && n <= 7 && !it.disabled;
      const float ty = cy - vs * 0.5f - (pips ? 4 * s : 1 * s);
      dl->AddText(f.semibold, vs, ImVec2(g.value_l, ty), value_col, v.c_str());
      if (pips) {
        const float pw = 9 * s, pg = 3 * s, total = pw * n + pg * (n - 1);
        float px = (g.value_l + g.value_r) * 0.5f - total * 0.5f;
        px = std::min(px, g.value_r - total);
        const float py = ty + vs + 5 * s;
        for (int i = 0; i < n; ++i, px += pw + pg)
          dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + 2 * s),
                            i == it.index ? value_col : WithAlpha(value_col, 0.28f));
      }
      if (focused && band && !it.disabled) {
        const ImU32 c = color::kOnFocus;
        const float aw = kChevronW * s, ah = 10 * s;
        ArrowLeft(dl, g.chevron_l, ty + vs * 0.5f + 1 * s, aw, ah,
                  it.index <= 0 && listed ? WithAlpha(c, 0.25f) : c);
        ArrowRight(dl, g.value_r + kChevronGap * s, ty + vs * 0.5f + 1 * s, aw, ah,
                   it.index >= n - 1 && listed ? WithAlpha(c, 0.25f) : c);
      }
      break;
    }
    case ItemType::kSlider: {
      const float t = it.hi > it.lo ? std::clamp((it.value - it.lo) / (it.hi - it.lo), 0.0f, 1.0f) : 0.0f;
      const float by = std::round(cy);
      const ImU32 track = band ? WithAlpha(color::kOnFocus, 0.18f) : WithAlpha(color::kText, 0.16f);
      const ImU32 fill = band ? color::kOnFocus : it.disabled ? color::kTextFaint : color::kTextDim;
      dl->AddRectFilled(ImVec2(g.bar_l, by - 1 * s), ImVec2(g.bar_r, by + 1 * s), track);
      const float kx = g.bar_l + (g.bar_r - g.bar_l) * t;
      dl->AddRectFilled(ImVec2(g.bar_l, by - 1 * s), ImVec2(kx, by + 1 * s), fill);
      dl->AddRectFilled(ImVec2(kx - 2 * s, by - 8 * s), ImVec2(kx + 2 * s, by + 8 * s), fill);
      const float tw = TextSize(f.semibold, vs, it.value_text).x;
      dl->AddText(f.semibold, vs, ImVec2(g.value_r - tw, cy - vs * 0.5f - 1 * s), value_col, it.value_text.c_str());
      break;
    }
    case ItemType::kLink:
      ArrowRight(dl, x1 - 18 * s - kChevronW * s, cy, kChevronW * s, 10 * s, band ? text : dim);
      [[fallthrough]];
    case ItemType::kAction:
    case ItemType::kInfo:
      if (!it.value_text.empty())
        dl->AddText(f.semibold, vs, ImVec2(g.value_l, cy - vs * 0.5f - 1 * s), value_col, ValueText(it).c_str());
      break;
    default:
      break;
  }
}

}  // namespace

const Item* DrawList(ImDrawList* dl, std::vector<Item>& items, ListState& st, ImVec2 p0, ImVec2 p1,
                     const Metrics& m, Input& in, bool active) {
  const float s = m.s;
  const Fonts f = GetFonts();
  const float view_h = p1.y - p0.y;
  const int count = int(items.size());

  std::vector<float> top(items.size()), height(items.size());
  float total = 0;
  for (int i = 0; i < count; ++i) {
    const Item& it = items[size_t(i)];
    const float h = it.type == ItemType::kSection ? (i == 0 ? m.section_h - 16 * s : m.section_h)
                    : it.tall                     ? m.tall_h
                                                  : m.row_h;
    top[size_t(i)] = total;
    height[size_t(i)] = h;
    total += h;
  }
  auto focusable = [&](int i) { return i >= 0 && i < count && items[size_t(i)].type != ItemType::kSection; };
  if (!st.focus_label.empty() && (!focusable(st.focus) || items[size_t(st.focus)].label != st.focus_label)) {
    for (int i = 0; i < count; ++i) {
      if (focusable(i) && items[size_t(i)].label == st.focus_label) {
        st.focus = i;
        break;
      }
    }
  }
  if (!focusable(st.focus)) {
    int found = -1;
    for (int i = std::max(0, st.focus); i < count && found < 0; ++i)
      if (focusable(i)) found = i;
    for (int i = std::min(st.focus, count - 1); i >= 0 && found < 0; --i)
      if (focusable(i)) found = i;
    st.focus = found;
    st.band_y = -1;
  }
  int first_focusable = -1;
  for (int i = 0; i < count && first_focusable < 0; ++i)
    if (focusable(i)) first_focusable = i;

  // Changes go through these, with their sounds: a two-way choice sounds on
  // or off, a slider's pitch follows its value.
  const int focus_before = st.focus;
  // (The sound after the change, so the sound settings are heard as set.)
  auto choose = [](Item& it, int to) {
    if (to == it.index || !it.on_choice) return;
    const bool two_way = it.options.size() == 2;
    it.on_choice(to);
    Cue(two_way ? (to == 1 ? Sound::kOn : Sound::kOff) : Sound::kChange);
  };
  auto set_value = [](Item& it, float v) {
    if (v == it.value || !it.on_value) return;
    const float at = (v - it.lo) / std::max(1e-6f, it.hi - it.lo);
    it.on_value(v);
    Cue(Sound::kSlide, at);
  };
  auto activate = [](Item& it) {
    Cue(Sound::kSelect);
    it.on_activate();
  };

  bool keep_visible = false;
  const ImGuiIO& io = ImGui::GetIO();
  if (active && st.focus >= 0) {
    auto step = [&](int dir) {
      for (int i = st.focus + dir; i >= 0 && i < count; i += dir) {
        if (focusable(i)) {
          st.focus = i;
          break;
        }
      }
      keep_visible = true;
    };
    if (in.Pressed(Action::kUp)) step(-1);
    if (in.Pressed(Action::kDown)) step(+1);
    if (in.Pressed(Action::kPageUp) || in.Pressed(Action::kPageDown)) {
      const int dir = in.Pressed(Action::kPageDown) ? 1 : -1;
      const float goal = top[size_t(st.focus)] + dir * (view_h - m.row_h * 1.5f);
      for (int i = st.focus + dir; i >= 0 && i < count; i += dir) {
        if (!focusable(i)) continue;
        st.focus = i;
        if (dir < 0 ? top[size_t(i)] <= goal : top[size_t(i)] >= goal) break;
      }
      keep_visible = true;
    }
    for (Action a : {Action::kUp, Action::kDown, Action::kPageUp, Action::kPageDown}) in.Consume(a);

    Item& it = items[size_t(st.focus)];
    const bool can_change = !it.disabled;
    if (in.Pressed(Action::kLeft) || in.Pressed(Action::kRight)) {
      const int dir = in.Pressed(Action::kRight) ? 1 : -1;
      const int n = int(it.options.size());
      if (it.type == ItemType::kChoice && can_change && n > 0) {
        choose(it, it.index < 0 ? (dir > 0 ? 0 : n - 1) : std::clamp(it.index + dir, 0, n - 1));
      } else if (it.type == ItemType::kSlider && can_change) {
        // Held: faster after a moment.
        const float held = in.HeldFor(dir > 0 ? Action::kRight : Action::kLeft);
        const float mult = held > 2.0f ? 10.0f : held > 0.9f ? 4.0f : 1.0f;
        set_value(it, std::clamp(it.value + dir * it.step * mult, it.lo, it.hi));
      } else if (it.disabled && (it.type == ItemType::kChoice || it.type == ItemType::kSlider)) {
        Cue(Sound::kDeny);
      }
      in.Consume(Action::kLeft);
      in.Consume(Action::kRight);
    }
    if (in.Pressed(Action::kAccept)) {
      if (it.type == ItemType::kChoice && !it.options.empty()) {
        if (can_change) choose(it, (it.index + 1) % int(it.options.size()));
        else Cue(Sound::kDeny);
      } else if (it.on_activate) {
        if (can_change) activate(it);
        else Cue(Sound::kDeny);
      }
      in.Consume(Action::kAccept);
    }
    if (in.Pressed(Action::kDefault) && it.on_default) {
      if (can_change) {
        Cue(Sound::kChange);
        it.on_default();
      } else {
        Cue(Sound::kDeny);
      }
      in.Consume(Action::kDefault);
    }
  }

  // The mouse: hovering focuses (once it moves), clicks act, the wheel scrolls.
  if (st.dragging >= 0 && (!ImGui::IsMouseDown(0) || !focusable(st.dragging))) st.dragging = -1;
  const bool hover_list = active && MouseIn(p0, p1);
  if (hover_list && io.MouseWheel != 0) st.scroll_target -= io.MouseWheel * m.row_h * 2.5f;
  if (hover_list && st.dragging < 0) {
    const float my = io.MousePos.y - p0.y + st.scroll;
    int hovered = -1;
    for (int i = 0; i < count; ++i)
      if (my >= top[size_t(i)] && my < top[size_t(i)] + height[size_t(i)]) hovered = i;
    if (focusable(hovered) && (in.mouse_moved() || ImGui::IsMouseClicked(0))) st.focus = hovered;
    if (focusable(hovered) && ImGui::IsMouseClicked(0)) {
      Item& it = items[size_t(hovered)];
      const RowGeom g = Geometry(it, p0.x, p1.x, m, f);
      const float mx = io.MousePos.x;
      if (!it.disabled) {
        if (it.type == ItemType::kChoice && !it.options.empty()) {
          const int n = int(it.options.size());
          int to = (it.index + 1) % n;
          if (mx < g.value_l - 4 * s && mx > g.chevron_l - 16 * s) to = std::max(0, it.index - 1);
          if (it.index < 0) to = 0;
          choose(it, to);
        } else if (it.type == ItemType::kSlider) {
          if (mx > g.bar_l - 12 * s && mx < g.bar_r + 12 * s) st.dragging = hovered;
        } else if (it.on_activate) {
          activate(it);
        }
      } else if (it.type != ItemType::kInfo) {
        Cue(Sound::kDeny);
      }
    }
  }
  if (st.dragging >= 0) {
    Item& it = items[size_t(st.dragging)];
    const RowGeom g = Geometry(it, p0.x, p1.x, m, f);
    const float t = std::clamp((io.MousePos.x - g.bar_l) / std::max(1.0f, g.bar_r - g.bar_l), 0.0f, 1.0f);
    float v = it.lo + (it.hi - it.lo) * t;
    if (it.step > 0) v = it.lo + std::round((v - it.lo) / it.step) * it.step;
    set_value(it, std::clamp(v, it.lo, it.hi));
  }
  if (active && st.focus != focus_before && st.focus >= 0 && focus_before >= 0) Cue(Sound::kMove);

  // Scrolling: keep the focused row (and its section's caption) in view.
  const float max_scroll = std::max(0.0f, total - view_h);
  if (keep_visible && st.focus >= 0) {
    const size_t i = size_t(st.focus);
    float a = top[i];
    if (st.focus == first_focusable) a = 0;
    else if (i > 0 && items[i - 1].type == ItemType::kSection) a = top[i - 1];
    const float b = top[i] + height[i];
    const float margin = m.row_h * 0.6f;
    if (a - margin < st.scroll_target) st.scroll_target = a - margin;
    if (b + margin > st.scroll_target + view_h) st.scroll_target = b + margin - view_h;
  }
  st.scroll_target = std::clamp(st.scroll_target, 0.0f, max_scroll);
  st.scroll = std::clamp(Approach(st.scroll, st.scroll_target, 20.0f), 0.0f, max_scroll);

  // The focus band glides between rows.
  if (st.focus >= 0) {
    const float ty = top[size_t(st.focus)], th = height[size_t(st.focus)];
    if (st.band_y < 0) {
      st.band_y = ty;
      st.band_h = th;
    } else {
      st.band_y = Approach(st.band_y, ty, 28.0f);
      st.band_h = Approach(st.band_h, th, 28.0f);
    }
  }

  dl->PushClipRect(p0, p1, true);
  auto visible = [&](int i, float y) { return y + height[size_t(i)] >= p0.y && y <= p1.y; };
  for (int i = 0; i < count; ++i) {
    const float y = p0.y + top[size_t(i)] - st.scroll;
    if (!visible(i, y)) continue;
    DrawItem(dl, items[size_t(i)], p0.x, y, p1.x, height[size_t(i)], false, false, m, f);
    // A hairline between neighbouring rows.
    if (i + 1 < count && items[size_t(i)].type != ItemType::kSection &&
        items[size_t(i) + 1].type != ItemType::kSection) {
      const float ly = std::round(y + height[size_t(i)]) - 0.5f;
      dl->AddLine(ImVec2(p0.x, ly), ImVec2(p1.x, ly), WithAlpha(color::kText, 0.05f), 1.0f);
    }
  }
  if (st.focus >= 0) {
    const ImVec2 b0(p0.x, std::round(p0.y + st.band_y - st.scroll));
    const ImVec2 b1(p1.x, std::round(b0.y + st.band_h));
    dl->AddRectFilled(b0, b1, active ? color::kFocus : WithAlpha(color::kFocus, 0.8f));
    dl->PushClipRect(b0, b1, true);
    for (int i = 0; i < count; ++i) {
      const float y = p0.y + top[size_t(i)] - st.scroll;
      if (y + height[size_t(i)] < b0.y || y > b1.y) continue;
      DrawItem(dl, items[size_t(i)], p0.x, y, p1.x, height[size_t(i)], true, i == st.focus, m, f);
    }
    dl->PopClipRect();
  }
  dl->PopClipRect();

  // A thin scroll bar beside the list when it doesn't fit.
  if (max_scroll > 0) {
    const float x = p1.x + 14 * s;
    dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + 2 * s, p1.y), WithAlpha(color::kText, 0.03f));
    const float th = std::max(24 * s, view_h * view_h / total);
    const float ty = p0.y + (view_h - th) * (st.scroll / max_scroll);
    dl->AddRectFilled(ImVec2(x, ty), ImVec2(x + 2 * s, ty + th), WithAlpha(color::kText, 0.22f));
  }
  st.focus_label = st.focus >= 0 ? items[size_t(st.focus)].label : std::string();
  return st.focus >= 0 ? &items[size_t(st.focus)] : nullptr;
}

void DrawDescription(ImDrawList* dl, const Item* item, ImVec2 p0, ImVec2 p1, const Metrics& m) {
  if (!item || item->type == ItemType::kSection) return;
  const Fonts f = GetFonts();
  const float s = m.s, w = p1.x - p0.x;
  float y = p0.y;
  if (item->icon && item->tall) {
    const float ih = 84 * s;
    dl->AddImage(Tex(item->icon), ImVec2(p0.x, y), ImVec2(p0.x + ih * item->icon_aspect, y + ih), ImVec2(0, 0),
                 ImVec2(1, 1), item->icon_dim ? IM_COL32(120, 124, 132, 200) : IM_COL32_WHITE);
    y += ih + 20 * s;
  }
  const float ts = 25 * s;
  const std::string label = T(item->label);
  dl->AddText(f.display, ts, ImVec2(p0.x, y), color::kText, label.c_str(), nullptr, w);
  y += TextSize(f.display, ts, label, w).y + 14 * s;
  const float bs = 15.5f * s;
  if (!item->description.empty()) {
    const std::string desc = T(item->description);
    dl->AddText(f.text, bs, ImVec2(p0.x, y), color::kTextDim, desc.c_str(), nullptr, w);
    y += TextSize(f.text, bs, desc, w).y + 14 * s;
  }
  for (const std::string& english : item->facts) {
    if (english.empty()) continue;
    const std::string fact = T(english);
    const float indent = 16 * s;
    dl->AddRectFilled(ImVec2(p0.x + 1 * s, y + bs * 0.5f - 1.5f * s), ImVec2(p0.x + 5 * s, y + bs * 0.5f + 2.5f * s),
                      color::kAccent);
    dl->AddText(f.text, bs, ImVec2(p0.x + indent, y), color::kText, fact.c_str(), nullptr, w - indent);
    y += TextSize(f.text, bs, fact, w - indent).y + 8 * s;
  }
  if (!item->facts.empty()) y += 6 * s;
  if (item->disabled && !item->disabled_note.empty()) {
    const std::string note = T(item->disabled_note);
    dl->AddText(f.text, bs, ImVec2(p0.x, y), color::kWarn, note.c_str(), nullptr, w);
    y += TextSize(f.text, bs, note, w).y + 14 * s;
  }
  if (!item->default_text.empty()) {
    const std::string d = text::F("Default: {0}", {T(item->default_text)});
    dl->AddText(f.text, 14 * s, ImVec2(p0.x, y), color::kTextFaint, d.c_str(), nullptr, w);
  }
}

// ----------------------------------------------------------------- pop-up ---
namespace {

std::string PlainText(std::string s) {
  for (const char* mark : {"**", "`"})
    for (size_t at; (at = s.find(mark)) != std::string::npos;) s.erase(at, std::strlen(mark));
  for (size_t open; (open = s.find('[')) != std::string::npos;) {
    const size_t mid = s.find("](", open), close = mid == std::string::npos ? mid : s.find(')', mid);
    if (close == std::string::npos) break;
    s = s.substr(0, open) + s.substr(open + 1, mid - open - 1) + s.substr(close + 1);
  }
  return s;
}

}  // namespace

float DrawRichText(ImDrawList* dl, const std::string& text, ImVec2 pos, float width, const Metrics& m, bool draw) {
  const Fonts f = GetFonts();
  const float s = m.s, bs = 15 * s;
  float y = pos.y;
  bool first = true;
  std::stringstream in(text);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {
      y += 6 * s;
    } else if (line.rfind("## ", 0) == 0) {
      if (!first) {
        y += 18 * s;
        if (draw) dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + width, y), color::kHairline, 1.0f);
        y += 20 * s;
      }
      const std::string t = PlainText(line.substr(3));
      // "v1.9.2|9 October 2026": the version, then the date dimmed.
      const size_t bar = t.find('|');
      const std::string head = t.substr(0, bar), date = bar == std::string::npos ? "" : t.substr(bar + 1);
      const float hs = 24 * s;
      if (draw) {
        dl->AddText(f.display, hs, ImVec2(pos.x, y), color::kText, head.c_str());
        if (!date.empty())
          dl->AddText(f.text, 14 * s, ImVec2(pos.x + TextSize(f.display, hs, head).x + 14 * s, y + hs - 17 * s),
                      color::kTextFaint, date.c_str());
      }
      y += hs + 6 * s;
    } else if (line.rfind("### ", 0) == 0) {
      y += 8 * s;
      const std::string t = PlainText(line.substr(4));
      if (draw) dl->AddText(f.semibold, 16.5f * s, ImVec2(pos.x, y), color::kText, t.c_str(), nullptr, width);
      y += TextSize(f.semibold, 16.5f * s, t, width).y + 6 * s;
    } else {
      size_t indent = 0;
      while (indent < line.size() && line[indent] == ' ') ++indent;
      const bool bullet = line.compare(indent, 2, "- ") == 0;
      const std::string t = PlainText(line.substr(indent + (bullet ? 2 : 0)));
      const float x = pos.x + float(indent / 2) * 18 * s + (bullet ? 16 * s : 0);
      if (draw) {
        if (bullet)
          dl->AddRectFilled(ImVec2(x - 12 * s, y + bs * 0.5f - 1 * s), ImVec2(x - 8 * s, y + bs * 0.5f + 3 * s),
                            WithAlpha(color::kTextDim, 0.8f));
        dl->AddText(f.text, bs, ImVec2(x, y), color::kTextDim, t.c_str(), nullptr, pos.x + width - x);
      }
      y += TextSize(f.text, bs, t, pos.x + width - x).y + 5 * s;
    }
    first = false;
  }
  return y - pos.y;
}

bool DrawModal(ImDrawList* dl, Modal& modal, ImVec2 view0, ImVec2 view1, const Metrics& m, Input& in) {
  if (!modal.open) return false;
  const Fonts f = GetFonts();
  // In the launcher's language.
  const std::string title = T(modal.title), message = T(modal.body);
  std::vector<std::string> buttons;
  for (const std::string& b : modal.buttons) buttons.push_back(T(b));
  const float s = m.s;
  const int frame = ImGui::GetFrameCount();
  if (modal.shown_frame < 0) modal.shown_frame = frame;
  const bool take_input = frame > modal.shown_frame;

  dl->AddRectFilled(view0, view1, IM_COL32(0, 0, 0, 165));
  const float view_w = view1.x - view0.x, view_h = view1.y - view0.y;
  const float w = std::min(modal.width * s, view_w - 64 * s);
  const float pad = 32 * s, inner = w - pad * 2;

  // Measure.
  const float ts = 26 * s, bs = 16 * s, bh = 44 * s;
  float h = pad + TextSize(f.display, ts, title, inner).y + 16 * s;
  if (!message.empty()) h += TextSize(f.text, bs, message, inner).y + 14 * s;
  const std::string live = modal.live ? modal.live() : std::string();
  if (!live.empty()) h += TextSize(f.text, 15 * s, live, inner).y + 12 * s;
  if (modal.progress) h += 22 * s;
  float reader_h = 0, reader_total = 0;
  if (!modal.reader.empty()) {
    reader_total = DrawRichText(dl, modal.reader, ImVec2(0, 0), inner - 16 * s, m, false);
    const float room = view_h - 96 * s - h - (modal.buttons.empty() ? 0 : bh + 24 * s) - pad;
    reader_h = std::max(80 * s, std::min(reader_total, room));
    h += reader_h + 16 * s;
  }
  std::vector<float> bw;
  float row_w = 0;
  for (const std::string& b : buttons) {
    bw.push_back(std::max(132 * s, TextSize(f.semibold, 16 * s, b).x + 48 * s));
    row_w += bw.back();
  }
  row_w += 10 * s * float(bw.empty() ? 0 : bw.size() - 1);
  const bool stacked = row_w > inner;
  if (!modal.buttons.empty()) h += 10 * s + (stacked ? (bh + 8 * s) * float(bw.size()) - 8 * s : bh);
  h += pad;

  const ImVec2 p0(std::round(view0.x + (view_w - w) * 0.5f), std::round(view0.y + (view_h - h) * 0.5f));
  const ImVec2 p1(p0.x + w, p0.y + h);
  dl->AddRectFilled(p0, p1, color::kPanel);
  dl->AddRect(p0, p1, WithAlpha(color::kText, 0.09f));
  dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + 2 * s), color::kAccent);

  float y = p0.y + pad;
  const float x = p0.x + pad;
  dl->AddText(f.display, ts, ImVec2(x, y), color::kText, title.c_str(), nullptr, inner);
  y += TextSize(f.display, ts, title, inner).y + 16 * s;
  if (!message.empty()) {
    dl->AddText(f.text, bs, ImVec2(x, y), color::kTextDim, message.c_str(), nullptr, inner);
    y += TextSize(f.text, bs, message, inner).y + 14 * s;
  }
  if (!live.empty()) {
    dl->AddText(f.text, 15 * s, ImVec2(x, y), color::kText, live.c_str(), nullptr, inner);
    y += TextSize(f.text, 15 * s, live, inner).y + 12 * s;
  }
  if (modal.progress) {
    const float p = modal.progress();
    const float by = y + 6 * s;
    dl->AddRectFilled(ImVec2(x, by), ImVec2(x + inner, by + 3 * s), WithAlpha(color::kText, 0.12f));
    if (p >= 0) {
      dl->AddRectFilled(ImVec2(x, by), ImVec2(x + inner * std::clamp(p, 0.0f, 1.0f), by + 3 * s), color::kAccent);
    } else {  // a segment sweeping across
      const float t = float(std::fmod(ImGui::GetTime() * 0.7, 1.0));
      const float a = x + inner * std::max(0.0f, t * 1.3f - 0.3f), b = x + inner * std::min(1.0f, t * 1.3f);
      dl->AddRectFilled(ImVec2(a, by), ImVec2(b, by + 3 * s), color::kAccent);
    }
    y += 22 * s;
  }
  if (!modal.reader.empty()) {
    const ImVec2 r0(x, y), r1(x + inner, y + reader_h);
    const float max_scroll = std::max(0.0f, reader_total - reader_h);
    if (take_input) {
      const float line = 48 * s;
      if (in.Pressed(Action::kUp)) modal.reader_scroll -= line;
      if (in.Pressed(Action::kDown)) modal.reader_scroll += line;
      if (in.Pressed(Action::kPageUp)) modal.reader_scroll -= reader_h * 0.85f;
      if (in.Pressed(Action::kPageDown)) modal.reader_scroll += reader_h * 0.85f;
      if (MouseIn(r0, r1)) modal.reader_scroll -= ImGui::GetIO().MouseWheel * line * 1.5f;
    }
    modal.reader_scroll = std::clamp(modal.reader_scroll, 0.0f, max_scroll);
    dl->PushClipRect(r0, r1, true);
    DrawRichText(dl, modal.reader, ImVec2(x, y - modal.reader_scroll), inner - 16 * s, m, true);
    dl->PopClipRect();
    if (max_scroll > 0) {
      const float bx = r1.x - 2 * s;
      dl->AddRectFilled(ImVec2(bx, r0.y), ImVec2(bx + 2 * s, r1.y), WithAlpha(color::kText, 0.06f));
      const float th = std::max(24 * s, reader_h * reader_h / reader_total);
      const float ty = r0.y + (reader_h - th) * (modal.reader_scroll / max_scroll);
      dl->AddRectFilled(ImVec2(bx, ty), ImVec2(bx + 2 * s, ty + th), WithAlpha(color::kText, 0.35f));
    }
    y += reader_h + 16 * s;
  }

  // Buttons.
  const int n = int(modal.buttons.size());
  if (n > 0) {
    modal.focus = std::clamp(modal.focus, 0, n - 1);
    const int focus_before = modal.focus;
    if (take_input) {
      const bool reader = !modal.reader.empty();
      if (in.Pressed(Action::kLeft) || (!reader && in.Pressed(Action::kUp))) modal.focus = std::max(0, modal.focus - 1);
      if (in.Pressed(Action::kRight) || (!reader && in.Pressed(Action::kDown)))
        modal.focus = std::min(n - 1, modal.focus + 1);
    }
    y += 10 * s;
    float bx = stacked ? x : p1.x - pad - row_w;
    int chosen = -1;
    for (int i = 0; i < n; ++i) {
      const float bwi = stacked ? inner : bw[size_t(i)];
      const ImVec2 a(bx, y), b(bx + bwi, y + bh);
      const bool hot = take_input && MouseIn(a, b);
      if (hot && in.mouse_moved()) modal.focus = i;
      const bool on = modal.focus == i;
      if (on) dl->AddRectFilled(a, b, color::kFocus);
      else dl->AddRect(a, b, WithAlpha(color::kText, hot ? 0.45f : 0.22f));
      const std::string& label = buttons[size_t(i)];
      const ImVec2 ls = TextSize(f.semibold, 16 * s, label);
      dl->AddText(f.semibold, 16 * s, ImVec2(std::round(a.x + (bwi - ls.x) * 0.5f), std::round(a.y + (bh - ls.y) * 0.5f)),
                  on ? color::kOnFocus : color::kText, label.c_str());
      if (hot && ImGui::IsMouseClicked(0)) chosen = i;
      if (stacked) y += bh + 8 * s;
      else bx += bwi + 10 * s;
    }
    if (take_input) {
      if (in.Pressed(Action::kAccept)) chosen = modal.focus;
      else if (in.Pressed(Action::kBack) && modal.cancel >= 0) chosen = modal.cancel;
    }
    if (chosen >= 0) Cue(chosen == modal.cancel ? Sound::kBack : Sound::kSelect);
    else if (modal.focus != focus_before) Cue(Sound::kMove);
    if (chosen >= 0) {
      // The handler may open another pop-up in this one's place.
      auto handler = modal.on_button;
      modal.open = false;
      if (handler) handler(chosen);
    }
  }
  in.ConsumeAll();  // nothing under the pop-up reacts
  return true;
}

}  // namespace kk::ui
