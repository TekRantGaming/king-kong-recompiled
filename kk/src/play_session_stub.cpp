// Stand-in for play_session.cpp (the game side, menus-game's) so the UI side builds and its screens
// can be shown before the two are merged. Built only while src/play_session.cpp does not exist
// (CMakeLists.txt). It never reads or writes a file.
//
// Developer switches (environment):
//   KK_DEV_FAKE_SAVES=1   three made-up saves (slots 1, 2 and 4) instead of none
//   KK_DEV_PAUSE_MENU=<s> the pause menu "opens" <s> seconds after the game starts, until Resume
//   KK_DEV_PAUSE_NOSAVE=1 no save loaded: Start again and Score are not available
#include "play_session.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <mutex>

#include <rex/logging.h>

namespace kk::play {
namespace {

std::mutex g_lock;
StartRequest g_request;
bool g_deleted[kSlotCount] = {};
bool g_resumed = false;

bool FakeSaves() {
  const char* v = std::getenv("KK_DEV_FAKE_SAVES");
  return v && *v && *v != '0';
}

}  // namespace

std::vector<SaveSlot> ListSaves(const std::filesystem::path&) {
  std::vector<SaveSlot> slots(kSlotCount);
  for (int i = 0; i < kSlotCount; ++i) {
    slots[size_t(i)].index = i;
    slots[size_t(i)].best_scores.assign(kChapterCount, 0);
  }
  if (!FakeSaves()) return slots;
  const int64_t now = int64_t(std::time(nullptr));
  struct Fake {
    int slot;
    const char* name;
    int reached, continue_chapter;
    int64_t age;
  };
  static const Fake kFakes[] = {{0, "A", 8, 8, 3600 * 2}, {1, "Ann", 27, 26, 86400 * 3}, {3, "Jack", 41, 41, 600}};
  std::lock_guard lock(g_lock);
  for (const Fake& f : kFakes) {
    if (g_deleted[f.slot]) continue;
    SaveSlot& s = slots[size_t(f.slot)];
    s.exists = true;
    s.name = f.name;
    s.last_saved = now - f.age;
    s.created = now - f.age - 86400 * 7;
    s.chapter_reached = f.reached;
    s.continue_chapter = f.continue_chapter;
    for (int c = 0; c <= f.reached && c < kChapterCount; ++c) s.best_scores[size_t(c)] = 5000 + (c * 7919 + f.slot * 131) % 20000;
  }
  return slots;
}

int MostRecentSave(const std::vector<SaveSlot>& slots) {
  int best = -1;
  for (const SaveSlot& s : slots)
    if (s.exists && (best < 0 || s.last_saved > slots[size_t(best)].last_saved)) best = s.index;
  return best;
}

bool BackupSaves(const std::filesystem::path&, const std::string& reason) {
  REXLOG_INFO("KK play (stub): back up the saves ({})", reason);
  return true;
}

bool DeleteSave(const std::filesystem::path&, int slot) {
  REXLOG_INFO("KK play (stub): delete slot {}", slot + 1);
  if (slot < 0 || slot >= kSlotCount) return false;
  std::lock_guard lock(g_lock);
  g_deleted[slot] = true;
  return true;
}

void SetStartRequest(const StartRequest& request) {
  std::lock_guard lock(g_lock);
  g_request = request;
  REXLOG_INFO("KK play (stub): start request kind {} slot {} chapter {} name '{}'", int(request.kind), request.slot,
              request.chapter, request.new_name);
}

StartRequest GetStartRequest() {
  std::lock_guard lock(g_lock);
  return g_request;
}

const std::vector<ChapterInfo>& Chapters() {
  // Placeholder names until the game's own are read from its files.
  static const std::vector<ChapterInfo> chapters = [] {
    static const int kLevels[kChapterCount] = {100,  101,  201,  203,  204,  301,  302,  303,  305,  306,  304,
                                               401,  402,  403,  404,  501,  502,  504,  702,  705,  901,  1002,
                                               1101, 1200, 1201, 703,  1401, 1402, 1501, 1701, 1702, 1703, 1801,
                                               1803, 1804, 1805, 1901, 1902, 2001, 2003, 2004, 7};
    std::vector<ChapterInfo> out;
    for (int i = 0; i < kChapterCount; ++i) {
      ChapterInfo c;
      c.entry = i;
      c.name = i == 0 ? "Venture" : i == 8 ? "V-Rex" : i == 27 ? "Kong to the Rescue" : "Chapter " + std::to_string(i + 1);
      c.level_key = kLevels[i];
      c.start_point = i == 0 || i == 41 ? 0 : i == 40 ? 49 : 1;
      out.push_back(c);
    }
    return out;
  }();
  return chapters;
}

bool PauseMenuOpen() {
  static const double after = [] {
    const char* v = std::getenv("KK_DEV_PAUSE_MENU");
    return v && *v ? std::atof(v) : -1.0;
  }();
  if (after < 0) return false;
  static const auto first = std::chrono::steady_clock::now();
  std::lock_guard lock(g_lock);
  return !g_resumed && std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() >= after;
}

void RunPauseAction(PauseAction action) {
  REXLOG_INFO("KK play (stub): pause action {}", int(action));
  std::lock_guard lock(g_lock);
  if (action == PauseAction::kResume) g_resumed = true;
}

bool PauseActionAvailable(PauseAction action) {
  const bool no_save = std::getenv("KK_DEV_PAUSE_NOSAVE") != nullptr;
  return !(no_save && (action == PauseAction::kStartAgain || action == PauseAction::kScore));
}

}  // namespace kk::play
