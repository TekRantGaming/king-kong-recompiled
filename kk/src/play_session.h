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

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "extras.h"

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
  kExtra,         // `chapter` is an original Extras id; load the selected save, never create one
};

struct StartRequest {
  StartKind kind = StartKind::kNone;
  int slot = -1;
  int chapter = -1;        // kChapter: 0..kChapterCount-1; kExtra: an id from kExtras
  std::string new_name;    // kNewGame / kChapter on an empty slot: the new save's name ("" = the game's default)
};

// The launcher sets it before Play; the game side carries it out on the title screen (one frame, no menu).
void SetStartRequest(const StartRequest& request);
StartRequest GetStartRequest();
// C24: the guest imports only into a fresh private runtime root. Personal progress
// application edits only header chapter_reached, never the host's world or options.
std::vector<uint8_t> ExportSessionSave(const std::filesystem::path& root, int slot);
bool ImportSessionSave(const std::filesystem::path& private_root, const std::vector<uint8_t>& bytes);
bool ApplyTogetherProgress(const std::filesystem::path& personal_root, int slot, uint64_t completed);

ExtraProgress ReadExtraProgress(const std::filesystem::path& user_dir, int slot);

// Chapter-select entries: the name shown (the game's own, in the launcher's language, once the launcher has
// extracted it from the player's game files; "Chapter N" until then) and, when the game files have one, a
// picture for the launcher (a BMP in the launcher's art cache, art::LoadImage reads it; empty when none).
struct ChapterInfo {
  int entry = 0;
  int text_index = 0;  // the game's chapter table, third column: the name's text id 0x4902DD68 + text_index
  std::string name;
  std::filesystem::path picture;
  int level_key = 0, start_point = 0;
};
const std::vector<ChapterInfo>& Chapters();

// ------------------------------------------------------------------------------------------- the pause menu ---

// True while the port's pause menu is open (the game is paused; its own pause page is not shown and the
// game reads an empty pad).
bool PauseMenuOpen();

// Photo mode (task 272): the gain on the output values that takes back the paused game's own dimming (while
// paused it dims its world, often to about 0.4 of play, sometimes not at all): play's last frame over the
// paused one, measured 1.5 s into the pause (pause_backdrop_logic.h MeanUndim); 1 when not measured.
float PhotoUndim();
// This pause's dimming has been measured, or cannot be (photo mode waits for it, up to a moment).
bool PhotoDimKnown();

// What the pause menu's items do, carried out the game's own way (its page code, its confirmations).
// kQuitToLauncher ("Return to main menu"): the game's Quit game (its save behaviour), then the program
// restarts into the launcher's Play menu. kQuitToDesktop ("Quit game"): the same, then the program exits.
// kSave: the game's "Save" (story runs only; see menus.md). kGameOptions: the game's own Options pages
// (Audio, Video, Controls), drawn and read by the game; the pause menu comes back when they close.
// kLoadCheckpoint ("Load last checkpoint", task 237): the pause page's own item 6, which its list never shows:
// play resumes and the game reloads the last checkpoint the way it does after a death, without the death
// (no death counters, no confirmation page of the game's; the port asks first).
enum class PauseAction { kResume, kStartAgain, kScore, kQuitToLauncher, kSave, kQuitToDesktop, kGameOptions,
                         kLoadCheckpoint };
void RunPauseAction(PauseAction action);
// Whether an item is available right now (e.g. Start again and Score need a save loaded).
bool PauseActionAvailable(PauseAction action);

// Original audio, subtitle, video and control options live in each save, not port cvars.
// Launcher/pause changes apply across saves (bonus modes only in saves that have unlocked them).
// A negative slot reads the active game profile (in play).
enum class ProfileOption { kMaster, kVoices, kMusic, kEffects, kSubtitles, kOldMovie, kHorizontalFlip, kAimingVisor, kInventory, kCount };
struct ProfileOptions {
  bool available = false;
  int slot = -1;
  float master = 100, voices = 100, music = 100, effects = 100;
  bool subtitles = false;
  bool subtitle_default = false;
  bool old_movie = false, horizontal_flip = false, aiming_visor = false, inventory = false;
  std::string error;
};
ProfileOptions ReadProfileOptions(const std::filesystem::path& user_dir, int slot);
// Options to change (volumes in percent, subtitles 0 or 1), by ProfileOption.
struct ProfileChange {
  std::array<float, size_t(ProfileOption::kCount)> values{};
  std::array<bool, size_t(ProfileOption::kCount)> set{};
};
// Applies the change to every save: in play, the game's own save and the running game through the
// game thread; every other slot's save file directly (backed up, then replaced in one step). False
// when a save could not be written (it is left unchanged).
bool SetProfileOptionsInAllSaves(const std::filesystem::path& user_dir, const ProfileChange& change);

// Task 177: the native renderer writes its cache files on a background thread and the runtime hard-exits
// (no destructors) when the title ends; called just before every exit so queued writes finish (<= 3 s).
// Task 233: it first stops the renderer's pipeline compiles (QuiescePipelines).
void FlushRendererFiles();
// Task 233: before a hard exit or a hand-over to a new process: the native renderer starts no new background
// pipeline compile and waits up to about 1 s for the ones inside the driver (never hangs; once per process,
// later calls return at once). Nothing happens without the native renderer.
void QuiescePipelines();

}  // namespace kk::play
