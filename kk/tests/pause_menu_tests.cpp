// Task 222: the pause menu's layout (option A) without ImGui or the game: the list's entries and their order,
// names that match the launcher's Settings tabs, no "launcher" in any text, the page order (live rows first, then
// the quiet "Change from the main menu" group) and LB / RB between the listed pages.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "pause_menu_logic.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}
using namespace kk::pause;

std::vector<Entry> Only(const std::vector<Listed>& list) {
  std::vector<Entry> out;
  for (const Listed& l : list) out.push_back(l.entry);
  return out;
}
bool Contains(std::string_view text, std::string_view word) {
  std::string lower(text);
  for (char& c : lower) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return lower.find(word) != std::string::npos;
}
}  // namespace

int main() {
  // The owner's list (18 Oct): Resume, Photo mode, Display, Graphics, Audio, Controls, Gameplay, (gap) Restart,
  // Quit to main menu.
  Available a;
  a.photo = true;
  a.restart = true;
  a.quit_game = false;
  const std::vector<Listed> list = Entries(a);
  Check(Only(list) == std::vector<Entry>{Entry::kResume, Entry::kPhotoMode, Entry::kDisplay, Entry::kGraphics,
                                         Entry::kAudio, Entry::kControls, Entry::kGameplay, Entry::kRestart,
                                         Entry::kQuitToMainMenu},
        "the owner's list");
  int gaps = 0;
  for (const Listed& l : list) gaps += l.gap_before;
  Check(gaps == 1 && list[7].gap_before, "one gap, before Restart chapter");

  // Nothing optional: no Photo mode, no game items; the gap moves to the first way out.
  Available none;
  none.quit_game = false;
  const std::vector<Listed> bare = Entries(none);
  Check(bare.size() == 7 && bare.front().entry == Entry::kResume && bare.back().entry == Entry::kQuitToMainMenu &&
            bare.back().gap_before,
        "bare list");
  Available nothing_out;
  nothing_out.quit_to_main_menu = nothing_out.quit_game = false;
  Check(Entries(nothing_out).size() == 6, "no way out offered: the settings pages only");

  // Everything: Cheats after Gameplay, the game's items in order, Quit game last.
  Available all;
  all.photo = all.cheats = all.game_options = all.restart = all.score = all.save = true;
  const std::vector<Entry> full = Only(Entries(all));
  Check(full == std::vector<Entry>{Entry::kResume, Entry::kPhotoMode, Entry::kDisplay, Entry::kGraphics, Entry::kAudio,
                                   Entry::kControls, Entry::kGameplay, Entry::kCheats, Entry::kGameOptions,
                                   Entry::kRestart, Entry::kScore, Entry::kSave, Entry::kQuitToMainMenu,
                                   Entry::kQuitGame},
        "full list");

  // Names: the settings entries are the launcher's tab names (no "Picture"); no text says "launcher"; every entry
  // has a sentence for the strip.
  for (Entry e : full) {
    Check(Label(e)[0] != 0 && What(e)[0] != 0, "every entry has a name and a sentence");
    Check(!Contains(Label(e), "launcher") && !Contains(What(e), "launcher"), "no 'launcher' in the pause menu");
    Check(!Contains(Label(e), "picture"), "no Picture page");
    const int page = PageOf(e);
    if (page >= 0) Check(std::string_view(Label(e)) == kk::settings_layout::kPageNames[page], "tab names");
  }
  Check(std::string_view(Label(Entry::kQuitToMainMenu)) == "Quit to main menu", "Quit to main menu");
  Check(!Contains(kQuietCaption, "launcher") && !Contains(kGreyedNote, "launcher"), "quiet group text");
  Check(std::string_view(kQuietCaption) == "Change from the main menu", "quiet caption wording");
  Check(std::string_view(kGreyedNote) == "This can't be changed while you play. Change it from the main menu.",
        "greyed note wording");
  Check(PageOf(Entry::kResume) < 0 && PageOf(Entry::kRestart) < 0 && PageOf(Entry::kQuitGame) < 0,
        "game items open no page");

  // LB / RB: the listed pages, wrapping; Cheats only while listed.
  using kk::settings_layout::Page;
  Check(NextPage(list, Page::kDisplay, 1) == Page::kGraphics, "RB from Display");
  Check(NextPage(list, Page::kDisplay, -1) == Page::kGameplay, "LB from Display wraps to Gameplay");
  Check(NextPage(list, Page::kGameplay, 1) == Page::kDisplay, "RB from Gameplay wraps to Display");
  Check(NextPage(Entries(all), Page::kGameplay, 1) == Page::kCheats, "RB from Gameplay to Cheats when listed");
  Check(NextPage(list, Page::kAdvanced, 1) == Page::kAdvanced, "an unlisted page stays");

  // Page order: live rows under their sections first (a section without live rows goes), then the quiet caption
  // and the greyed rows without their sections.
  using R = RowKind;
  // Screen: Window mode (greyed), Resolution (greyed), Aspect ratio | Picture: Brightness, Gamma | Frame rate:
  // VSync (greyed), Frame rate
  const std::vector<R> display = {R::kSection, R::kGreyed, R::kGreyed, R::kLive, R::kSection, R::kLive,
                                  R::kLive,    R::kSection, R::kGreyed, R::kLive};
  Check(PageOrder(display) == std::vector<int>{0, 3, 4, 5, 6, 7, 9, kQuietHeading, 1, 2, 8}, "display order");
  const std::vector<R> only_greyed = {R::kSection, R::kGreyed, R::kGreyed};
  Check(PageOrder(only_greyed) == std::vector<int>{kQuietHeading, 1, 2}, "only greyed rows");
  const std::vector<R> only_live = {R::kSection, R::kLive, R::kSection, R::kLive};
  Check(PageOrder(only_live) == std::vector<int>{0, 1, 2, 3}, "no greyed rows: the launcher's order");
  const std::vector<R> empty_section = {R::kSection, R::kSection, R::kLive};
  Check(PageOrder(empty_section) == std::vector<int>{1, 2}, "an empty section goes");
  Check(PageOrder({}).empty(), "no rows");
  const std::vector<R> no_section = {R::kLive, R::kGreyed, R::kLive};
  Check(PageOrder(no_section) == std::vector<int>{0, 2, kQuietHeading, 1}, "rows without a section");

  std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
