// The countdown on the launcher's Home screen to the King Kong Remastered
// update (v2.0.0). Its time and title come from the website's launch.json
// (fetched in the background when the launcher opens, cached in the settings
// as kk_launch_utc / kk_launch_title); without it, the built-in time is used.

#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>

namespace kk::countdown {

constexpr const char* kLaunchJsonUrl = "https://kong.tekrantgaming.com/launch.json";
constexpr const char* kBuiltInUtc = "2026-10-30T18:00:00Z";
constexpr const char* kBuiltInTitle = "King Kong Remastered update";
constexpr const char* kInviteUrl = "https://discord.gg/ZUeuCRncb";

// "2026-10-30T18:00:00Z" (seconds optional; "Z" or "+00:00") to seconds since
// 1970 UTC, or nothing if it isn't one (or is far outside the launch's years).
std::optional<int64_t> ParseUtc(const std::string& text);

// What a launch.json says: the time ("utc") and an optional title. Nothing if
// the text isn't a usable launch.json.
struct LaunchInfo {
  int64_t utc = 0;
  std::string utc_text;
  std::string title;  // empty: the built-in title
};
std::optional<LaunchInfo> ParseLaunchJson(const std::string& json);

// Fetches launch.json on a worker thread (short timeouts); `done` is set when
// it has finished, `info` when it was good.
struct Fetch {
  std::atomic<bool> done{false};
  std::optional<LaunchInfo> info;  // read only once done
};
std::shared_ptr<Fetch> StartFetch();

// Seconds since 1970 UTC now (the system clock).
int64_t NowUtc();

// "T- 8d 04:12:33" for the seconds left (never below zero).
std::string CountdownText(int64_t seconds_left);

// The date line for a launch time: "Friday 30 October, 18:00 UK time", plus
// " (19:00 your time)" when the player's own clock differs from UK time (in
// the launcher's language).
std::string DateLine(int64_t utc);

}  // namespace kk::countdown
