// The launcher instead of the game's menus (Beta 3, native-renderer/docs/menus.md).
//
// The launcher's Play menu (Continue, New game, Load game, Chapter select) decides what the game starts
// with; the game then never shows its title, save or main menus: the port loads the chosen save and jumps
// straight into the chapter (native-renderer/docs/chapter-jump.md). In play, the port's own pause menu
// replaces the game's (IntMIG_Page_Main), drawn in the launcher's style.
//
// This header is the contract between the game side (play_session.cpp, pause hooks: the session, saves,
// the jump, the pause takeover) and the UI side (launcher.cpp, pause_menu.cpp: what the player sees).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kk::play {

// ------------------------------------------------------------------ saves (port side, no game needed) ---

constexpr int kSlotCount = 5;     // the game's five save slots
constexpr int kChapterCount = 42;  // the chapter select's entries

struct SaveSlot {
  int index = 0;            // 0..4
  bool exists = false;      // the slot has a valid save
  std::string name;         // the save's name (header +0x00)
  int64_t last_saved = 0;   // Unix time (header +0x20)
  int64_t created = 0;      // Unix time (header +0x30)
  int chapter_reached = 0;  // highest chapter-select entry reached (header +0x34)
  int continue_level = 0;   // the save's continue point: level key and start point (header +0x28/+0x2C)
  int continue_start = 0;
  int continue_chapter = -1;  // the chapter-select entry that level belongs to, -1 unknown
  std::vector<int> best_scores;  // per chapter (header +0x38 + 4 * entry), kChapterCount entries
};

// Reads the five slots' headers from the user data (never writes).
std::vector<SaveSlot> ListSaves(const std::filesystem::path& user_dir);

// The slot played most recently (the highest last_saved), or -1 when there is no save.
int MostRecentSave(const std::vector<SaveSlot>& slots);

// Copies every save file into user_dir/save-backups/<yyyyMMdd-HHmmss>/ (keeps the newest 20 copies).
// Called before anything that can write or delete a save. False when the copy failed: then do nothing.
bool BackupSaves(const std::filesystem::path& user_dir, const std::string& reason);

// Deletes one slot's save, after BackupSaves. False on failure (the save is then untouched).
bool DeleteSave(const std::filesystem::path& user_dir, int slot);

// ------------------------------------------------------------------------------ what the game starts with ---

enum class StartKind {
  kNone,          // the game's own boot (developer use only; players never see the game's menus)
  kContinue,      // load `slot`, start at its continue point
  kNewGame,       // create a new save in `slot` (empty, or overwritten after a backup), start the story
  kChapter,       // load `slot` (or create one when it's empty), start chapter-select entry `chapter`
};

struct StartRequest {
  StartKind kind = StartKind::kNone;
  int slot = -1;
  int chapter = -1;        // kChapter only, 0..kChapterCount-1
  std::string new_name;    // kNewGame / kChapter on an empty slot: the new save's name ("" = the game's default)
};

// The launcher sets it before Play; the game side carries it out on the title screen (one frame, no menu).
void SetStartRequest(const StartRequest& request);
StartRequest GetStartRequest();

// Chapter-select entries: the name shown (translated where the game has it) and, when the game files have
// one, a picture for the launcher (decoded RGBA, empty when none).
struct ChapterInfo {
  int entry = 0;
  std::string name;
  int level_key = 0, start_point = 0;
};
const std::vector<ChapterInfo>& Chapters();

// ------------------------------------------------------------------------------------------- the pause menu ---

// True while the port's pause menu is open (the game is paused; its own pause page is not shown and the
// game reads an empty pad).
bool PauseMenuOpen();

// What the pause menu's items do, carried out the game's own way (its page code, its confirmations).
enum class PauseAction { kResume, kStartAgain, kScore, kQuitToLauncher };
void RunPauseAction(PauseAction action);
// Whether an item is available right now (e.g. Start again and Score need a save loaded).
bool PauseActionAvailable(PauseAction action);

}  // namespace kk::play
