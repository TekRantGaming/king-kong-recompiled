// The pause menu's layout (task 222, owner's option A, 18 Oct; task 248, owner 19 Oct: a shorter list with a
// Settings sub-list), without ImGui or the game: which entries the slim list on the left and the Settings list have
// and in what order, what the description strip says for each, which settings page an
// entry opens, the order of a page's rows (the rows that change while playing first, then the quiet "Change from
// the main menu" group). pause_menu.cpp draws it; tests/pause_menu_tests.cpp checks it.
//
// In game the launcher is "the main menu" (owner, 18 Oct): no text here says "launcher".
#pragma once

#include <vector>

#include "settings_layout.h"

namespace kk::pause {

enum class Entry {
  kResume,
  kPhotoMode,
  kControls,
  kGameplay,
  kSettings,      // opens the Settings list (task 248)
  // The Settings list:
  kDisplay,
  kGraphics,
  kAudio,
  kCheats,        // always (Beta 12, owner 19 Oct: the main Cheats switch on it must stay reachable when off)
  kGameOptions,   // "Options", the game's own Options pages: developer builds only
  // After the gap in the main list:
  kLoadCheckpoint,  // "Load last checkpoint" (owner 18 Oct, replaces the chapter restart): the reload a death makes
  kScore,         // the game's Score page (chapter-select runs)
  kSave,          // the game's Save (story runs)
  kQuitToMainMenu,
  kQuitGame,      // the game's Quit game, then the program closes
};

// What the game offers right now (play::PauseActionAvailable, photo::Available).
struct Available {
  bool photo = false;
  bool game_options = false;
  bool checkpoint = false;
  bool score = false;
  bool save = false;
  bool quit_to_main_menu = true;
  bool quit_game = true;
};

struct Listed {
  Entry entry;
  bool gap_before;  // a little space above it: the game's own items after the settings pages
};

// The main list (owner 19 Oct): Resume, Photo mode, Controls, Gameplay, Settings, then (after a gap) Load last
// checkpoint, Score, Save game (next to Score, when the game offers it), Quit to main menu, Quit game.
inline std::vector<Listed> Entries(const Available& a) {
  std::vector<Listed> out;
  auto add = [&](Entry e, bool gap = false) { out.push_back({e, gap}); };
  add(Entry::kResume);
  if (a.photo) add(Entry::kPhotoMode);
  for (Entry e : {Entry::kControls, Entry::kGameplay, Entry::kSettings}) add(e);
  bool gap = true;
  auto lower = [&](bool on, Entry e) {
    if (!on) return;
    add(e, gap);
    gap = false;
  };
  lower(a.checkpoint, Entry::kLoadCheckpoint);
  lower(a.score, Entry::kScore);
  lower(a.save, Entry::kSave);
  lower(a.quit_to_main_menu, Entry::kQuitToMainMenu);
  lower(a.quit_game, Entry::kQuitGame);
  return out;
}

// The Settings list ("PAUSED / SETTINGS"): Display, Graphics, Audio, Cheats, Options (developer builds). Cheats is
// always listed (Beta 12): with the main Cheats switch off the page still shows, so it can be switched on again.
inline std::vector<Listed> SettingsEntries(const Available& a) {
  std::vector<Listed> out;
  for (Entry e : {Entry::kDisplay, Entry::kGraphics, Entry::kAudio, Entry::kCheats}) out.push_back({e, false});
  if (a.game_options) out.push_back({Entry::kGameOptions, false});
  return out;
}

// Whether the entry sits in the Settings list (its page's breadcrumb is "PAUSED / SETTINGS / <page>").
inline bool InSettings(Entry e) {
  return e == Entry::kDisplay || e == Entry::kGraphics || e == Entry::kAudio || e == Entry::kCheats ||
         e == Entry::kGameOptions;
}

// The entry's name (English, translated as drawn). Settings pages use the launcher's tab names.
inline const char* Label(Entry e) {
  switch (e) {
    case Entry::kResume: return "Resume";
    case Entry::kPhotoMode: return "Photo mode";
    case Entry::kControls: return settings_layout::kPageNames[settings_layout::kControls];
    case Entry::kGameplay: return settings_layout::kPageNames[settings_layout::kGameplay];
    case Entry::kSettings: return "Settings";
    case Entry::kDisplay: return settings_layout::kPageNames[settings_layout::kDisplay];
    case Entry::kGraphics: return settings_layout::kPageNames[settings_layout::kGraphics];
    case Entry::kAudio: return settings_layout::kPageNames[settings_layout::kAudio];
    case Entry::kCheats: return settings_layout::kPageNames[settings_layout::kCheats];
    case Entry::kGameOptions: return "Options";
    case Entry::kLoadCheckpoint: return "Load last checkpoint";
    case Entry::kScore: return "Score";
    case Entry::kSave: return "Save game";
    case Entry::kQuitToMainMenu: return "Quit to main menu";
    case Entry::kQuitGame: return "Quit game";
  }
  return "";
}

// The Quit confirmations in a story run, where the game offers Save game (Beta 12, owner 19 Oct: a player lost
// progress closing the window). True to the game: it writes the save only from its save pages, the pause menu's
// Save game and the save its Quit game makes (pause_hooks.cpp; both of the menu's ways out go through it); a
// checkpoint is only a snapshot in memory (ES_CheckPoint) for Load last checkpoint. Translated as drawn.
inline constexpr const char* kQuitToMainMenuSaves =
    "Leave the game and go back to the main menu? The game saves when you quit from this menu or choose Save game, not at checkpoints. Closing the window another way loses unsaved progress.";
inline constexpr const char* kQuitGameSaves = "Leave the game and close it? The game saves when you quit from this menu or choose Save game, not at checkpoints. Closing the window another way loses unsaved progress.";

// The description strip's sentence for the entry (English, translated as drawn).
inline const char* What(Entry e) {
  switch (e) {
    case Entry::kResume: return "Go back to the game where you left off.";
    case Entry::kPhotoMode: return "Freeze the game and take pictures with a free camera.";
    case Entry::kSettings: return "Display, graphics and audio settings.";
    case Entry::kDisplay: return "Brightness, gamma, the frame rate cap, HDR, field of view and camera shake.";
    case Entry::kGraphics: return "Upscaling, anti-aliasing, lighting, effects and the texture pack.";
    case Entry::kAudio: return "The game's volumes and the menu sounds.";
    case Entry::kControls: return "Camera, controller, mouse and button mapping.";
    case Entry::kGameplay: return "Subtitles, language, HUD and Performance metrics.";
    case Entry::kCheats: return "Switch cheats on or off. Changes apply at once.";
    case Entry::kGameOptions: return "The game's own options pages (test builds only).";
    case Entry::kLoadCheckpoint: return "Go back to the last checkpoint you reached.";
    case Entry::kScore: return "See your score for this chapter.";
    case Entry::kSave: return "Save your progress.";
    case Entry::kQuitToMainMenu: return "Leave the game and go back to the main menu.";
    case Entry::kQuitGame: return "Leave the game and close it.";
  }
  return "";
}

// The settings page an entry opens (settings_layout.h), or -1 for the game's items.
inline int PageOf(Entry e) {
  switch (e) {
    case Entry::kDisplay: return settings_layout::kDisplay;
    case Entry::kGraphics: return settings_layout::kGraphics;
    case Entry::kAudio: return settings_layout::kAudio;
    case Entry::kControls: return settings_layout::kControls;
    case Entry::kGameplay: return settings_layout::kGameplay;
    case Entry::kCheats: return settings_layout::kCheats;
    default: return -1;
  }
}

// The list a page belongs to: the Settings list for Display, Graphics, Audio and Cheats, else the main list
// (Controls, Gameplay).
inline std::vector<Listed> ListWithPage(const Available& a, int page) {
  std::vector<Listed> settings = SettingsEntries(a);
  for (const Listed& l : settings)
    if (PageOf(l.entry) == page) return settings;
  return Entries(a);
}

// The entry that opens `page` in its list, or kResume if none does.
inline Entry EntryOfPage(const Available& a, int page) {
  for (const Listed& l : ListWithPage(a, page))
    if (PageOf(l.entry) == page) return l.entry;
  return Entry::kResume;
}

// LB / RB on a page: the next or previous page of the same list (wrapping), or `page` if it isn't listed. Pass
// ListWithPage(a, page): Settings pages move among themselves, Controls and Gameplay between themselves.
inline int NextPage(const std::vector<Listed>& list, int page, int dir) {
  std::vector<int> pages;
  for (const Listed& l : list)
    if (PageOf(l.entry) >= 0) pages.push_back(PageOf(l.entry));
  const int n = int(pages.size());
  for (int i = 0; i < n; ++i)
    if (pages[size_t(i)] == page) return pages[size_t(((i + dir) % n + n) % n)];
  return page;
}

// ------------------------------------------------------------------------------------------- page rows ---
enum class RowKind { kSection, kLive, kGreyed };

// Where the quiet group's caption goes in PageOrder's result.
inline constexpr int kQuietHeading = -1;
// The quiet group's caption (English, translated as drawn; settings_help.h kNotWhilePlaying says the same).
inline constexpr const char* kQuietCaption = "Change from the main menu";
// The greyed row's note (launcher.cpp kInLauncherOnly).
inline constexpr const char* kGreyedNote = "This can't be changed while you play. Change it from the main menu.";

// A page's rows in the pause menu: the rows that can change while playing first, under their sections (a section
// with none of them goes), then kQuietHeading and the greyed rows in their order, without their sections.
inline std::vector<int> PageOrder(const std::vector<RowKind>& rows) {
  std::vector<int> out, greyed;
  int section = -1;
  for (int i = 0; i < int(rows.size()); ++i) {
    switch (rows[size_t(i)]) {
      case RowKind::kSection: section = i; break;
      case RowKind::kGreyed: greyed.push_back(i); break;
      case RowKind::kLive:
        if (section >= 0) out.push_back(section);
        section = -1;
        out.push_back(i);
        break;
    }
  }
  if (!greyed.empty()) {
    out.push_back(kQuietHeading);
    out.insert(out.end(), greyed.begin(), greyed.end());
  }
  return out;
}

}  // namespace kk::pause
