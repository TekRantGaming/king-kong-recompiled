// The game side of the launcher's Play menu (native-renderer/docs/menus.md, contract play_session.h):
// the saves (read, backed up, deleted by the port), the chapter list, and the start request carried out
// on the title screen so that no logo, title, save or main menu is ever shown. Research and addresses:
// native-renderer/docs/chapter-jump.md.
//
// Start request (one frame on the title screen, in the universe's uni_go, sub_8253DB80):
//   Continue  load the slot's save from its file (as the save menu would), then ask for its continue
//             point, as the main menu's Play does.
//   New game  create the save in the slot the way the save menu's "Create new" does (the header for a
//             new game, the universe re-init, the continue point 100 / -1), write it with the save
//             system's own save job, then start the story as the load page does.
//   Chapter   load the slot's save (or create one in an empty slot), then do what pressing A on the
//             chapter select's entry does (IntMIG_Page_Map) and what its load page does.
// While that happens the screen is kept black (CoverScreen); the logo movies are skipped.
//
// Routing: in a session the launcher started, the game never shows its start menu (level 3): when it
// asks for it (the end of a chapter-select run after the Score pages, or Quit game), the port restarts
// into the launcher's Play menu instead, once any save in progress has been written.
//
// Save safety: every launch backs up the saves before the game can write; the save routine and the
// save job never let a save go out with continue level 0 (the game deletes the save then).

#include "ui_paint.h"

#include "play_session.h"
#include "handoff.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "guest.h"
#include "launcher_art.h"
#include "launcher_text.h"
#include "play_internal.h"
#include "benchmark.h"
#include "settings.h"
#include "ps2_movies.h"
#include "dev_launcher_chain.h"
#include "video_modes.h"

namespace kk {
#if defined(KK_DEV_TOOLS)
void DevStartScriptClock();                                         // dev_tools.cpp
void DevUniGoTick(uint8_t* base, uint32_t g, bool request, bool started);  // dev_chapter_jump.cpp
void DevDumpTexts(const uint8_t* base, const char* name);                   // dev_chapter_jump.cpp
#endif
}  // namespace kk

namespace kk::play {
namespace {

// ------------------------------------------------------------------------------------------ helpers ---

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return std::byteswap(v);
}
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = std::byteswap(v);
  std::memcpy(base + addr, &v, 4);
}
uint32_t BE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return std::byteswap(v);
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr const char* kTitleId = "555307D3";
constexpr size_t kSaveFileSize = 0x8040, kHeaderSize = 0x100, kDataSize = 0x7F40;
constexpr int kBackupsKept = 20;
constexpr int kScoreEntries = 40;  // the header holds best scores for the first 40 entries (+0x38 .. +0xD7)

// The game's addresses (chapter-jump.md).
constexpr uint32_t kAiPtr = 0x82CCBF64;           // the running AI instance; +0x40 its variables
constexpr uint32_t kGamePtr = 0x82CC97C0;         // the universe variables G once a level is up
constexpr uint32_t kProfilePtr = 0x82CC8D24;      // the same block, as the profile P
constexpr uint32_t kVideoModeTarget = 0x82CCAC30; // the object sub_825EE130 writes the flip to (+0x9CC)
constexpr uint32_t kMemcard = 0x82C63950;         // the save system M
constexpr uint32_t kSaveHeaders = kMemcard + 0x4C, kSaveData = kMemcard + 0x550, kSaveSlot = kMemcard + 0x54C;
constexpr uint32_t kSaveQueue = 0x82C8E7CC;       // the save system's job queue: +0xC a job is waiting
constexpr uint32_t kSaveStatus = kSaveQueue + 4;  // the last job's result
constexpr uint32_t kActivePad = 0x82C63704;       // the pad that pressed Start on the title
constexpr uint32_t kStorageDevice = 0x828E3898;   // the storage device the selector chose
constexpr uint32_t kUniverseScript = 0x828E838C;  // the universe's script instance
constexpr uint32_t kChapterNumber = 0x828D177C;   // set from G[0x526C + 4i] by the chapter select
constexpr uint32_t kLoadFlag = 0x828E3B48;        // set by the load page
constexpr uint32_t kTitleLevel = 2, kStartMenuLevel = 3, kNewGameLevel = 100;

// ------------------------------------------------------------------------------------------- state ---

std::mutex g_mutex;
StartRequest g_request;
std::filesystem::path g_user_dir;
std::function<void()> g_quit;

std::atomic<bool> g_session{false};   // a start request was carried out
std::atomic<bool> g_starting{false};  // a start request is waiting for the title screen
std::atomic<bool> g_leaving{false};   // going back to the launcher
std::atomic<bool> g_cover{false};
std::atomic<int> g_save_jobs{0};
std::atomic<bool> g_chapter_session{false};  // the session is a chapter-select run      // save jobs (sub_82722620) running now

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool IsHex16(const std::string& name) {
  if (name.size() != 16) return false;
  return std::all_of(name.begin(), name.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

// <user data>\<profile>\555307D3: the game's saves. The runtime's profile folder is B13EBABEBABEBABE;
// any other 16-digit profile folder that has the game's saves is taken when that one is missing.
std::filesystem::path TitleDir(const std::filesystem::path& user_dir) {
  std::error_code ec;
  const auto preferred = user_dir / "B13EBABEBABEBABE" / kTitleId;
  if (std::filesystem::is_directory(preferred, ec)) return preferred;
  for (const auto& e : std::filesystem::directory_iterator(user_dir, ec)) {
    if (!e.is_directory(ec) || !IsHex16(Upper(e.path().filename().string()))) continue;
    if (std::filesystem::is_directory(e.path() / kTitleId, ec)) return e.path() / kTitleId;
  }
  return preferred;
}

std::filesystem::path SlotFolder(const std::filesystem::path& user_dir, int slot) {
  return TitleDir(user_dir) / "00000001" / fmt::format("KKProfileSlot{}.sav", slot + 1);
}
std::filesystem::path SlotFile(const std::filesystem::path& user_dir, int slot) {
  return SlotFolder(user_dir, slot) / "KKProfile.dat";
}
std::filesystem::path SlotHeaderFile(const std::filesystem::path& user_dir, int slot) {
  return TitleDir(user_dir) / "Headers" / "00000001" / fmt::format("KKProfileSlot{}.sav.header", slot + 1);
}

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  std::ifstream in(path, std::ios::binary);
  if (!in) return data;
  data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return data;
}

std::string LocalStamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

// The files under a folder, as relative paths.
std::vector<std::filesystem::path> FilesUnder(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) return files;
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::end(it);
       it.increment(ec)) {
    if (it->is_regular_file(ec)) files.push_back(std::filesystem::relative(it->path(), root, ec));
  }
  std::sort(files.begin(), files.end());
  return files;
}

// A backup folder's name: yyyyMMdd-HHmmss, with -N when two were made in the same second.
bool IsBackupName(const std::string& name) {
  if (name.size() < 15 || name[8] != '-') return false;
  for (size_t i = 0; i < 15; ++i)
    if (i != 8 && !std::isdigit(static_cast<unsigned char>(name[i]))) return false;
  return name.size() == 15 || name[15] == '-';
}

