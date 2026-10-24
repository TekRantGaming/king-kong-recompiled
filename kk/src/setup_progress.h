// Task 365 (owner, 23 Oct, DECISIONS "First time setup and shader screen changes"): the first time setup's top right
// row. A progress bar by page (every page after the shaders, until the Finish page, which shows nothing there), a
// Next page button, Skip all (with a warning pop-up) and Auto detect (on the pages it sets: the Settings pages and the
// button prompts). The page order is the one SetupStage shows (task 332). Kept free of the launcher so tests check it
// (tests/launcher_logic_tests.cpp). Stage numbers are SetupStage's: 16 to 18 the Settings pages Display, Graphics and
// Controls, 12 the button prompts, 6 the HD texture pack, 7 the PS2 cutscenes (only where they play), 15 the
// Workshop, 13 You're all set (the Finish page).
#pragma once

#include <vector>

namespace kk::setup_progress {

inline constexpr int kDisplay = 16, kGraphics = 17, kControls = 18, kPrompts = 12, kTextures = 6, kCutscenes = 7,
                     kWorkshop = 15, kFinish = 13;

// The pages in order; the PS2 cutscene page only where the PS2 movies play.
inline std::vector<int> Pages(bool cutscenes) {
  std::vector<int> pages = {kDisplay, kGraphics, kControls, kPrompts, kTextures};
  if (cutscenes) pages.push_back(kCutscenes);
  pages.push_back(kWorkshop);
  pages.push_back(kFinish);
  return pages;
}

// Where the bar is: page `index` (1 based) of `count` (the Finish page counted, so the bar is never full before it).
// Not shown on the Finish page or on a stage that is not one of these pages (the shader screen, the game files).
struct Bar {
  bool shown = false;
  int index = 0, count = 0;
};
inline Bar For(int stage, bool cutscenes) {
  const std::vector<int> pages = Pages(cutscenes);
  Bar bar;
  if (stage == kFinish) return bar;
  for (size_t i = 0; i < pages.size(); ++i)
    if (pages[i] == stage) bar = {true, int(i) + 1, int(pages.size())};
  return bar;
}

// The page after `stage` (Next page), or -1 when there is none to go to.
inline int Next(int stage, bool cutscenes) {
  const std::vector<int> pages = Pages(cutscenes);
  for (size_t i = 0; i + 1 < pages.size(); ++i)
    if (pages[i] == stage) return pages[i + 1];
  return -1;
}

// Auto detect is offered on the pages whose settings it picks (the Settings pages and the button prompts); it then
// goes on to the texture pack page.
inline bool AutoDetectOffered(int stage) {
  return stage == kDisplay || stage == kGraphics || stage == kControls || stage == kPrompts;
}

}  // namespace kk::setup_progress
