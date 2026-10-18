// Content downloads (owner, 17 Oct 2026; codex/state/setup-flow.md "Downloads"): the HD texture pack and the
// restored PS2 cutscenes. The launcher downloads the owner's ZIP over HTTPS (WinHTTP on Windows, curl on
// Linux), continues it after an interruption or a restart (Range from the kept .part file), checks its
// SHA-256, unpacks it into the user folder (content\hd-textures, content\ps2-cutscenes) and turns the
// content on through the settings. "I already have a texture pack" installs a ZIP the player chooses the
// same way (checked like the loader checks packs). Remove deletes it and puts the settings back.
//
// Everything runs on one worker thread; the UI thread only reads Status() and applies TakeChanges() (the
// settings), so it goes on while the launcher is minimised (task 173 heartbeat runs TickState). Nothing
// here touches the settings or the SDK: the tests build it on its own (tests/content_tests.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "content_logic.h"

namespace kk::content {

// kArtbook (Beta 7 Extras, artbook_files.h): the PS2 bonus disc from archive.org, turned into the Weta
// paintings in contentrtbook. It is quiet: it runs in the background without the download pop-ups, strip or
// "Play while downloading" question, never pauses for the game and changes no settings (Quiet()).
// Task 209 (owner, 18 Oct): kUhd, the Kong UHD Textures pack, a Workshop download (a texture pack mod ZIP that
// installs into the mods folder); kImport, the Workshop's Import texture pack / Import mod (and removing a mod
// folder): its status is the progress of the one running now.
enum class Kind { kTexturePack = 0, kCutscenes = 1, kArtbook = 2, kUhd = 3, kImport = 4 };
constexpr int kKindCount = 5;
constexpr bool Quiet(Kind kind) { return kind == Kind::kArtbook; }

struct Info {
  const char* id;         // file names: content\downloads\<id>.part, <id>.state
  const char* folder;     // under content\: where it is installed
  const char* url;
  const char* zip_name;
  uint64_t zip_size;      // the ZIP the owner published (the server's size wins when it differs)
  uint64_t unpacked;      // bytes it unpacks to
  const char* sha256;
};
const Info& InfoFor(Kind kind);

enum class Stage {
  kNone,         // not installed, nothing kept
  kQueued,       // waiting for the job before it
  kStarting,     // asking the server for the size, checking free space
  kDownloading,
  kPaused,       // stopped by the player or by a lost connection: the partial file is kept
  kVerifying,    // SHA-256 of the whole ZIP
  kUnpacking,
  kInstalled,
  kFailed,       // `error` says why; nothing was applied
  kRemoving,
};

enum class Error {
  kNone,
  kConnection,  // lost or no connection (the partial file is kept: Download continues it)
  kServer,      // the server refused or sent something unexpected
  kSpace,       // not enough free space (`space_needed`)
  kChecksum,    // the ZIP did not match its SHA-256 (it was deleted)
  kBadPack,     // the ZIP is not a pack the game can use (`problem`)
  kUnpack,      // the unpacking tool failed or is missing
  kWrite,       // cannot write or delete in the user folder
  kBusy,        // another download or install is running
};

struct Status {
  Stage stage = Stage::kNone;
  Error error = Error::kNone;
  Problem problem = Problem::kNone;
  uint64_t done = 0, total = 0;    // bytes of the current step (download, check, unpack)
  double speed = 0;                // bytes per second (download)
  double seconds_left = -1;        // -1: not known yet
  uint64_t space_needed = 0;       // with kSpace: bytes needed in all
  uint64_t space_free = 0;
  bool installed = false;
  bool imported = false;           // installed from the player's own ZIP
  std::string installed_name;      // the pack folder's name ("kk-4x-usv2"); kUhd / kImport: the mod's name
  std::string mod_id;              // kUhd / kImport: the mod folder it installed as (its kk_mods id)
  ImportTarget target = ImportTarget::kTexturePack;  // kImport: which button started it
  std::filesystem::path folder;    // the folder the settings point at
  bool Busy() const {
    return stage == Stage::kQueued || stage == Stage::kStarting || stage == Stage::kDownloading ||
           stage == Stage::kVerifying || stage == Stage::kUnpacking || stage == Stage::kRemoving;
  }
};

// A setting to change on the UI thread. With `when_name`, only when that setting is `when_value` now
// (Remove puts a folder back only when it still points at the removed content).
// Names starting with '@' are events for the launcher (task 209): "@texture-pack-mod" = a texture pack mod was
// installed (value: its folder, the kk_mods id; it becomes the one texture pack on), "@mod-installed" = a mod
// was installed (value: its id), "@mod-removed" = a mod folder was deleted (value: its id).
struct Change {
  std::string name, value;
  bool reset = false;
  std::string when_name, when_value;
};

// The network part, swappable for tests.
class Fetcher {
 public:
  enum class Result { kComplete, kStopped, kConnection, kServer, kWrite, kRestart };
  virtual ~Fetcher() = default;
  // The file's size (a GET of bytes=0-0). False: no answer (`detail` says why).
  virtual bool Probe(const std::string& url, uint64_t& total, std::string& detail) = 0;
  // Downloads the rest of `url` onto `part` from its current size (`have` follows the file as it grows)
  // until it is `total` bytes. Returns early with kStopped as soon as `stop` is set.
  virtual Result Fetch(const std::string& url, const std::filesystem::path& part, uint64_t total,
                       const std::atomic<bool>& stop, std::atomic<uint64_t>& have) = 0;
  // Testing: lose the connection once after this many bytes of the file (0: never).
  std::atomic<uint64_t> drop_at{0};
};
// WinHTTP on Windows, curl on Linux.
std::unique_ptr<Fetcher> MakeSystemFetcher();

// Unpacks `zip` into the empty folder `into`. `progress` is called about twice a second; returning false
// stops (and fails) the unpacking. Windows: tar.exe (in Windows 10 and 11). Linux: bsdtar, else unzip,
// else python3's zipfile. False when the tool failed or none was found (`detail` says which).
bool UnpackZip(const std::filesystem::path& zip, const std::filesystem::path& into,
               const std::function<bool()>& progress, std::string& detail);

// Kind::kArtbook: how its pictures are checked after they come off the disc (the launcher passes a real
// decoder; without one the JPEG header is checked).
void SetPictureCheck(std::function<bool(const std::filesystem::path&)> check);

// Where each kind is installed: <user data>\content\<folder> (Windows: Documents\king_kong\content; Linux:
// ~/.local/share/king_kong/content, or $XDG_DATA_HOME/king_kong/content: the SDK chooses the user folder).
std::filesystem::path ContentRoot(const std::filesystem::path& user_dir);
std::filesystem::path InstallDir(const std::filesystem::path& user_dir, Kind kind);

class Service {
 public:
  // The launcher's one service (made on first use, never destroyed: its worker may still be running at
  // exit; a download in progress then continues at the next start).
  static Service& Get();
  Service();
  ~Service();  // tests only: stops and joins the worker