std::vector<std::filesystem::path> BackupFolders(const std::filesystem::path& backups) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(backups, ec))
    if (e.is_directory(ec) && IsBackupName(e.path().filename().string())) out.push_back(e.path());
  std::sort(out.begin(), out.end());
  return out;
}

const char* KindName(StartKind kind) {
  switch (kind) {
    case StartKind::kContinue: return "continue";
    case StartKind::kNewGame: return "new";
    case StartKind::kChapter: return "chapter";
    case StartKind::kExtra: return "extra";
    default: return "none";
  }
}

std::string EncodeName(const std::string& name) {
  std::string out;
  for (unsigned char c : name) {
    if (std::isalnum(c) || c == '_' || c == '-' || c == '.') out += char(c);
    else out += fmt::format("%{:02X}", c);
  }
  return out;
}
std::string DecodeName(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      out += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

// "continue:<slot>", "new:<slot>[:<name>]", "chapter:<slot>:<entry>[:<name>]" (names percent-encoded).
std::string FormatRequest(const StartRequest& r) {
  switch (r.kind) {
    case StartKind::kContinue: return fmt::format("continue:{}", r.slot);
    case StartKind::kNewGame: return fmt::format("new:{}:{}", r.slot, EncodeName(r.new_name));
    case StartKind::kChapter: return fmt::format("chapter:{}:{}:{}", r.slot, r.chapter, EncodeName(r.new_name));
    case StartKind::kExtra: return fmt::format("extra:{}:{}", r.slot, r.chapter);
    default: return "";
  }
}
StartRequest ParseRequest(const std::string& text) {
  StartRequest r;
  std::vector<std::string> parts;
  size_t from = 0;
  for (size_t at; (at = text.find(':', from)) != std::string::npos; from = at + 1) parts.push_back(text.substr(from, at - from));
  parts.push_back(text.substr(from));
  auto num = [&](size_t i, int fallback) { return i < parts.size() && !parts[i].empty() ? std::atoi(parts[i].c_str()) : fallback; };
  if (parts[0] == "continue") {
    r.kind = StartKind::kContinue;
    r.slot = num(1, -1);
  } else if (parts[0] == "new") {
    r.kind = StartKind::kNewGame;
    r.slot = num(1, -1);
    if (parts.size() > 2) r.new_name = DecodeName(parts[2]);
  } else if (parts[0] == "chapter") {
    r.kind = StartKind::kChapter;
    r.slot = num(1, -1);
    r.chapter = num(2, 0);
    if (parts.size() > 3) r.new_name = DecodeName(parts[3]);
  } else if (parts[0] == "extra" && parts.size() == 3) {
    // Extra ids must be exact: an invalid request must never start a different bonus.
    try {
      size_t n = 0;
      r.chapter = std::stoi(parts[2], &n);
      if (n == parts[2].size() && FindExtra(r.chapter)) {
        r.kind = StartKind::kExtra;
        r.slot = num(1, -1);
      }
    } catch (...) {}
  }
  return r;
}

}  // namespace

// ============================================================================================== saves ===

const std::vector<ChapterInfo>& Chapters() {
  // The chapter select's table (filled by uni_init at G+0x5008 / G+0x50D4 / G+0x51A0; checked against the
  // game's own table when a session starts, see CheckChapterTable). Names and pictures: the game's, from the
  // launcher's art cache (launcher_art.cpp extracts them from the player's game files), in the launcher's
  // language; "Chapter N" and no picture until then. Looked for again when the language changes and, while
  // missing, every 2 s (the extraction runs on a worker thread).
  static constexpr int kKeys[kChapterCount] = {100,  101,  201,  203,  204,  301,  302,  303,  305,  306,  304,
                                               401,  402,  403,  404,  501,  502,  504,  702,  705,  901,  1002,
                                               1101, 1200, 1201, 703,  1401, 1402, 1501, 1701, 1702, 1703, 1801,
                                               1803, 1804, 1805, 1901, 1902, 2001, 2003, 2004, 7};
  static constexpr int kText[kChapterCount] = {17, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                               13, 14, 15, 16, 18, 19, 20, 21, 22, 23, 38, 24, 25, 26,
                                               27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 39, 40, 41};
  // Built again only when something changed (the language, or the names arrived); earlier lists are kept
  // so a reference another thread holds stays good.
  static std::mutex mutex;
  static std::vector<std::unique_ptr<std::vector<ChapterInfo>>> lists;
  static int loaded_lang = -1;
  static bool have_names = false;
  static std::chrono::steady_clock::time_point checked{};
  std::lock_guard lock(mutex);
  const int lang = int(text::Current());
  const auto now = std::chrono::steady_clock::now();
  if (!lists.empty() && lang == loaded_lang && (have_names || now - checked < std::chrono::seconds(2)))
    return *lists.back();
  checked = now;
  static constexpr const char* kCodes[] = {"en", "de", "es", "fr", "it"};  // text::Lang's order
  const auto dir = art::LauncherArtDir(g_user_dir);
  std::vector<std::string> names;
  if (!g_user_dir.empty() && art::HasLauncherArt(dir)) {
    std::ifstream f(art::ChapterNamesPath(dir, kCodes[lang >= 0 && lang < 5 ? lang : 0]), std::ios::binary);
    for (std::string line; std::getline(f, line);) names.push_back(line);
    if (names.size() != art::kChapterNames) names.clear();
  }
  if (!lists.empty() && lang == loaded_lang && names.empty()) return *lists.back();  // still nothing new
  std::vector<ChapterInfo> out;
  for (int i = 0; i < kChapterCount; ++i) {
    ChapterInfo c;
    c.entry = i;
    c.text_index = kText[i];
    c.name = names.empty() || names[size_t(kText[i])].empty() ? fmt::format("Chapter {}", i + 1) : names[size_t(kText[i])];
    if (!names.empty()) {
      std::error_code ec;
      const auto picture = art::ChapterPicturePath(dir, kText[i]);
      if (std::filesystem::exists(picture, ec)) c.picture = picture;
    }
    c.level_key = kKeys[i];
    c.start_point = i == 0 || i == 41 ? 0 : i == 40 ? 49 : 1;
    out.push_back(std::move(c));
  }
  lists.push_back(std::make_unique<std::vector<ChapterInfo>>(std::move(out)));
  loaded_lang = lang;
  have_names = !names.empty();
  return *lists.back();
}

std::vector<SaveSlot> ListSaves(const std::filesystem::path& user_dir) {
  std::vector<SaveSlot> slots;
  for (int i = 0; i < kSlotCount; ++i) {
    SaveSlot s;
    s.index = i;
    s.best_scores.assign(kChapterCount, 0);
    const auto data = ReadAll(SlotFile(user_dir, i));
    if (data.size() == kSaveFileSize) {
      const uint8_t* h = data.data();
      s.name.assign(reinterpret_cast<const char*>(h), strnlen(reinterpret_cast<const char*>(h), 31));
      s.last_saved = BE32(h + 0x20);
      s.continue_level = int32_t(BE32(h + 0x28));
      s.continue_start = int32_t(BE32(h + 0x2C));
      s.created = BE32(h + 0x30);
      s.chapter_reached = int32_t(BE32(h + 0x34));
      for (int e = 0; e < kScoreEntries; ++e) s.best_scores[size_t(e)] = int32_t(BE32(h + 0x38 + 4 * e));
      for (const auto& c : Chapters())
        if (c.level_key == s.continue_level) {
          s.continue_chapter = c.entry;
          break;
        }
      // A header with continue level 0 is what the game treats as an empty slot.
      s.exists = s.continue_level != 0;
    }
    slots.push_back(std::move(s));
  }
  return slots;
}

