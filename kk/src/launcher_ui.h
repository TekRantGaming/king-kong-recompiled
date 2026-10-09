// The launcher's own small UI toolkit, drawn with ImGui's draw lists: one
// focus model for controller, keyboard and mouse, rows of settings with a
// description panel, pop-ups, and button prompts that follow the last input
// device (Xbox, PlayStation or keyboard pictures from the glyphs folder).
//
// Pages describe their rows each frame (a list of Item); DrawList lays them
// out, moves the focus, applies input and calls the items' callbacks.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace kk::ui {

// ------------------------------------------------------------------ theme ---
// Night over Skull Island: near-black, bone-white focus, one aged-brass accent.
namespace color {
constexpr ImU32 kBase = IM_COL32(8, 10, 13, 255);
constexpr ImU32 kPanel = IM_COL32(14, 16, 20, 250);
constexpr ImU32 kText = IM_COL32(234, 236, 239, 255);
constexpr ImU32 kTextDim = IM_COL32(152, 159, 168, 255);
constexpr ImU32 kTextFaint = IM_COL32(98, 105, 115, 255);
constexpr ImU32 kHairline = IM_COL32(255, 255, 255, 20);
constexpr ImU32 kFocus = IM_COL32(232, 227, 216, 255);  // bone
constexpr ImU32 kOnFocus = IM_COL32(15, 17, 21, 255);
constexpr ImU32 kOnFocusDim = IM_COL32(96, 98, 102, 255);
constexpr ImU32 kAccent = IM_COL32(201, 160, 94, 255);  // aged brass
constexpr ImU32 kGood = IM_COL32(136, 184, 142, 255);
constexpr ImU32 kWarn = IM_COL32(222, 168, 96, 255);
constexpr ImU32 kBad = IM_COL32(220, 126, 110, 255);
}  // namespace color

ImU32 WithAlpha(ImU32 c, float alpha);
ImTextureRef Tex(rex::ui::ImmediateTexture* t);

// Display (Bahnschrift: tabs, headings, the title), text (Segoe UI) and its
// semibold.
struct Fonts {
  ImFont* display;
  ImFont* text;
  ImFont* semibold;
};
Fonts GetFonts();

ImVec2 TextSize(ImFont* font, float size, const std::string& text, float wrap = 0.0f);
// Letter-spaced text (captions and tabs).
float TrackedWidth(ImFont* font, float size, const std::string& text, float tracking);
void DrawTracked(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const std::string& text,
                 float tracking);
// Shortens `text` with an ellipsis to fit `width`.
std::string Ellipsize(ImFont* font, float size, const std::string& text, float width);

// Moves `v` toward `target`, framerate independent (`rate` per second).
float Approach(float v, float target, float rate);

// ------------------------------------------------------------------ input ---
enum class Action {
  kUp, kDown, kLeft, kRight,
  kPageUp, kPageDown,
  kAccept, kBack, kDefault, kResetPage,
  kPrevTab, kNextTab, kPlay,
  kCount,
};
constexpr size_t kActionCount = size_t(Action::kCount);

enum class Device { kKeyboard, kXbox, kPlayStation };

// Controllers (SDL gamepads), the keyboard (through ImGui) and the mouse, as
// actions. Held directions repeat. The device of the last input picks the
// prompt pictures.
class Input {
 public:
  Input();
  ~Input();
  void Update();  // once a frame, before anything reads it
  // Lets go of the controllers (before the game's own input starts).
  void Release();
  bool Pressed(Action a) const { return pressed_[size_t(a)]; }
  void Consume(Action a) { pressed_[size_t(a)] = false; }
  void ConsumeAll() { pressed_ = {}; }
  // How long a direction has been held (sliders speed up).
  float HeldFor(Action a) const;
  void Inject(Action a, Device d);  // testing aid
  Device device() const { return device_; }
  // The kind of controller last used (for controller pictures while the
  // keyboard is in use).
  Device pad_device() const { return pad_device_; }
  bool mouse_moved() const { return mouse_moved_; }
  int gamepad_count() const { return int(pads_.size()); }
  // While a key binding is being captured, keys aren't actions.
  void set_keyboard_enabled(bool on) { keyboard_enabled_ = on; }

 private:
  struct Repeat {
    bool held = false;
    double since = 0, next = 0;
  };
  bool RepeatHeld(Repeat& r, bool down, double now);
  void UpdateGamepads(double now);

  std::array<bool, kActionCount> pressed_{};
  std::array<Repeat, 6> dir_{};  // up, down, left, right, page up, page down (controller)
  std::array<float, 6> held_{};
  std::array<float, 4> key_held_{};  // arrow keys
  Device device_ = Device::kKeyboard;
  Device pad_device_ = Device::kXbox;
  bool mouse_moved_ = false;
  bool keyboard_enabled_ = true;
  ImVec2 last_mouse_{-1, -1};
  bool sdl_ready_ = false;
  std::vector<void*> pads_;  // SDL_Gamepad*
  std::array<bool, 8> buttons_down_{};
  // When each action was last held on a controller / pressed on the keyboard:
  // Steam's desktop controller layout also types keys for the controller's
  // buttons (arrows, Enter, Escape), and one press must count once.
  std::array<double, kActionCount> pad_held_at_{};
  std::array<double, kActionCount> key_pressed_at_{};
};