  // The user folder; reads what is installed, applies an install whose settings were never applied, and
  // continues a download that was running when the program closed (a paused one stays paused).
  void Init(const std::filesystem::path& user_dir, std::unique_ptr<Fetcher> fetcher = nullptr);
  bool Ready() const;
  void OverrideUrl(Kind kind, std::string url, std::string sha256 = {});  // developer tests, local server

  Status Get(Kind kind) const;
  void Download(Kind kind);  // starts, or continues a paused or failed download
  void Pause(Kind kind);
  // Job J3, Play while a download runs: paused like Pause (the partial file kept), and remembered (also over
  // a restart) so that ResumeAfterPlay continues it when the launcher is back.
  void PauseForPlay(Kind kind);
  bool ResumeAfterPlay();    // continues what PauseForPlay paused; true if anything was continued
  void Cancel(Kind kind);    // stops and deletes the partial download
  void Remove(Kind kind);    // cancels, uninstalls, puts the settings back
  // "I already have a texture pack" (the setup) and the Workshop's Import texture pack / Import mod: the ZIP
  // installs into <user data>/mods as a mod folder of its own (Kind::kImport's status follows it). It never
  // touches anything already installed unless it is the same mod again (same folder and name: an update).
  void Import(const std::filesystem::path& zip, ImportTarget target = ImportTarget::kTexturePack);
  // Deletes a mod folder in <user data>/mods on the worker (Kind::kImport's status, stage kRemoving).
  void RemoveMod(const std::filesystem::path& folder);
  std::vector<Change> TakeChanges();
  bool Idle() const;         // no job running or queued
  Fetcher* fetcher() { return fetcher_.get(); }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<Fetcher> fetcher_;
};

// Optional log lines (the launcher sends them to its log).
void SetLog(std::function<void(bool warning, const std::string& line)> log);

}  // namespace kk::content