int MostRecentSave(const std::vector<SaveSlot>& slots) {
  int best = -1;
  for (const auto& s : slots)
    if (s.exists && (best < 0 || s.last_saved > slots[size_t(best)].last_saved)) best = s.index;
  return best;
}

bool BackupSaves(const std::filesystem::path& user_dir, const std::string& reason) {
  std::error_code ec;
  const auto title = TitleDir(user_dir);
  const auto files = FilesUnder(title);
  if (files.empty()) {
    REXLOG_INFO("KK play: no saves to back up ({})", reason);
    return true;
  }
  const auto backups = user_dir / "save-backups";
  const auto profile = title.parent_path().filename();
  // Nothing changed since the newest backup: keep that one.
  if (const auto existing = BackupFolders(backups); !existing.empty()) {
    const auto last = existing.back() / profile / kTitleId;
    bool same = FilesUnder(last) == files;
    for (size_t i = 0; same && i < files.size(); ++i) same = ReadAll(title / files[i]) == ReadAll(last / files[i]);
    if (same) {
      REXLOG_INFO("KK play: saves unchanged since backup {} ({})", existing.back().filename().string(), reason);
      return true;
    }
  }
  std::string name = LocalStamp();
  for (int n = 2; std::filesystem::exists(backups / name, ec); ++n) name = LocalStamp() + fmt::format("-{}", n);
  const auto dest = backups / name;
  for (const auto& rel : files) {
    const auto to = dest / profile / kTitleId / rel;
    std::filesystem::create_directories(to.parent_path(), ec);
    if (ec || !std::filesystem::copy_file(title / rel, to, std::filesystem::copy_options::overwrite_existing, ec) ||
        std::filesystem::file_size(to, ec) != std::filesystem::file_size(title / rel, ec)) {
      REXLOG_ERROR("KK play: backing up the saves failed at {} ({})", rel.string(), ec.message());
      return false;
    }
  }
  if (std::ofstream note(dest / "reason.txt"); note) note << reason << "\n";
  REXLOG_INFO("KK play: saves backed up to {} ({}, {} files)", dest.string(), reason, files.size());
  // Keep the newest backups only.
  auto all = BackupFolders(backups);
  for (size_t i = 0; all.size() > size_t(kBackupsKept) && i < all.size() - size_t(kBackupsKept); ++i) {
    std::filesystem::remove_all(all[i], ec);
    REXLOG_INFO("KK play: removed the old save backup {}", all[i].filename().string());
  }
  return true;
}

bool DeleteSave(const std::filesystem::path& user_dir, int slot) {
  if (slot < 0 || slot >= kSlotCount) return false;
  std::error_code ec;
  const auto folder = SlotFolder(user_dir, slot), header = SlotHeaderFile(user_dir, slot);
  if (!std::filesystem::exists(folder, ec) && !std::filesystem::exists(header, ec)) return true;
  if (!BackupSaves(user_dir, fmt::format("before deleting slot {}", slot + 1))) return false;
  // The save is moved out of the game's folder (into the backups) rather than erased.
  const auto dest = user_dir / "save-backups" / fmt::format("deleted-{}-slot{}", LocalStamp(), slot + 1);
  std::filesystem::create_directories(dest, ec);
  if (std::filesystem::exists(folder, ec)) {
    std::filesystem::rename(folder, dest / folder.filename(), ec);
    if (ec) {
      REXLOG_ERROR("KK play: deleting save slot {} failed ({})", slot + 1, ec.message());
      return false;
    }
  }
  if (std::filesystem::exists(header, ec)) std::filesystem::rename(header, dest / header.filename(), ec);
  REXLOG_INFO("KK play: deleted save slot {} (moved to {})", slot + 1, dest.string());
  return true;
}

// ====================================================================================== start request ===

void SetStartRequest(const StartRequest& request) {
  std::lock_guard lock(g_mutex);
  g_request = request;
  g_starting = request.kind != StartKind::kNone;
#if defined(KK_DEV_TOOLS)
  if (request.kind == StartKind::kExtra && std::getenv("KK_DEV_LAUNCHER_INPUT")) {
    // A scripted controller activation is for this session, never repeated after returning to Extras;
    // the next '|' session, if any, runs there. Frames this run captures are named after the bonus.
    const std::string next = kk::DevLauncherAfterStart(std::getenv("KK_DEV_LAUNCHER_INPUT"));
    const std::string tag = fmt::format("extra{}-", request.chapter);
#if defined(_WIN32)
    _putenv_s("KK_DEV_LAUNCHER_INPUT", next.c_str());
    _putenv_s("KK_DEV_LAUNCHER_INPUT_FILE", "");
    _putenv_s("KK_DEV_SHOTS_TAG", tag.c_str());
#else
    setenv("KK_DEV_LAUNCHER_INPUT", next.c_str(), 1);
    unsetenv("KK_DEV_LAUNCHER_INPUT_FILE");
    setenv("KK_DEV_SHOTS_TAG", tag.c_str(), 1);
#endif
  }
#endif
  REXLOG_INFO("KK play: start request {} (slot {}, chapter {})", KindName(request.kind), request.slot, request.chapter);
}

StartRequest GetStartRequest() {
  std::lock_guard lock(g_mutex);
  return g_request;
}

void SetUserDir(const std::filesystem::path& user_dir) { g_user_dir = user_dir; }
const std::filesystem::path& UserDir() { return g_user_dir; }
std::filesystem::path ProfileFilePath(const std::filesystem::path& user_dir, int slot) {
  return slot >= 0 && slot < kSlotCount ? SlotFile(user_dir, slot) : std::filesystem::path{};
}
ExtraProgress ReadExtraProgress(const std::filesystem::path& user_dir, int slot) {
  if (slot < 0 || slot >= kSlotCount) return {};
  return DecodeExtraProgress(ReadAll(SlotFile(user_dir, slot)));
}
void SetQuitHandler(std::function<void()> quit) { g_quit = std::move(quit); }
bool SessionActive() { return g_session; }
bool ProfileSaveBusy(const uint8_t* base) {
  return g_save_jobs != 0 || Load32(base, kSaveQueue + 0xC) != 0;
}

std::wstring StartRequestArg() {
  const std::string s = FormatRequest(GetStartRequest());
  return s.empty() ? std::wstring() : L"--kk_start=" + std::wstring(s.begin(), s.end());
}

void LoadStartRequestFromSettings() {
  std::string text = REXCVAR_GET(kk_start);
  rex::cvar::ResetToDefault("kk_start");  // never saved
#if defined(KK_DEV_TOOLS)
  // KK_DEV_START=<request>: the same as --kk_start, for tests without the launcher.
  if (const char* v = std::getenv("KK_DEV_START"); text.empty() && v && *v) text = v;
#endif
  if (text.empty()) return;
  const StartRequest r = ParseRequest(text);
  if (r.kind == StartKind::kNone) {
    REXLOG_WARN("KK play: unknown start request '{}'", text);
    return;
  }
  SetStartRequest(r);
}

