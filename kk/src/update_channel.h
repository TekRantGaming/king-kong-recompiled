// Which update channel a build follows (task 358, Beta 17).
//
// The channel comes from the build: CMake's KK_RELEASE_CHANNEL (the kk-stable preset sets "stable";
// otherwise it follows KK_VERSION: a "-beta" version is a beta build) becomes KK_BUILD_CHANNEL.
//
// A stable build follows only the stable repo (TekRantGaming/king-kong-recompiled): its update
// channel is always stable, a "beta" left in the settings file by a beta install goes back to the
// default, and Change version (the switch to beta) is not offered. A beta build keeps the behaviour
// from before: with no channel in the settings file (a stable install, or a fresh one) it follows the
// beta channel; a channel the player chose with Change version stays.
//
// Pure, so it can be tested without a game (launcher_logic_tests.cpp).

#pragma once

#include <string>
#include <string_view>

namespace kk {

#if defined(KK_BUILD_CHANNEL)
inline constexpr std::string_view kBuildChannel = KK_BUILD_CHANNEL;
#elif defined(KK_VERSION)
// Test programs that build settings.cpp without the CMake channel: from the version, as before.
inline constexpr std::string_view kBuildChannel =
    std::string_view(KK_VERSION).find("-beta") != std::string_view::npos ? "beta" : "stable";
#endif

inline bool IsStableBuild(std::string_view build_channel) { return build_channel != "beta"; }

// The update channel setting a build of `build_channel` starts with. `saved` is the value in the
// settings file, empty when it has none.
inline std::string ChannelAtStart(std::string_view build_channel, std::string_view saved) {
  if (IsStableBuild(build_channel)) return "stable";
  if (saved.empty()) return "beta";
  return saved == "beta" ? "beta" : "stable";
}

// Whether the launcher offers Change version (stable <-> beta) on this build.
inline bool OffersChannelChoice(std::string_view build_channel) { return !IsStableBuild(build_channel); }

// Whether a stable version ("1.9.12", "2.0.0") is from before the native renderer (2.0.0): switching a beta
// to such a version resets the beta's own settings (ResetBetaOnlySettings); 2.0.0 and later keep them.
inline bool StableLacksNativeRenderer(std::string_view version) {
  if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) version.remove_prefix(1);
  int major = 0;
  size_t i = 0;
  for (; i < version.size() && version[i] >= '0' && version[i] <= '9'; ++i) major = major * 10 + (version[i] - '0');
  return i > 0 && major < 2;
}

// The channel an update check asks for on this build: a stable build never asks the beta repo,
// whatever it is asked for.
inline std::string CheckChannel(std::string_view build_channel, std::string_view wanted) {
  if (IsStableBuild(build_channel)) return "stable";
  return wanted == "beta" ? "beta" : "stable";
}

}  // namespace kk
