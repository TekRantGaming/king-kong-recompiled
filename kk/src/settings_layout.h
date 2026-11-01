// Where every setting lives: the settings categories and the order of their rows, shared by the launcher's
// Settings and the pause menu's (launcher.cpp, SettingsPages). The order follows what most PC games use
// (owner, 16 Oct): Display, Graphics, Audio, Controls, Gameplay, Cheats, then Advanced (Cheats has its own tab,
// owner 16 Oct evening; the launcher adds About as its last tab, which has no settings rows); inside each, the usual order
// (Display: window mode, monitor, resolution, refresh rate, VSync, frame-rate cap, then the camera: field of
// view, motion blur, camera shake; Graphics: preset, graphics API, upscaler and quality, anti-aliasing, the effects,
// advanced items last; Audio: master, music, effects, speech, menu sounds; Controls: sensitivity, inversion,
// vibration, bindings; Gameplay: subtitles, language, HUD, Performance metrics, then the rest). Owner, 18 Oct (after
// task 222): Field of view, Motion blur and Camera shake moved to Display, Performance metrics to Gameplay; the
// frame rate cap stays in Display. Only placement and order: no setting's value or meaning.
//
// The page builders add rows in this order (conditional rows may be missing); the developer build checks
// each built page against this table (InLayoutOrder) and tests check the table itself.
// Task 332 (owner, 20 Oct): the first-time setup shows the FULL Display, Graphics and Controls pages, built by the
// same code (launcher.cpp SettingsPageItems, launcher_setup_pages.inc): a row added to kDisplayRows, kGraphicsRows
// or kControlsRows (and its page builder) shows in the setup by itself.
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace kk::settings_layout {

enum Page { kDisplay, kGraphics, kAudio, kControls, kGameplay, kCheats, kAdvanced, kPageCount };
inline constexpr const char* kPageNames[kPageCount] = {"Display", "Graphics", "Audio", "Controls", "Gameplay",
                                                      "Cheats", "Advanced"};

struct Row {
  const char* section;  // the heading the row is under (English, translated as drawn)
  const char* label;    // the row's label; "*" stands for rows made from a list (the cheats)
};

// Also the first-time setup's Display page (task 332).
inline constexpr Row kDisplayRows[] = {
    {"Screen", "Window mode"},       {"Screen", "Monitor"},
    {"Screen", "Window size"},       {"Screen", "Resolution"},
    {"Screen", "Refresh rate"},      {"Screen", "Aspect ratio"},
    {"Picture", "Brightness"},       {"Picture", "Gamma"},
    {"Frame rate", "VSync"},         {"Frame rate", "Frame rate"},
    // (No HDR rows since Beta 14, owner 21 Oct: HDR is experimental, in the F8 graphics panel only.)
    {"Camera", "Field of view"},     {"Camera", "Motion blur"},
    {"Camera", "Camera shake"},      {"Camera", "Camera shake strength"},
};

// Also the first-time setup's Graphics page (task 332).
inline constexpr Row kGraphicsRows[] = {
    {"Preset", "Look"},
    {"Preset", "Preset"},
    {"Preset", "Recommended"},
    {"Renderer", "Graphics API"},
    {"Upscaling", "Upscaler"},
    {"Upscaling", "Upscaler mode"},
    {"Upscaling", "Render quality"},
    {"Upscaling", "Internal resolution"},
    {"Image", "Antialiasing"},
    {"Image", "Sharpness"},
    {"Image", "Texture filtering"},
    {"Lighting", "Ambient occlusion"},
    {"Lighting", "AO strength"},
    {"Lighting", "AO quality"},
    {"Lighting", "Global illumination"},
    {"Lighting", "GI distance"},
    {"Lighting", "Shadows"},
    {"Lighting", "Water reflections"},
    {"Lighting", "Light shafts"},
    {"Effects", "Screen blur"},
    {"Effects", "Distance fog"},
    {"Extras", "Horizontal flip"},
    {"Texture pack", "Texture pack"},
    {"Texture pack", "Texture packs in the Workshop"},  // task 209: downloads, imports, removing are there
    {"Texture pack", "Texture pack folder"},
    {"Advanced", "Effect resolution"},
    {"Advanced", "Rebuild shaders"},
    {"Advanced", "Damn it, I paid for the full CPU"},
};

inline constexpr Row kAudioRows[] = {
    {"Game audio", "Master volume"},
    {"Game audio", "Music volume"},
    {"Game audio", "Effects volume"},
    {"Game audio", "Voices volume"},
    {"Launcher", "Menu music"},
    {"Launcher", "Menu music volume"},
    {"Launcher", "Menu sounds"},
    {"Launcher", "Menu sound volume"},
    {"Achievements", "Sound"},
    {"Achievements", "Sound to play"},
    {"Achievements", "Volume"},
    {"Achievements", "Open sounds folder"},
    {"Reset", "Reset page"},
};

// Also the first-time setup's Controls page (task 332).
inline constexpr Row kControlsRows[] = {
    {"Input", "Input"},
    {"Input", "Button prompts"},
    {"Controller", "Camera sensitivity"},
    {"Controller", "Camera response"},
    {"Controller", "Stick deadzone"},
    {"Controller", "Camera horizontal"},
    {"Controller", "Camera vertical"},
    {"Controller", "Left stick"},
    {"Controller", "Aim"},
    {"Controller", "Vibration"},
    {"Controller", "Vibration strength"},
    {"Keyboard & mouse", "Mouse sensitivity"},
    {"Keyboard & mouse", "Mouse camera"},
    {"Keyboard & mouse", "Mouse vertical"},
    {"Mapping", "Button remapping"},
    {"Mapping", "Keyboard bindings"},
    {"Keys while playing", "Graphics panel"},
};

inline constexpr Row kGameplayRows[] = {
    {"Subtitles and language", "Subtitles"},
    {"Subtitles and language", "Language"},
    {"HUD", "Aiming visor"},
    {"HUD", "Inventory"},
    {"HUD", "Notifications"},
    {"HUD", "Popup style"},
    {"Overlay", "Performance metrics"},
    {"Game", "Startup logos"},
};

inline constexpr Row kCheatsRows[] = {
    {"Cheats", "Cheats"},
    {"Cheat list", "*"},
};

inline constexpr Row kAdvancedRows[] = {
    {"Downloads", "Notify me when downloads finish"},
    {"Cutscenes", "PS2 cutscenes"},
    {"Cutscenes", "Download PS2 cutscenes"},
    {"Cutscenes", "Remove PS2 cutscenes"},
    {"Cutscenes", "PS2 movie folder"},
    {"Diagnostics", "Copy diagnostics"},
    {"Diagnostics", "Saved crash reports"},
};

struct Rows {
  const Row* begin_;
  size_t size;
  const Row* begin() const { return begin_; }
  const Row* end() const { return begin_ + size; }
};

inline Rows RowsOf(Page page) {
  switch (page) {
    case kDisplay: return {kDisplayRows, std::size(kDisplayRows)};
    case kGraphics: return {kGraphicsRows, std::size(kGraphicsRows)};
    case kAudio: return {kAudioRows, std::size(kAudioRows)};
    case kControls: return {kControlsRows, std::size(kControlsRows)};
    case kGameplay: return {kGameplayRows, std::size(kGameplayRows)};
    case kCheats: return {kCheatsRows, std::size(kCheatsRows)};
    case kAdvanced: return {kAdvancedRows, std::size(kAdvancedRows)};
    default: return {nullptr, 0};
  }
}

// Where a row is in its page's table (-1: not in it). Rows made from a list match the "*" entry.
inline int IndexOf(Page page, std::string_view label) {
  int i = 0, wildcard = -1;
  for (const Row& r : RowsOf(page)) {
    if (label == r.label) return i;
    if (std::string_view(r.label) == "*") wildcard = i;
    ++i;
  }
  return wildcard;
}

// One built row: a section heading or a setting.
struct Built {
  bool section;
  std::string_view label;
};

// Whether a built page follows the table: every setting is in it, in its order, and each is under the
// heading the table names (headings may be missing only when the page would show the same heading again).
// `why` says what is wrong.
inline bool InLayoutOrder(Page page, const std::vector<Built>& rows, std::string* why = nullptr) {
  int last = -1;
  std::string_view heading;
  for (const Built& b : rows) {
    if (b.section) {
      heading = b.label;
      continue;
    }
    const int at = IndexOf(page, b.label);
    if (at < 0) {
      if (why) *why = std::string("not in the layout: ") + std::string(b.label);
      return false;
    }
    if (at < last) {
      if (why) *why = std::string("out of order: ") + std::string(b.label);
      return false;
    }
    const Row& r = RowsOf(page).begin()[at];
    if (heading != r.section) {
      if (why) *why = std::string(b.label) + " is under '" + std::string(heading) + "', not '" + r.section + "'";
      return false;
    }
    // A list's rows share one index; other rows must not repeat.
    if (at == last && std::string_view(r.label) != "*") {
      if (why) *why = std::string("repeated: ") + std::string(b.label);
      return false;
    }
    last = at;
  }
  return true;
}

}  // namespace kk::settings_layout
