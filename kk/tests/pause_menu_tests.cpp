// Task 222: the pause menu's layout (option A) without ImGui or the game: the list's entries and their order,
// names that match the launcher's Settings tabs, no "launcher" in any text, the page order (live rows first, then
// the quiet "Change from the main menu" group) and LB / RB between the listed pages. Task 248 (owner 19 Oct): the
// shorter main list with a Settings list.
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
  // The owner's list (19 Oct): Resume, Photo mode, Controls, Gameplay, Settings, (gap) Load last checkpoint, Score,
  // Quit to main menu, Quit game.
  Available a;
  a.photo = true;
  a.checkpoint = true;
  a.score = true;
  const std::vector<Listed> list = Entries(a);
  Check(Only(list) == std::vector<Entry>{Entry::kResume, Entry::kPhotoMode, Entry::kControls, Entry::kGameplay,
                                         Entry::kSettings, Entry::kLoadCheckpoint, Entry::kScore,
                                         Entry::kQuitToMainMenu, Entry::kQuitGame},
        "the owner's list");
  int gaps = 0;
  for (const Listed& l : list) gaps += l.gap_before;
  Check(gaps == 1 && list[5].gap_before, "one gap, before Load last checkpoint");

  // Nothing optional: no Photo mode, no game items; the gap moves to the first way out.
  Available none;
  none.quit_game = false;
  const std::vector<Listed> bare = Entries(none);
  Check(Only(bare) == std::vector<Entry>{Entry::kResume, Entry::kControls, Entry::kGameplay, Entry::kSettings,
                                         Entry::kQuitToMainMenu} &&
            bare.back().gap_before,
        "bare list");
  Available nothing_out;
  nothing_out.quit_to_main_menu = nothing_out.quit_game = false;
  Check(Entries(nothing_out).size() == 4, "no way out offered: Resume, Controls, Gameplay, Settings");

  // Everything: Save game next to Score in the lower group; Cheats and Options only in the Settings list.
  Available all;
  all.photo = all.cheats = all.game_options = all.checkpoint = all.score = all.save = true;
  const std::vector<Entry> full = Only(Entries(all));
  Check(full == std::vector<Entry>{Entry::kResume, Entry::kPhotoMode, Entry::kControls, Entry::kGameplay,
                                   Entry::kSettings, Entry::kLoadCheckpoint, Entry::kScore, Entry::kSave,
                                   Entry::kQuitToMainMenu, Entry::kQuitGame},
        "full list");
  const std::vector<Entry> full_settings = Only(SettingsEntries(all));
  Check(full_settings == std::vector<Entry>{Entry::kDisplay, Entry::kGraphics, Entry::kAudio, Entry::kCheats,
                                            Entry::kGameOptions},
        "full Settings list");
  // Players (no cheats, no developer Options): Display, Graphics, Audio.
  Check(Only(SettingsEntries(a)) == std::vector<Entry>{Entry::kDisplay, Entry::kGraphics, Entry::kAudio},
        "players' Settings list");
  for (const Listed& l : SettingsEntries(all)) Check(!l.gap_before && InSettings(l.entry), "Settings list entries");
  for (Entry e : full) Check(!InSettings(e), "main list entries are not in Settings");
  Check(std::string_view(Label(Entry::kSettings)) == "Settings", "Settings label");
  Check(std::string_view(What(Entry::kSettings)) == "Display, graphics and audio settings.", "Settings strip");
  Check(PageOf(Entry::kSettings) < 0, "Settings opens a list, not a page");

  // Names: the settings entries are the launcher's tab names (no "Picture"); no text says "launcher"; every entry
  // has a sentence for the strip.
  std::vector<Entry> every = full;
  every.insert(every.end(), full_settings.begin(), full_settings.end());
  for (Entry e : every) {
    Check(Label(e)[0] != 0 && What(e)[0] != 0, "every entry has a name and a sentence");
    Check(!Contains(Label(e), "launcher") && !Contains(What(e), "launcher"), "no 'launcher' in the pause menu");
    Check(!Contains(Label(e), "picture"), "no Picture page");
    const int page = PageOf(e);
    if (page >= 0) Check(std::string_view(Label(e)) == kk::settings_layout::kPageNames[page], "tab names");
  }
  Check(std::string_view(Label(Entry::kQuitToMainMenu)) == "Quit to main menu", "Quit to main menu");
  // Task 237 (owner 18 Oct): "Load last checkpoint" replaces "Restart chapter"; it is listed only when offered.
  Check(std::string_view(Label(Entry::kLoadCheckpoint)) == "Load last checkpoint", "Load last checkpoint");
  Check(Contains(What(Entry::kLoadCheckpoint), "checkpoint") && !Contains(What(Entry::kLoadCheckpoint), "chapter"),
        "the strip speaks of the checkpoint, not the chapter");
  for (Entry e : every) Check(!Contains(Label(e), "restart"), "no Restart entry");
  Available no_checkpoint = all;
  no_checkpoint.checkpoint = false;
  for (const Listed& l : Entries(no_checkpoint))
    Check(l.entry != Entry::kLoadCheckpoint, "no checkpoint (Extras galleries, credits): not listed");
  Check(!Contains(kQuietCaption, "launcher") && !Contains(kGreyedNote, "launcher"), "quiet group text");
  Check(std::string_view(kQuietCaption) == "Change from the main menu", "quiet caption wording");
  Check(std::string_view(kGreyedNote) == "This can't be changed while you play. Change it from the main menu.",
        "greyed note wording");
  Check(PageOf(Entry::kResume) < 0 && PageOf(Entry::kLoadCheckpoint) < 0 && PageOf(Entry::kQuitGame) < 0 &&
            PageOf(Entry::kGameOptions) < 0,
        "game items open no page");

  // Back: a page returns to the list it came from, on its entry.
  using kk::settings_layout::Page;
  Check(EntryOfPage(a, Page::kDisplay) == Entry::kDisplay && InSettings(EntryOfPage(a, Page::kAudio)),
        "Settings pages go back to the Settings list");
  Check(EntryOfPage(a, Page::kControls) == Entry::kControls && !InSettings(EntryOfPage(a, Page::kGameplay)),
        "Controls and Gameplay go back to the main list");
  Check(EntryOfPage(all, Page::kCheats) == Entry::kCheats && InSettings(Entry::kCheats), "Cheats is in Settings");

  // LB / RB: the pages of the same list, wrapping; Cheats only while listed.
  const std::vector<Listed> sl = ListWithPage(a, Page::kDisplay);
  Check(Only(sl) == Only(SettingsEntries(a)), "Display's list is the Settings list");
  Check(NextPage(sl, Page::kDisplay, 1) == Page::kGraphics, "RB from Display");
  Check(NextPage(sl, Page::kDisplay, -1) == Page::kAudio, "LB from Display wraps to Audio");
  Check(NextPage(sl, Page::kAudio, 1) == Page::kDisplay, "RB from Audio wraps to Display");
  Check(NextPage(ListWithPage(all, Page::kAudio), Page::kAudio, 1) == Page::kCheats, "RB from Audio to Cheats when listed");
  Check(NextPage(ListWithPage(all, Page::kCheats), Page::kCheats, 1) == Page::kDisplay,
        "RB from Cheats skips Options (no page) and wraps to Display");
  const std::vector<Listed> ml = ListWithPage(a, Page::kControls);
  Check(Only(ml) == Only(list), "Controls' list is the main list");
  Check(NextPage(ml, Page::kControls, 1) == Page::kGameplay && NextPage(ml, Page::kGameplay, 1) == Page::kControls &&
            NextPage(ml, Page::kControls, -1) == Page::kGameplay,
        "Controls and Gameplay between themselves");
  Check(NextPage(ListWithPage(a, Page::kCheats), Page::kCheats, 1) == Page::kCheats,
        "an unlisted page stays (Cheats while off)");
  Check(NextPage(ListWithPage(a, Page::kAdvanced), Page::kAdvanced, 1) == Page::kAdvanced, "Advanced stays");

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
