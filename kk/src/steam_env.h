#pragma once
// Steam's environment in programs this one starts (Linux; 30 Oct, the 2.0.0 Ally test). When this process starts
// Steam (co-op, steam_runtime.cpp Init), Steam's client writes a Steam game's environment into it: the SDL
// controller hints, SteamVirtualGamepadInfo, its shader cache paths. The game this launcher starts next
// (RelaunchWith) is not run by Steam, so with them its SDL waits for Steam's virtual controllers, which Steam
// only makes for a game it started itself, and the game had no controller at all. RecordBefore/RecordAfter
// note what Steam's start added or changed; ChildEnvironment is this process's environment without it. A game
// Steam started (Game Mode, Add to Steam) had Steam's variables before, so they stay.
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>  // environ (with _GNU_SOURCE, as the build has)

namespace kk::steam_env {

inline std::mutex g_mutex;
inline std::map<std::string, std::string> g_before;                    // the environment before Steam started
inline std::map<std::string, std::optional<std::string>> g_steam_set;  // name -> its value before (none: unset)

inline std::map<std::string, std::string> Snapshot() {
  std::map<std::string, std::string> out;
  for (char** e = environ; e && *e; ++e) {
    const std::string entry = *e;
    const auto eq = entry.find('=');
    if (eq != std::string::npos) out[entry.substr(0, eq)] = entry.substr(eq + 1);
  }
  return out;
}

inline void RecordBefore() {
  std::lock_guard lock(g_mutex);
  if (g_before.empty()) g_before = Snapshot();  // the first start only: a retry would count Steam's as before
}

inline void RecordAfter() {
  std::lock_guard lock(g_mutex);
  for (const auto& [name, value] : Snapshot()) {
    const auto was = g_before.find(name);
    if (was == g_before.end()) g_steam_set[name] = std::nullopt;
    else if (was->second != value) g_steam_set[name] = was->second;
  }
}

// "NAME=value" entries for a program this one starts.
inline std::vector<std::string> ChildEnvironment() {
  std::lock_guard lock(g_mutex);
  std::vector<std::string> out;
  for (const auto& [name, value] : Snapshot()) {
    const auto steam = g_steam_set.find(name);
    if (steam == g_steam_set.end()) out.push_back(name + "=" + value);
    else if (steam->second) out.push_back(name + "=" + *steam->second);
  }
  return out;
}

inline size_t SteamSetCount() {
  std::lock_guard lock(g_mutex);
  return g_steam_set.size();
}

}  // namespace kk::steam_env
#endif