bool RelaunchWith(const std::wstring& extra) {
  static constexpr const wchar_t* kDropped[] = {L"--kk_skip_launcher", L"--kk_start", L"--kk_launcher_open",
                                               L"--kk_startup_child", L"--kk_benchmark", L"--kk_benchmark_record",
                                               L"--kk_benchmark_route"};
  auto dropped = [](const std::wstring& arg) {
    for (const wchar_t* d : kDropped) {
      const std::wstring p(d);
      if (arg.rfind(p, 0) == 0 && (arg.size() == p.size() || arg[p.size()] == L'=')) return true;
    }
    return false;
  };
  std::vector<std::wstring> args;
#if defined(_WIN32)
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) args.emplace_back(argv[i]);
  if (argv) LocalFree(argv);
#else
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  bool first = true;
  for (std::string a; std::getline(cmdline, a, '\0');) {
    if (!first) args.emplace_back(a.begin(), a.end());
    first = false;
  }
#endif
  std::vector<std::wstring> kept;
  for (auto& a : args) {
    if (dropped(a)) continue;
#if defined(KK_DEV_TOOLS)
    // Developer runs give --log_file: the new process writes beside it, not over it.
    if (a.rfind(L"--log_file=", 0) == 0) {
      std::filesystem::path p(a.substr(11));
      a = L"--log_file=" + (p.parent_path() / (p.stem().wstring() + L"-relaunch" + p.extension().wstring())).wstring();
    }
#endif
    kept.push_back(a);
  }
  for (size_t from = 0, at; from < extra.size(); from = at + 1) {
    at = extra.find(L' ', from);
    if (at == std::wstring::npos) at = extra.size();
    if (at > from) kept.push_back(extra.substr(from, at - from));
  }
#if defined(KK_DEV_TOOLS)
  // The developer start request is for this run only.
  for (const char* name : {"KK_DEV_START", "KK_DEV_START_END"}) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
  }
#endif
#if defined(_WIN32)
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  auto quote = [](const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (wchar_t c : a) {
      if (c == L'"') q += L'\\';
      q += c;
    }
    return q + L"\"";
  };
  std::wstring cmd = quote(exe);
  for (const auto& a : kept) cmd += L" " + quote(a);
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const auto dir = std::filesystem::path(exe).parent_path();
  kk::handoff::BeforeChild();  // this window stays up until the new one has drawn (handoff.h)
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
    REXLOG_ERROR("KK play: relaunch failed ({})", GetLastError());
    kk::handoff::ChildFailed();
    return false;
  }
  CloseHandle(pi.hThread);
  kk::handoff::ChildStarted(pi.hProcess);
#else
  const char* appimage = std::getenv("APPIMAGE");
  const std::string exe = appimage ? appimage : std::filesystem::read_symlink("/proc/self/exe").string();
  std::vector<std::string> narrow;
  for (const auto& a : kept) narrow.emplace_back(a.begin(), a.end());
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(exe.c_str()));
  for (auto& a : narrow) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
    REXLOG_ERROR("KK play: relaunch failed");
    return false;
  }
#endif
  REXLOG_INFO("KK play: started the program again with {}", std::string(extra.begin(), extra.end()));
  return true;
}

namespace {
std::atomic<int64_t> g_leave_since{0};
std::string g_leave_why, g_leave_page = "play";
}  // namespace

void ReturnToLauncher(const char* why, const char* page) {
  if (g_leaving.exchange(true)) return;
  g_leave_why = why;
  g_leave_page = page;
  g_leave_since = NowMs();
  g_cover = true;
  kk::RequestUiPaint();
  REXLOG_INFO("KK play: back to the launcher ({})", why);
}

bool CoverScreen() { return g_cover; }

// ===================================================================================== the start itself ===

namespace {

bool g_player_registered = false;
bool g_video_modes_pending = false;  // a loaded save has video modes still to apply (video_modes.h)
bool g_jump_requested = false;  // our level request is waiting for uni_go
bool g_level_loaded = false;    // ... and has been carried out
bool g_cheats_pending = false;
bool g_extra_session = false;
std::atomic<bool> g_extra_movie{false};
int64_t g_extra_movie_since = 0;
int g_pending_save_slot = -1;   // a new save to write once the save system is idle
int64_t g_pending_save_since = 0;
int64_t g_level_loaded_ms = 0;

}  // namespace
bool BonusMovieActive() { return g_extra_movie; }
}  // namespace kk::play

REX_EXTERN(__imp__sub_82413190);  // universe -> save data
REX_EXTERN(__imp__sub_82413080);  // save data -> universe
REX_EXTERN(__imp__sub_8271D698);  // universe re-init (keeps the saved variables)
REX_EXTERN(__imp__sub_82700A48);  // Press Start: the active pad's player signs in
REX_EXTERN(__imp__sub_827291A0);  // the save system's command entry
REX_EXTERN(__imp__sub_82729138);  // allocates and clears the five slots' file buffer (M+0x14)
REX_EXTERN(__imp__sub_82413978);  // builds a slot's header
REX_EXTERN(__imp__sub_825EAB98);  // what "Create new" calls after the re-init
REX_EXTERN(__imp__sub_827252C8);  // the Extras movie player; player+0x130 stays set during playback
REX_EXTERN(__imp__sub_825EE130);  // applies the original saved video modifiers

