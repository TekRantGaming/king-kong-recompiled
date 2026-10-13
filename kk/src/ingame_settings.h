// The launcher's settings pages in game, for the pause menu's Settings (docs/menus.md): the same
// rows, built by the same code as in the launcher (launcher.cpp, SettingsPages), but only the rows
// that change while the game runs. Changes apply at once and are saved like in the launcher.

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "launcher_ui.h"

namespace rex::ui {
class ImmediateDrawer;
}

namespace kk {

class InGameSettings {
 public:
  enum Page { kDisplay, kGraphics, kGameplay, kControls, kCheats, kAudio, kSubtitles, kPageCount };
  static const char* PageName(Page page);  // English, translated as drawn

  virtual ~InGameSettings() = default;

  // The page's rows (Controls: its open sub-page, Button remapping or Keyboard bindings).
  virtual std::vector<ui::Item> Items(Page page) = 0;
  // Controls' sub-pages: which one is open (0 none), and back from it.
  virtual int SubPage() const = 0;
  virtual void LeaveSubPage() = 0;
  // Once a frame: a key binding being captured, and saving the settings a moment after a change.
  virtual void Tick() = 0;
  // Saves now if anything changed (the menu closing).
  virtual void SaveNow() = 0;
  virtual bool CanResetPage(Page page) const = 0;
  virtual void ResetPage(Page page) = 0;
  // A key binding is being captured (its pop-up is open; keys aren't actions).
  virtual bool Capturing() const = 0;
  // The line under the list ("Buttons reset to the defaults."), empty when there is none.
  virtual std::string StatusLine() const = 0;

  // The input, button pictures and pop-up the rows use (the pause menu shares them).
  virtual ui::Input& input() = 0;
  virtual ui::Glyphs& glyphs() = 0;
  virtual ui::Modal& modal() = 0;
  // The prompts' picture set: keys while the keyboard and mouse are in use, else the controller's.
  virtual std::string PromptSet() const = 0;
};

// `config_path`: the settings file; `user_dir`: the user data (the Xbox 360 button pictures).
std::unique_ptr<InGameSettings> CreateInGameSettings(rex::ui::ImmediateDrawer* immediate,
                                                     std::filesystem::path config_path,
                                                     std::filesystem::path user_dir);

}  // namespace kk
