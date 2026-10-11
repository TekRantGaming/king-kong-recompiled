// Updates from the port's GitHub releases.
//
// When the launcher opens it asks GitHub for the latest release (unless
// kk_check_updates is off). If it is newer than this build, the launcher offers
// to install it: the release zip is downloaded and unpacked, its files are
// staged beside the old ones, then swapped in, and the launcher restarts.
// The game folder, saves and settings are never touched.
//
// Two channels (kk_update_channel). Stable follows the main repo's latest
// release. Beta also follows the beta repo, which holds only beta builds, so the
// main repo's Releases page only ever shows stable versions; whichever of the
// two is newer wins. Until the beta repo has a release, beta gets stable builds.

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace kk {

constexpr const char* kReleasesApi = "https://api.github.com/repos/TekRantGaming/king-kong-recompiled/releases/latest";
constexpr const char* kBetaReleasesApi =
    "https://api.github.com/repos/TekRantGaming/king-kong-recompiled-beta/releases/latest";

struct UpdateInfo {
  std::string version;   // "1.4.1" (from the tag "v1.4.1")
  std::string page_url;  // the release page (what's new)
  std::string zip_url;   // this platform's download: the Windows zip, or the Linux AppImage
};

struct UpdateStatus {
  std::atomic<bool> busy{false}, done{false}, failed{false};
  std::atomic<uint64_t> bytes{0}, total{0};  // download progress
  std::optional<UpdateInfo> found;           // set by CheckForUpdate when newer
  std::string message;                       // read once done or failed
};

// True when version `a` ("1.4.1", "2.0.0-beta.2") is newer than `b`. A beta
// comes before the release of the same number: 2.0.0-beta.2 < 2.0.0.
bool IsNewerVersion(const std::string& a, const std::string& b);

// Asks GitHub for the newest release on `channel` ("stable" or "beta"); sets
// `status.found` if it is newer than this build. With `switching`, it is set
// whenever it differs from this build, older too: moving from a beta back to
// stable can mean going down a version. Runs on the calling thread (use a worker).
void CheckForUpdate(UpdateStatus& status, const std::string& channel, bool switching = false);

// Downloads `info` and installs it beside the running program. On success the
// caller restarts the program. Runs on the calling thread (use a worker).
void InstallUpdate(const UpdateInfo& info, UpdateStatus& status);

// Removes the *.old files a previous update left behind (they were in use then).
void CleanUpAfterUpdate();

}  // namespace kk