namespace kk::play {
namespace {

// Press Start on the title for pad 1: its player becomes the active one (as the title's own Press Start).
void RegisterPlayer(PPCContext& ctx, uint8_t* base) {
  if (g_player_registered) return;
  g_player_registered = true;
  const PPCContext saved = ctx;
  Store32(base, kActivePad, 0);
  __imp__sub_82700A48(ctx, base);
  ctx = saved;
}

// The slot buffer (M+0x14) the save and load jobs use, for this slot.
void PrepareSlotBuffer(PPCContext& ctx, uint8_t* base, int slot) {
  const PPCContext saved = ctx;
  Store32(base, kSaveSlot, uint32_t(slot));
  __imp__sub_82729138(ctx, base);
  ctx = saved;
}

void SetDeviceAndSlot(uint8_t* base, uint32_t g, int slot) {
  Store32(base, g + 0x4E10, uint32_t(slot));  // the save menu page: the chosen slot
  Store32(base, kStorageDevice, 1);           // the storage device the device selector chose
  Store32(base, g + 0x5F08, 1);               // ... as the universe sees it
  Store32(base, g + 0x5F24, 0);
}

// The loaded save's "Old movie" filter / horizontal flip, with the game's own sub_825EE130, once its profile
// pointer and flip target are set (video_modes.h): calling it while the title loads the save crashed.
void ApplyPendingVideoModes(PPCContext& ctx, uint8_t* base) {
  const uint32_t p = Load32(base, kProfilePtr), target = Load32(base, kVideoModeTarget);
  const uint32_t flip = p ? Load32(base, p + 0x4DA0) : 0, old_movie = p ? Load32(base, p + 0x4DA4) : 0;
  switch (NextVideoModeStep(g_video_modes_pending, p, target, flip, old_movie)) {
    case VideoModeStep::kWait: return;
    case VideoModeStep::kNothing: g_video_modes_pending = false; return;
    case VideoModeStep::kApply: break;
  }
  g_video_modes_pending = false;
  const PPCContext saved = ctx;
  __imp__sub_825EE130(ctx, base);
  ctx = saved;
  REXLOG_INFO("KK play: applied the save's video modes (old movie {}, horizontal flip {})", old_movie, flip);
}

// The save's file into the save system and the universe, as choosing it in the save menu does (proven:
// chapter-jump.md, "A jump that loads a save first").
bool LoadSaveFromFile(PPCContext& ctx, uint8_t* base, uint32_t g, int slot) {
  const auto path = SlotFile(g_user_dir, slot);
  const auto data = ReadAll(path);
  if (data.size() != kSaveFileSize || BE32(data.data() + 0x28) == 0) {
    REXLOG_WARN("KK play: slot {} has no save ({}, {} bytes)", slot + 1, path.string(), data.size());
    return false;
  }
  RegisterPlayer(ctx, base);
  PrepareSlotBuffer(ctx, base, slot);
  const uint32_t all = Load32(base, kMemcard + 0x14);
  if (all) std::memcpy(base + all + uint32_t(slot) * kSaveFileSize, data.data(), kSaveFileSize);
  std::memcpy(base + kSaveHeaders + uint32_t(slot) * kHeaderSize, data.data(), kHeaderSize);
  std::memcpy(base + kSaveData, data.data() + kHeaderSize, kDataSize);
  const PPCContext saved = ctx;
  __imp__sub_82413080(ctx, base);  // the save's variables into the universe
  ctx = saved;
  SetDeviceAndSlot(base, g, slot);
  Store32(base, g + 0x4FF8, BE32(data.data() + 0x28));  // its continue point (header +0x28 / +0x2C)
  Store32(base, g + 0x4FFC, BE32(data.data() + 0x2C));
  // The save's video modes are applied once the game can take them (ApplyPendingVideoModes).
  g_video_modes_pending = Load32(base, g + 0x4DA0) || Load32(base, g + 0x4DA4);
  REXLOG_INFO("KK play: loaded save slot {} '{}' (continue {} {})", slot + 1,
              std::string(reinterpret_cast<const char*>(data.data()), strnlen(reinterpret_cast<const char*>(data.data()), 31)),
              int32_t(BE32(data.data() + 0x28)), int32_t(BE32(data.data() + 0x2C)));
  return true;
}

// Sends the save job for `slot` (what the save menu's "Create new" sends) once the save system is idle.
void TrySendPendingSave(PPCContext& ctx, uint8_t* base) {
  if (g_pending_save_slot < 0) return;
  if (Load32(base, kSaveQueue + 0xC) != 0 || g_save_jobs > 0) {
    if (NowMs() - g_pending_save_since > 20000) {
      REXLOG_WARN("KK play: the save system stayed busy for 20 s (status {:08X}); the new save is written at the "
                  "game's first save", Load32(base, kSaveStatus));
      g_pending_save_slot = -1;
    }
    return;
  }
  const int slot = g_pending_save_slot;
  g_pending_save_slot = -1;
  // The header in memory must hold a real continue point (level 0 would make the job delete the save).
  if (Load32(base, kSaveHeaders + uint32_t(slot) * kHeaderSize + 0x28) == 0) {
    REXLOG_ERROR("KK play: the new save's header has no continue point; not written");
    return;
  }
  const PPCContext saved = ctx;
  Store32(base, kSaveSlot, uint32_t(slot));
  ctx.r3.u64 = 0x20000060;
  ctx.r4.u64 = uint32_t(slot);
  __imp__sub_827291A0(ctx, base);
  ctx = saved;
  REXLOG_INFO("KK play: writing the new save in slot {} (the game's save job)", slot + 1);
}

// The save menu's "Create new" (IntMIG_Page_Memcard, sub_824CB738 at 0x824CB7C4): a new game's header
// for the slot, the universe re-init, the continue point 100 / -1, and the slot chosen; then the save
// job writes it (sent once the save system is idle).
void CreateNewSave(PPCContext& ctx, uint8_t* base, uint32_t g, int slot, const std::string& name) {
  RegisterPlayer(ctx, base);
  PrepareSlotBuffer(ctx, base, slot);
  const uint32_t header = kSaveHeaders + uint32_t(slot) * kHeaderSize;
  std::memset(base + header, 0, kHeaderSize);
  // The name (header +0x00, 31 characters; the save menu's own keyboard types it letter by letter with
  // sub_82413040). ASCII only: the game's font has no other letters.
  std::string ascii;
  for (unsigned char c : name)
    if (c >= 0x20 && c < 0x7F) ascii += char(c);
  if (ascii.empty()) ascii = fmt::format("Player {}", slot + 1);
  std::memcpy(base + header, ascii.data(), std::min<size_t>(ascii.size(), 31));
  const PPCContext saved = ctx;
  ctx.r3.u64 = uint32_t(slot);
  ctx.r4.u64 = 2;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = kNewGameLevel;
  ctx.r7.u64 = uint32_t(-1);
  ctx.r8.u64 = 0;
  __imp__sub_82413978(ctx, base);
  ctx = saved;
  ctx.r3.u64 = Load32(base, kUniverseScript);
  __imp__sub_8271D698(ctx, base);
  ctx = saved;
  // sub_825EAB98 reads the universe through the script globals (G and P), which are only set once the
  // start menu has run: point them at the universe for the call, as they are there.
  const uint32_t old_g = Load32(base, kGamePtr), old_p = Load32(base, kProfilePtr);
  if (!old_g) Store32(base, kGamePtr, g);
  if (!old_p) Store32(base, kProfilePtr, g);
  __imp__sub_825EAB98(ctx, base);
  ctx = saved;
  Store32(base, kGamePtr, old_g);
  Store32(base, kProfilePtr, old_p);
  __imp__sub_82413190(ctx, base);  // the universe's saved variables into the save data
  ctx = saved;
  Store32(base, g + 0x4FFC, uint32_t(-1));
  Store32(base, g + 0x4FF8, kNewGameLevel);
  SetDeviceAndSlot(base, g, slot);
  g_pending_save_slot = slot;
  g_pending_save_since = NowMs();
  REXLOG_INFO("KK play: created a new save '{}' in slot {} (header level {} start {})", ascii, slot + 1,
              int32_t(Load32(base, header + 0x28)), int32_t(Load32(base, header + 0x2C)));
}

// What the load page (IntMIG_Page_load, sub_824B4DF0) does with a level and start point.
void RequestLevel(uint8_t* base, uint32_t g, uint32_t level, uint32_t start) {
  Store32(base, g + 8, level);
  Store32(base, g + 0xC, start);
  Store32(base, kLoadFlag, 1);
  Store32(base, g + 0x5F1C, 0);
  Store32(base, g + 4, 1);
  g_jump_requested = true;
}

// Continue the loaded save at its continue point, as the main menu's Play does.
void StartStory(uint8_t* base, uint32_t g) {
  const uint32_t level = Load32(base, g + 0x4FF8);
  int32_t start = int32_t(Load32(base, g + 0x4FFC));
  if (level == kNewGameLevel && start == -1) start = 0;  // as the load page does
  Store32(base, g + 0x5668, 0);                          // not a chapter-select run
  if (level == kNewGameLevel && start == 0) Store32(base, g + 0x1C, 1);  // the load page: a new game
  RequestLevel(base, g, level, uint32_t(start));
  REXLOG_INFO("KK play: starting the story at level {} start {}", level, start);
}

// Pressing A on chapter-select entry `idx` (IntMIG_Page_Map, sub_824BD150, for an entry other than the
// one in progress), then the load page. The universe re-init resets the slot, the device and the
// continue point: they are set again from the save's header afterwards.
void StartChapter(PPCContext& ctx, uint8_t* base, uint32_t g, int idx, int slot) {
  const PPCContext saved = ctx;
  __imp__sub_82413190(ctx, base);                               // snapshot of the universe variables
  ctx = saved;
  std::memmove(base + 0x82C6BDE0, base + kSaveData, kDataSize);  // the chapter-select run's copies
  std::memmove(base + 0x82C73D20, base + kSaveHeaders, 0x500);
  ctx.r3.u64 = Load32(base, kUniverseScript);
  __imp__sub_8271D698(ctx, base);
  ctx = saved;
  __imp__sub_82413190(ctx, base);
  ctx = saved;
  if (slot >= 0) {
    SetDeviceAndSlot(base, g, slot);
    const uint32_t header = kSaveHeaders + uint32_t(slot) * kHeaderSize;
    Store32(base, g + 0x4FF8, Load32(base, header + 0x28));
    Store32(base, g + 0x4FFC, Load32(base, header + 0x2C));
  }
  Store32(base, kChapterNumber, Load32(base, g + (0x149B + uint32_t(idx)) * 4));
  Store32(base, g + 0x5668, 1);  // started from the chapter select
  Store32(base, g + 0x5670, uint32_t(idx));
  const uint32_t level = Load32(base, g + (0x1402 + uint32_t(idx)) * 4), start = Load32(base, g + (0x1435 + uint32_t(idx)) * 4);
  RequestLevel(base, g, level, start);
  REXLOG_INFO("KK play: starting chapter-select entry {} (level {} start {})", idx, int32_t(level), int32_t(start));
}

// The port's chapter list against the game's own table (uni_init fills it at boot).
void CheckChapterTable(const uint8_t* base, uint32_t g) {
  const int count = int(Load32(base, g + 0x5000));
  int wrong = 0;
  for (const auto& c : Chapters()) {
    const int key = int32_t(Load32(base, g + (0x1402 + uint32_t(c.entry)) * 4));
    const int start = int32_t(Load32(base, g + (0x1435 + uint32_t(c.entry)) * 4));
    if (key != c.level_key || start != c.start_point) {
      ++wrong;
      REXLOG_WARN("KK play: chapter entry {}: the game has level {} start {}, the port {} {}", c.entry, key, start,
                  c.level_key, c.start_point);
    }
  }
  REXLOG_INFO("KK play: the game's chapter table has {} entries; {} differ from the port's", count, wrong);
}

void CarryOut(PPCContext& ctx, uint8_t* base, uint32_t g) {
  StartRequest r = GetStartRequest();
  g_starting = false;
  g_session = true;
  g_chapter_session = r.kind == StartKind::kChapter;
  g_extra_session = r.kind == StartKind::kExtra;
  CheckChapterTable(base, g);
  if (!BackupSaves(g_user_dir, fmt::format("before play ({})", KindName(r.kind)))) {
    REXLOG_ERROR("KK play: the saves could not be backed up before play");
    if (g_extra_session) {
      ReturnToLauncher("the bonus save backup failed", "extras");
      return;
    }
  }
  const auto slots = ListSaves(g_user_dir);
  if (g_extra_session) {
    const auto* extra = FindExtra(r.chapter);
    auto progress = ReadExtraProgress(g_user_dir, r.slot);
    progress.museum |= rex::cvar::GetFlagByName("kk_cheats") == "true" &&
                       rex::cvar::GetFlagByName("kk_cheat_bonus") == "true";
    if (!extra || extra->type == ExtraType::kMode || !ExtraUnlocked(*extra, progress)) {
      REXLOG_WARN("KK extras: refused unavailable bonus {}", r.chapter);
      ReturnToLauncher("the bonus is locked or unavailable", "extras");
      return;
    }
    if (r.slot >= 0 && r.slot < kSlotCount && slots[size_t(r.slot)].exists) {
      if (!LoadSaveFromFile(ctx, base, g, r.slot)) {
        ReturnToLauncher("the bonus save could not be loaded", "extras");
        return;
      }
    } else {
      RegisterPlayer(ctx, base);
      Store32(base, g + 0x4E10, uint32_t(-1));
    }
    const PPCContext saved = ctx;
    if (const auto selection = ExtraSelection(extra->id)) Store32(base, g + 0x5680, selection);
    if (extra->type == ExtraType::kMovie) {
      // Exactly the original Extras movie call. Its worker clears player+0x130 when finished.
      ctx.r3.u64 = Load32(base, 0x82CCAE80);
      ctx.r4.u64 = extra->level;
      REXLOG_INFO("KK extras: playing movie {}", extra->id);
      g_extra_movie = true;
      g_extra_movie_since = NowMs();
#if defined(KK_DEV_TOOLS)
      kk::DevStartScriptClock();
#endif
      __imp__sub_827252C8(ctx, base);
      ctx = saved;
      return;
    }
    __imp__sub_82413190(ctx, base);
    ctx = saved;
    std::memmove(base + 0x82C6BDE0, base + kSaveData, kDataSize);
    std::memmove(base + 0x82C73D20, base + kSaveHeaders, 0x500);
    if (extra->session_flag) Store32(base, g + 0x5674, extra->session_flag);
    else {
      for (uint32_t offset : {0x4DDCu, 0x4DD8u, 0x3348u, 0x3394u, 0x33E0u}) Store32(base, g + offset, 0);
      __imp__sub_82413190(ctx, base);
      ctx = saved;
    }
    RequestLevel(base, g, extra->level, extra->start_point);
    REXLOG_INFO("KK extras: starting bonus {} level {} start {}", extra->id, extra->level, extra->start_point);
    return;
  }
  if (r.slot < 0 || r.slot >= kSlotCount) {
    // No slot given: Continue takes the most recent save, the others the first empty slot.
    r.slot = MostRecentSave(slots);
    if (r.kind != StartKind::kContinue || r.slot < 0) {
      r.slot = 0;
      for (const auto& s : slots)
        if (!s.exists) {
          r.slot = s.index;
          break;
        }
      if (r.kind == StartKind::kContinue) r.kind = StartKind::kNewGame;
    }
  }
  const bool has_save = slots[size_t(r.slot)].exists;
  REXLOG_INFO("KK play: carrying out {} in slot {} ({}), chapter {}", KindName(r.kind), r.slot + 1,
              has_save ? "a save" : "empty", r.chapter);
  switch (r.kind) {
    case StartKind::kContinue:
      if (has_save && LoadSaveFromFile(ctx, base, g, r.slot)) {
        StartStory(base, g);
        break;
      }
      [[fallthrough]];
    case StartKind::kNewGame:
      CreateNewSave(ctx, base, g, r.slot, r.new_name);
      StartStory(base, g);
      break;
    case StartKind::kChapter: {
      if (!has_save || !LoadSaveFromFile(ctx, base, g, r.slot)) CreateNewSave(ctx, base, g, r.slot, r.new_name);
      StartChapter(ctx, base, g, std::clamp(r.chapter, 0, kChapterCount - 1), r.slot);
      break;
    }
    default:
      break;
  }
  g_cheats_pending = true;  // the launcher's cheats, once the level is up (and watched for changes)
}

// The title screen: uni_go has loaded the title's level (2) and run a few frames since.
int g_title_frames = -1, g_table_frames = 0;

}  // namespace
}  // namespace kk::play

