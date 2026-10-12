// Updates from the port's GitHub releases.
//
// When the launcher opens it asks GitHub for the latest release (unless
// kk_check_updates is off). If it is newer than this build, the launcher offers
// to install it: the release zip is downloaded and unpacked, its files are
// staged beside the old ones, then swapped in, and the launcher restarts.
// The game folder, saves and settings are never touched.
//
// Two channels (kk_update_channel). Stable follows the main repo's latest
// release. Beta follows the beta repo, which holds only beta builds, so the main
// repo's Releases page only ever shows stable versions. A beta player is offered
// a stable release only when its numbers are at least the running build's (the
// beta's own final release: stable 2.0.0 for 2.0.0-beta.N); a lower stable (a
// 1.9.12 hotfix) is not for them. With no beta release (or no answer from the
// beta repo) a beta player has no update. Change version installs the newest
// release of the channel it switches to (PickUpdate).

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

// The release a check offers, from what each repo has as its latest release (nullopt: none, or no
// answer), for a build of version `current` (pure, so it can be tested: RunUpdatePickTests).
// Not switching: a newer release of the channel; on beta also a newer stable whose numbers are at
// least current's. Switching (Change version): the channel's newest release when it differs from
// current (stable for stable, beta for beta).
std::optional<UpdateInfo> PickUpdate(const std::string& channel, bool switching, const std::string& current,
                                     const std::optional<UpdateInfo>& stable, const std::optional<UpdateInfo>& beta);

#if defined(KK_DEV_TOOLS)
// Developer check of PickUpdate's cases, logged as "KK dev: update pick ... ok / FAILED". Returns
// true when all pass.
bool RunUpdatePickTests();
#endif

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
