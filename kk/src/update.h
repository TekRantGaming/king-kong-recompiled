// Updates from the port's GitHub releases.
//
// When the launcher opens it asks GitHub for the latest release (unless
// kk_check_updates is off). If it is newer than this build, the launcher offers
// to install it: the release zip is downloaded and unpacked, its files are
// staged beside the old ones, then swapped in, and the launcher restarts.
// The game folder, saves and settings are never touched.

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace kk {

constexpr const char* kReleasesApi = "https://api.github.com/repos/TekRantGaming/king-kong-recompiled/releases/latest";

struct UpdateInfo {
  std::string version;   // "1.4.1" (from the tag "v1.4.1")
  std::string page_url;  // the release page (what's new)
  std::string zip_url;   // the Windows zip
};

struct UpdateStatus {
  std::atomic<bool> busy{false}, done{false}, failed{false};
  std::atomic<uint64_t> bytes{0}, total{0};  // download progress
  std::optional<UpdateInfo> found;           // set by CheckForUpdate when newer
  std::string message;                       // read once done or failed
};

// True when version `a` ("1.4.1") is newer than `b`.
bool IsNewerVersion(const std::string& a, const std::string& b);

// Asks GitHub for the latest release; sets `status.found` if it is newer than
// this build. Runs on the calling thread (use a worker).
void CheckForUpdate(UpdateStatus& status);

// Downloads `info` and installs it beside the running program. On success the
// caller restarts the program. Runs on the calling thread (use a worker).
void InstallUpdate(const UpdateInfo& info, UpdateStatus& status);

// Removes the *.old files a previous update left behind (they were in use then).
void CleanUpAfterUpdate();

}  // namespace kk