namespace kk::play {
bool OnGameMenuPage(const char* page) {
  if (benchmark::Active()) { benchmark::Interrupted(); return true; }
  if (!g_session) return false;
  if (g_extra_movie) return true;
  if (!g_leaving) {
    // A chapter-select run that has ended (its Score pages and save question are over) goes back to
    // Chapter select; anything else to the Play menu.
    const bool chapter_run = g_chapter_session && !LeftByQuit();
    REXLOG_INFO("KK play: the game opened its menu page {}", page);
#if defined(KK_DEV_TOOLS)
    // The start menu's texts (the chapter names, if they are text) with KK_DEV_TEXT_DUMP.
    if (std::getenv("KK_DEV_TEXT_DUMP") && kk::g_guest_base) kk::DevDumpTexts(kk::g_guest_base, "texts-menu.txt");
#endif
    ReturnToLauncher(chapter_run ? "the chapter-select run is over" : "the game left play",
                     g_extra_session ? "extras" : chapter_run ? "chapters" : "play");
  }
  return true;
}
}  // namespace kk::play

// The chapter select page (IntMIG_Page_Map): never shown after a launcher start.
REX_EXTERN(__imp__sub_824BD150);
REX_HOOK_RAW(sub_824BD150) {
  if (kk::play::OnGameMenuPage("IntMIG_Page_Map")) return;
  __imp__sub_824BD150(ctx, base);
}

