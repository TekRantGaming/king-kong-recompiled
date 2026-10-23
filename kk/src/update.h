// Updates from the port's GitHub releases.
//
// When the launcher opens it asks GitHub for the latest release (unless
// kk_check_updates is off). If it is newer than this build, the launcher offers
// to install it. Windows (1.9.13, the new updater from the Beta 17 line): the release zip is downloaded and checked
// against the release's SHA256SUMS.txt (and its size), unpacked, and every file
// its kk-files.txt lists is installed (update_files.h) except protected paths:
// staged beside the old ones, then swapped in, with every old file put back if a
// swap fails. Files kk-retired.txt lists are removed only while unchanged; any
// other file is never touched. A release without kk-files.txt (1.9.12 and
// older, when changing version) is installed with the old folder list. Then the
// launcher restarts. The game folder, saves and settings are never touched.
// Linux is unchanged: the AppImage is replaced as one file.
//
// The first start of a version (FinishUpdate) compares the install with
// kk-files.txt: an update by an older updater may have left files out. Missing
// or wrong files are fetched by downloading this version's release again,
// quietly on failure.
//
// Two channels (kk_update_channel). Stable follows the main repo's latest
// release, which is only ever a stable version: v2.0.0 is offered once it is
// published there. Beta also follows the beta repo, which holds only beta builds;
// whichever of the two is newer wins. Until the beta repo has a release, beta
// gets stable builds.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kk {

constexpr const char* kReleasesApi = "https://api.github.com/repos/TekRantGaming/king-kong-recompiled/releases/latest";
constexpr const char* kBetaReleasesApi =
    "https://api.github.com/repos/TekRantGaming/king-kong-recompiled-beta/releases/latest";

struct UpdateInfo {
  std::string version;   // "1.4.1" (from the tag "v1.4.1")
  std::string page_url;  // the release page (what's new)
  std::string zip_url;   // this platform's download: the Windows zip, or the Linux AppImage
  uint64_t zip_size = 0;  // its size as the release lists it (0: unknown)
  std::string sums_url;   // the release's SHA256SUMS.txt ("" when it has none: stable 1.9.12 and older)
};

// What the update screen shows below the bar.
enum class UpdatePhase : int { kDownloading, kChecking, kInstalling };

struct UpdateStatus {
  std::atomic<bool> busy{false}, done{false}, failed{false};
  std::atomic<uint64_t> bytes{0}, total{0};  // download progress
  std::atomic<int> phase{0};                 // UpdatePhase
  std::optional<UpdateInfo> found;           // set by CheckForUpdate when newer
  std::string message;                       // read once done or failed (English; {0} is `detail`)
  std::string detail;                        // the file a failed install names, else ""
};

// The first start check (FinishUpdate). The launcher shows the update screen only while `downloading`.
struct FinishStatus {
  std::atomic<bool> busy{false}, downloading{false}, done{false}, failed{false};
  std::atomic<uint64_t> bytes{0}, total{0};
  std::atomic<int> phase{0};   // UpdatePhase
  std::atomic<int> needed{0};  // files missing or wrong
  std::atomic<int> installed{0};
};

// True when version `a` ("1.4.1", "2.0.0-beta.2") is newer than `b`. A beta
// comes before the release of the same number: 2.0.0-beta.2 < 2.0.0.
bool IsNewerVersion(const std::string& a, const std::string& b);

// Asks GitHub for the newest release on `channel` ("stable" or "beta"); sets
// `status.found` if it is newer than this build. With `switching`, it is set
// whenever it differs from this build, older too: moving from a beta back to
// stable can mean going down a version. Runs on the calling thread (use a worker).
void CheckForUpdate(UpdateStatus& status, const std::string& channel, bool switching = false);

// Folders an update must never write or delete in, besides game\, logs\ and the rest of the fixed list
// (update_files.h Protected): the launcher passes the user and game data folders; any inside the program
// folder are kept out of InstallUpdate and FinishUpdate.
void SetProtectedFolders(std::vector<std::filesystem::path> folders);

// Downloads `info` and installs it beside the running program. On success the
// caller restarts the program. Runs on the calling thread (use a worker).
// Developer builds: KK_DEV_UPDATE_ZIP=<a release zip on this PC> is offered by
// CheckForUpdate as version KK_DEV_UPDATE_VERSION (default 2.0.0) and installed
// from that file (checked against a SHA256SUMS.txt beside it when there is one).
void InstallUpdate(const UpdateInfo& info, UpdateStatus& status);

// Removes the *.old files a previous update left behind (they were in use then).
void CleanUpAfterUpdate();

// The first start check: see the top of this file. Removes the retired files that are unchanged,
// then compares the install with kk-files.txt (sizes, hashes of small files). Missing or wrong files: downloads
// this version's release again (KK_DEV_UPDATE_ZIP in developer builds), checks it and installs just those
// files. Failure is only logged; the next start tries again. A passed check is remembered in `state_file`
// (the list's hash and the program folder) so later starts skip it. Windows only (Linux: nothing to do).
// Runs on the calling thread (use a worker).
void FinishUpdate(FinishStatus& status, const std::filesystem::path& state_file);

}  // namespace kk
