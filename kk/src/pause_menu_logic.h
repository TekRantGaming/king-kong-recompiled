// The pause menu's layout (task 222, owner's option A, 18 Oct), without ImGui or the game: which entries the
// slim list on the left has and in what order, what the description strip says for each, which settings page an
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
  kDisplay,
  kGraphics,
  kAudio,
  kControls,
  kGameplay,
  kCheats,        // only while the cheats are switched on (the owner's list has no Cheats entry)
  kGameOptions,   // the game's own Options pages: developer builds only
  kRestart,       // the game's "Start again": restarts the chapter (there is no checkpoint restart)
  kScore,         // the game's Score page (chapter-select runs)
  kSave,          // the game's Save (story runs)
  kQuitToMainMenu,
  kQuitGame,      // the game's Quit game, then the program closes
};

// What the game offers right now (play::PauseActionAvailable, photo::Available, kk_cheats).
struct Available {
  bool photo = false;
  bool cheats = false;
  bool game_options = false;
  bool restart = false;
  bool score = false;
  bool save = false;
  bool quit_to_main_menu = true;
  bool quit_game = true;
};

struct Listed {
  Entry entry;
  bool gap_before;  // a little space above it: the game's own items after the settings pages
};

// The list: Resume, Photo mode, Display, Graphics, Audio, Controls, Gameplay, then (after a gap) the game's items
// and the ways out.
inline std::vector<Listed> Entries(const Available& a) {
  std::vector<Listed> out;
  auto add = [&](Entry e, bool gap = false) { out.push_back({e, gap}); };
  add(Entry::kResume);
  if (a.photo) add(Entry::kPhotoMode);
  for (Entry e : {Entry::kDisplay, Entry::kGraphics, Entry::kAudio, Entry::kControls, Entry::kGameplay}) add(e);
  if (a.cheats) add(Entry::kCheats);
  if (a.game_options) add(Entry::kGameOptions);
  bool gap = true;
  auto lower = [&](bool on, Entry e) {
    if (!on) return;
    add(e, gap);
    gap = false;
  };
  lower(a.restart, Entry::kRestart);
  lower(a.score, Entry::kScore);
  lower(a.save, Entry::kSave);
  lower(a.quit_to_main_menu, Entry::kQuitToMainMenu);
  lower(a.quit_game, Entry::kQuitGame);
  return out;
}

// The entry's name (English, translated as drawn). Settings pages use the launcher's tab names.
inline const char* Label(Entry e) {
  switch (e) {
    case Entry::kResume: return "Resume";
    case Entry::kPhotoMode: return "Photo mode";
    case Entry::kDisplay: return settings_layout::kPageNames[settings_layout::kDisplay];
    case Entry::kGraphics: return settings_layout::kPageNames[settings_layout::kGraphics];
    case Entry::kAudio: return settings_layout::kPageNames[settings_layout::kAudio];
    case Entry::kControls: return settings_layout::kPageNames[settings_layout::kControls];
    case Entry::kGameplay: return settings_layout::kPageNames[settings_layout::kGameplay];
    case Entry::kCheats: return settings_layout::kPageNames[settings_layout::kCheats];
    case Entry::kGameOptions: return "Options";
    case Entry::kRestart: return "Restart chapter";
    case Entry::kScore: return "Score";
    case Entry::kSave: return "Save game";
    case Entry::kQuitToMainMenu: return "Quit to main menu";
    case Entry::kQuitGame: return "Quit game";
  }
  return "";
}

// The description strip's sentence for the entry (English, translated as drawn).
inline const char* What(Entry e) {
  switch (e) {
    case Entry::kResume: return "Go back to the game where you left off.";
    case Entry::kPhotoMode: return "Freeze the game and take pictures with a free camera.";
    case Entry::kDisplay: return "Brightness, gamma, the frame rate cap, HDR, field of view and camera shake.";
    case Entry::kGraphics: return "Upscaling, anti-aliasing, lighting, effects and the texture pack.";
    case Entry::kAudio: return "The game's volumes and the menu sounds.";
    case Entry::kControls: return "Camera, controller, mouse and button mapping.";
    case Entry::kGameplay: return "Subtitles, language, HUD and Performance metrics.";
    case Entry::kCheats: return "Switch cheats on or off. Changes apply at once.";
    case Entry::kGameOptions: return "The game's own options pages (test builds only).";
    case Entry::kRestart: return "Start this chapter again from its beginning.";
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

// LB / RB on a page: the next or previous settings page in the list (wrapping), or `page` if it isn't listed.
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