// The save routine (sub_8266AD80, mode in r3): builds the header from the universe's continue point
// G+0x4FF8 / +0x4FFC and opens the save pages. A continue level of 0 would make the save job delete the
// save: put back the slot's own continue point (from the save system's header, else its file), or the
// new-game start, before it runs.
REX_EXTERN(__imp__sub_8266AD80);
REX_HOOK_RAW(sub_8266AD80) {
  using namespace kk::play;
  const uint32_t g = Load32(base, 0x82CC8758);
  if (g) {
    const int32_t mode = int32_t(ctx.r3.u32), slot = int32_t(Load32(base, g + 0x4E10));
    // Modes 0 and 1 take the current level, 4 the end; 2 and 3 keep G+0x4FF8.
    const bool keeps = mode == 2 || mode == 3;
    if (slot >= 0 && slot < kSlotCount && keeps && Load32(base, g + 0x4FF8) == 0) {
      uint32_t level = Load32(base, kSaveHeaders + uint32_t(slot) * kHeaderSize + 0x28);
      uint32_t start = Load32(base, kSaveHeaders + uint32_t(slot) * kHeaderSize + 0x2C);
      if (level == 0) {
        const auto data = ReadAll(SlotFile(g_user_dir, slot));
        if (data.size() == kSaveFileSize) level = BE32(data.data() + 0x28), start = BE32(data.data() + 0x2C);
      }
      if (level == 0) level = kNewGameLevel, start = uint32_t(-1);
      Store32(base, g + 0x4FF8, level);
      Store32(base, g + 0x4FFC, start);
      REXLOG_WARN("KK play: save guard: the continue point was 0 (save mode {}); set to {} {}", mode,
                  int32_t(level), int32_t(start));
    }
  }
  __imp__sub_8266AD80(ctx, base);
}

// The save job (sub_82722620, the save system's worker thread): with a header whose continue level is 0
// it deletes the save (sub_82721970, XamContentDelete) instead of writing it. The port never lets that
// happen to a save on disk: the header gets that save's own continue point back first (and the job then
// writes the game's data as usual). With no save on disk there is nothing to lose.
REX_EXTERN(__imp__sub_82722620);
REX_HOOK_RAW(sub_82722620) {
  using namespace kk::play;
  const uint32_t slot = Load32(base, kSaveSlot);
  const uint32_t header = kSaveHeaders + slot * kHeaderSize;
  if (slot < uint32_t(kSlotCount) && Load32(base, header + 0x28) == 0) {
    const auto data = ReadAll(SlotFile(g_user_dir, int(slot)));
    if (data.size() == kSaveFileSize && BE32(data.data() + 0x28) != 0) {
      Store32(base, header + 0x28, BE32(data.data() + 0x28));
      Store32(base, header + 0x2C, BE32(data.data() + 0x2C));
      REXLOG_WARN("KK play: save guard: slot {} was about to be saved with continue level 0 (the game would "
                  "delete it); its own continue point {} {} put back", slot + 1, int32_t(BE32(data.data() + 0x28)),
                  int32_t(BE32(data.data() + 0x2C)));
    }
  }
  ++g_save_jobs;
  __imp__sub_82722620(ctx, base);
  --g_save_jobs;
}