// --------------------------------------------------------------- prompts ---
// Button pictures from glyphs/<set>/<name>.png beside the program (sets:
// keyboard, xbox_series, ps5, ps2), or <name>.bmp in a folder added for a set
// (xbox360, cut from the game's own textures).
class Glyphs {
 public:
  Glyphs(rex::ui::ImmediateDrawer* drawer, std::filesystem::path dir);
  ~Glyphs();
  void AddSet(const std::string& set, std::filesystem::path dir);
  void Reload();  // after a set's pictures changed
  // The picture for an action in a set (a controller set falls back to
  // xbox_series) and its width / height; nullptr if missing.
  rex::ui::ImmediateTexture* Get(Action a, const std::string& set, float* aspect);
  rex::ui::ImmediateTexture* Named(const std::string& set, const std::string& name, float* aspect);

 private:
  rex::ui::ImmediateDrawer* drawer_;
  std::filesystem::path dir_;
  std::map<std::string, std::filesystem::path> set_dirs_;
  struct Entry {
    std::unique_ptr<rex::ui::ImmediateTexture> texture;
    float aspect = 1.0f;
  };
  std::map<std::string, Entry> cache_;
};

// ----------------------------------------------------------------- sounds ---
// What just happened, for the launcher's sounds: focus moved, something was
// chosen, Back, a two-way choice switched on or off, a value changed, a slider
// moved, a tab changed, or a press that does nothing (disabled).
enum class Sound { kMove, kSelect, kBack, kOn, kOff, kChange, kSlide, kTab, kDeny, kCount };
// `value`: a slider's position (0..1).
void SetSoundHandler(std::function<void(Sound, float)> handler);
void Cue(Sound sound, float value = 0.5f);

// One prompt in the bar. Action::kLeft stands for left/right ("Change") and
// kUp for up/down ("Scroll").
struct Prompt {
  Action action;
  std::string label;
};

// ------------------------------------------------------------------ items ---
enum class ItemType {
  kSection,  // a caption over a group of rows
  kChoice,   // a value picked from options (on/off too)
  kSlider,   // a number between lo and hi
  kAction,   // does something when selected
  kLink,     // opens a sub-page
  kInfo,     // read-only (achievements)
};

struct Item {
  ItemType type = ItemType::kAction;
  std::string label;
  std::string description;
  std::vector<std::string> facts;  // extra lines in the description panel
  // kChoice
  std::vector<std::string> options;
  int index = 0;  // -1: none of the options (value_text is shown)
  // kSlider
  float value = 0, lo = 0, hi = 1, step = 1;
  // Shown on the right (for kSlider, the formatted value).
  std::string value_text;
  ImU32 value_color = 0;  // 0: the default color
  bool disabled = false;
  std::string disabled_note;  // why, in the description panel
  std::string default_text;   // "Default: ..." in the description panel
  rex::ui::ImmediateTexture* icon = nullptr;
  float icon_aspect = 1.0f;
  bool icon_dim = false;
  bool tall = false;      // two lines: label and a line of description (achievements)
  std::string subtitle;   // a tall row's second line (defaults to description)
  std::function<void(int)> on_choice;
  std::function<void(float)> on_value;
  std::function<void()> on_activate;
  std::function<void()> on_default;  // back to the default
};

// Kept per page: focus, scroll and the focus band's animation.
struct ListState {
  int focus = -1;
  std::string focus_label;  // follows its row when rows above appear or go
  float scroll = 0, scroll_target = 0;
  float band_y = -1, band_h = 0;
  int dragging = -1;  // slider held with the mouse
};

struct Metrics {
  float s = 1.0f;  // UI scale
  float row_h = 46, tall_h = 72, section_h = 50;
};

// Lays out, draws and drives a column of items between p0 and p1. Returns the
// focused item (or nullptr). `active`: this list has the input (no pop-up).
const Item* DrawList(ImDrawList* dl, std::vector<Item>& items, ListState& st, ImVec2 p0, ImVec2 p1,
                     const Metrics& m, Input& in, bool active);

// The description panel for the focused item.
void DrawDescription(ImDrawList* dl, const Item* item, ImVec2 p0, ImVec2 p1, const Metrics& m);

// The prompt bar, right-aligned to `right` and centred on `center_y`. A
// clicked prompt returns its action (Action::kCount if none).
Action DrawPrompts(ImDrawList* dl, Glyphs& glyphs, const std::string& set, const std::vector<Prompt>& prompts,
                   float right, float center_y, const Metrics& m, bool clickable);

// Tabs across the top with the previous/next pictures either side. Returns a
// clicked tab (or -1).
int DrawTabs(ImDrawList* dl, Glyphs& glyphs, const std::string& set, const std::vector<std::string>& names, int current,
             ImVec2 pos, const Metrics& m, bool clickable, float* underline_x, float* underline_w);

// ----------------------------------------------------------------- pop-up ---
struct Modal {
  bool open = false;
  std::string title;
  std::string body;                   // wrapped text
  std::function<std::string()> live;  // an extra line, read each frame
  std::function<float()> progress;    // a bar (0..1; below 0: busy without a known size)
  std::vector<std::string> buttons;
  int focus = 0;
  int cancel = -1;  // the button Back picks (-1: none)
  std::function<void(int)> on_button;  // the pop-up closes first; the handler may reopen it
  std::string reader;                  // long markdown-ish text that scrolls (changelog)
  float reader_scroll = 0;
  float width = 620;
  int shown_frame = -1;  // input waits a frame after opening
};

// Draws the open pop-up and handles its input. Returns true if one is open.
bool DrawModal(ImDrawList* dl, Modal& modal, ImVec2 view0, ImVec2 view1, const Metrics& m, Input& in);

// Markdown-ish text: "## " version headings, "### " headings, "- " bullets
// (two more spaces per level) and paragraphs; **bold**, `code` and links show
// as plain text. Returns the height used (draw = false only measures).
float DrawRichText(ImDrawList* dl, const std::string& text, ImVec2 pos, float width, const Metrics& m,
                   bool draw = true);

}  // namespace kk::ui