// uni_go (sub_8253DB80): the universe's script function that performs a requested level change, every
// frame. The start request is carried out here on the title screen; the start menu is turned into the
// launcher.
REX_EXTERN(__imp__sub_8253DB80);
REX_HOOK_RAW(sub_8253DB80) {
  using namespace kk::play;
  const uint32_t ai = Load32(base, kAiPtr);
  const uint32_t g = ai ? Load32(base, ai + 0x40) : 0;
  if (g) {
    if (const int chapter = kk::benchmark::NextChapter(); chapter >= 0) {
      g_level_loaded = false;
      g_cover = true;
      g_cheats_pending = true;
      StartChapter(ctx, base, g, chapter, 0);
    }
    const bool request = Load32(base, g + 4) != 0;
    const uint32_t level = Load32(base, g + 8);
    // The title's own level request; the start is carried out a few frames after it has loaded.
    if (g_starting && request && level == kTitleLevel && g_title_frames < 0) {
      g_title_frames = 0;
      REXLOG_INFO("KK play: the title screen's level is loading");
    }
    if (g_starting && !request) {
      const bool table = Load32(base, g + 0x5000) == uint32_t(kChapterCount);
      if (table) ++g_table_frames;
      if (g_title_frames >= 0) ++g_title_frames;
      // Normally 5 frames after the title has loaded; 90 frames (3 s) with the chapter table filled
      // as a fallback, should the title's request not have been seen.
      if (table && (g_title_frames > 5 || g_table_frames > 90)) {
        if (g_title_frames <= 5) REXLOG_WARN("KK play: no title level request seen; starting anyway");
        CarryOut(ctx, base, g);
        TrySendPendingSave(ctx, base);  // before the load (a story start's load plays the Intro movie)
      }
    }
    // In a session the launcher started, the start menu (level 3) means: back to the launcher.
    if (g_session && Load32(base, g + 4) != 0 && Load32(base, g + 8) == kStartMenuLevel) {
      Store32(base, g + 4, 0);
      if (g_extra_movie) {
        // A movie runs over the title level; keep its menus covered until playback ends.
      } else if (kk::benchmark::Active()) {
        kk::benchmark::Interrupted();
      } else {
        // A chapter-select run that has reached its end goes back to Chapter select; Quit game to the Play menu.
        const bool chapter_end = g_chapter_session && !LeftByQuit();
        ReturnToLauncher(chapter_end ? "the chapter-select run is over" : "the game left play",
                         g_extra_session ? "extras" : chapter_end ? "chapters" : "play");
      }
    }
  }
  const bool request = g && Load32(base, g + 4) != 0;
  if (request && g_level_loaded) kk::benchmark::Interrupted();
  if (request && g_jump_requested) REXLOG_INFO("KK play: uni_go loads level {}", int32_t(Load32(base, g + 8)));
  const bool ps2_movie = request && kk::movies::BeforeLoad(base, g, g_starting, g_leaving);
  __imp__sub_8253DB80(ctx, base);
  if (g_extra_movie) {
    const uint32_t player = Load32(base, 0x82CCAE80);
    if (player && Load32(base, player + 0x130) &&
        (Load32(base, player + 0x10C) || Load32(base, player + 0x110)) && g_cover) {
      g_cover = false;
      kk::RequestUiPaint();
    }
    if (NowMs() - g_extra_movie_since > 250 && (!player || Load32(base, player + 0x130) == 0)) {
      g_extra_movie = false;
      ReturnToLauncher("the bonus movie finished", "extras");
    }
  }
  if (ps2_movie) REXLOG_INFO("PS2 movie: original level loader returned, current {} requested {}",
                            Load32(base, g + 0x18), Load32(base, g + 8));
  ApplyPendingVideoModes(ctx, base);
  ServiceProfileOptions(ctx, base);
#if defined(KK_DEV_TOOLS)
  if (g && std::getenv("KK_DEV_AUDIO_TRACE")) {
    static bool traced = false;
    const uint32_t script = Load32(base, 0x828E838C);
    const uint32_t object = script ? Load32(base, script) : 0;
    const uint32_t desc = object ? Load32(base, object) : 0;
    if (!traced && desc) {
      traced = true;
      uint32_t offset = 0x100;
      const uint32_t table = Load32(base, desc + 8), count = Load32(base, desc + 12);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t d = table + i * 16, variable = Load32(base, d), shape = Load32(base, d + 8);
        const uint32_t type = (uint32_t(base[d + 12]) << 8) | base[d + 13];
        const uint32_t flags = (uint32_t(base[d + 14]) << 8) | base[d + 15];
        const uint32_t size = (uint32_t(base[0x828CEFF0 + type * 8 + 2]) << 8) |
                             base[0x828CEFF0 + type * 8 + 3];
        const uint32_t bytes = ((shape >> 30) * 4) + (shape & 0x3FFFFFFF) * size;
        if (variable >= 18680 && variable <= 19860)
          REXLOG_INFO("KK audio descriptor: G+{:X} file+{:X} bytes {} flags {:X} value {:08X}",
                      variable, offset, bytes, flags, Load32(base, g + variable));
        if (flags & 0x10) offset += bytes;
      }
    }
  }
#endif
  if (request && g_jump_requested && !g_level_loaded) {
    g_level_loaded = true;
    g_level_loaded_ms = NowMs();
    g_cover = false;
    kk::RequestUiPaint();
    REXLOG_INFO("KK play: the start request's level is loaded");
    kk::benchmark::LevelLoaded();
#if defined(KK_DEV_TOOLS)
    kk::DevStartScriptClock();
#endif
  }
  if (request) ResetPause();
  if (g_cheats_pending && g_level_loaded && ApplyCheatsNow(base)) g_cheats_pending = false;
  TrySendPendingSave(ctx, base);
  // Going back to the launcher: once the save system is idle (a save in progress is written).
  if (g_leaving) {
    static bool done = false;
    // No save job writing (the job runs on the save system's worker thread) and none waiting to be sent.
    const bool idle = g_save_jobs == 0 && g_pending_save_slot < 0;
    const int64_t waited = NowMs() - g_leave_since;
    if (!done && ((idle && waited > 500) || waited > 10000)) {
      done = true;
      if (!idle) REXLOG_WARN("KK play: a save is still being written after 10 s; leaving anyway");
      const std::wstring page(g_leave_page.begin(), g_leave_page.end());
      if (QuitToDesktop()) {
        REXLOG_INFO("KK play: Quit game: the program exits");
        if (g_quit) g_quit();
      } else if (RelaunchWith(L"--kk_launcher_open=" + page) && g_quit) {
        // The game's black cover stays up until the launcher has drawn its first frame (handoff.h).
        kk::handoff::QuitWhenChildShown(g_quit);
      }
    }
  }
#if defined(KK_DEV_TOOLS)
  kk::DevUniGoTick(base, g, request, g_level_loaded);
#endif
}

#if defined(KK_DEV_TOOLS)
// KK_DEV_SAVES_TEST="<user data folder>[|delete=<slot>]": tests the save functions on that folder (a private
// copy) without the game, logs what they return, writes saves_test.txt there and quits the program.
namespace kk::play {
namespace {
const bool g_saves_test = [] {
  const char* v = std::getenv("KK_DEV_SAVES_TEST");
  if (!v || !*v) return false;
  std::string arg = v;
  int del = -1;
  if (const size_t at = arg.find("|delete="); at != std::string::npos) {
    del = std::atoi(arg.c_str() + at + 8);
    arg.resize(at);
  }
  const std::filesystem::path dir = arg;
  std::ofstream out(dir / "saves_test.txt");
  auto list = [&](const char* when) {
    const auto slots = ListSaves(dir);
    out << when << ": most recent " << MostRecentSave(slots) << "\n";
    for (const auto& s : slots)
      out << fmt::format("  slot {} exists {} name '{}' last {} created {} reached {} continue {} {} (entry {}) score8 {}\n",
                         s.index, s.exists, s.name, s.last_saved, s.created, s.chapter_reached, s.continue_level,
                         s.continue_start, s.continue_chapter, s.best_scores[8]);
  };
  list("before");
  out << "backup 1: " << BackupSaves(dir, "test 1") << "\n";
  out << "backup 2 (unchanged): " << BackupSaves(dir, "test 2") << "\n";
  if (del >= 0) {
    out << "delete slot " << del << ": " << DeleteSave(dir, del) << "\n";
    list("after delete");
  }
  out << "chapters: " << Chapters().size() << ", entry 8 '" << Chapters()[8].name << "' level " << Chapters()[8].level_key
      << "\n";
  out << "request round trip: " << FormatRequest(ParseRequest("new:2:My%20Save%3A1")) << " name '"
      << ParseRequest("new:2:My%20Save%3A1").new_name << "'\n";
  out.close();
  std::exit(0);
}();
}  // namespace
}  // namespace kk::play
#endif
