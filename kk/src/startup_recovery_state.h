#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace kk::startup {
// Recovery's state and marker live here, beside its attempt journal. No save or settings file is opened.
struct RecoveryState {
  std::filesystem::path directory;
  std::string preferred = "vulkan", selected, reason;
  int crashes = 0;
  bool retry_requested = false;
  // The launcher has shown this fallback's notice once; it is not forced open for it again.
  bool notice_shown = false;

  explicit RecoveryState(const std::filesystem::path& root) : directory(root / "renderer-recovery") {}
  static bool Native(const std::string& api) {
#if defined(__linux__)
    return api == "vulkan";
#else
    return api == "vulkan" || api == "d3d12";
#endif
  }
  static std::string Name(const std::string& api) {
    return api == "vulkan" ? "Vulkan" : api == "d3d12" ? "DirectX 12" : "Unavailable";
  }
  bool Load() {
    std::ifstream in(directory / "fallback.state");
    std::string p, s, r;
    int c = 0;
    if (!(in >> p >> s >> c)) return false;
    in.ignore(1, '\n');
    std::getline(in, r);
    if (!Native(p) || (!Native(s) && s != "classic" && s != "failed") || c < 0 || c > 1) return false;
    // Old builds exhausted recovery into Classic. Forget that state and its crash marker.
    if (s == "classic") { Clear(); return false; }
    preferred = p; selected = s; crashes = c; reason = r;
    in >> retry_requested;
    in >> notice_shown;  // missing in older files: stays false
    return true;
  }
  bool Save() const {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return false;
    std::ofstream out(directory / "fallback.state", std::ios::trunc);
    out << preferred << '\n' << selected << '\n' << crashes << '\n' << reason << '\n' << retry_requested << '\n' << notice_shown << '\n';
    out.flush();
    return bool(out);
  }
  bool Marker(const std::string& api = {}) const {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return false;
    std::ofstream out(directory / "startup.marker", std::ios::trunc);
    out << api;
    out.flush();
    return bool(out);
  }
  bool Advance(const std::string& why) {
    if (!Native(selected)) return false;
    const std::string failed = selected;
#if defined(__linux__)
    selected = "failed";
#else
    selected = selected == preferred ? (preferred == "vulkan" ? "d3d12" : "vulkan") : "failed";
#endif
    crashes = 0;
    retry_requested = false;
    notice_shown = false;
    reason = Name(failed) + " " + why + ". Using " + Name(selected) + " for safe startup.";
    return Save() && Marker();
  }
  bool RecoverCrash() {
    std::ifstream in(directory / "startup.marker");
    std::string api;
    in >> api;
    if (!Native(api) || api != selected) return false;
    if (++crashes >= 2) return Advance("crashed twice during startup");
    reason = Name(api) + " stopped during startup. Retrying once before switching API.";
    return Save() && Marker();
  }
  bool Retry() {
    selected = preferred; crashes = 0; reason.clear();
    retry_requested = true;
    notice_shown = false;
    return Save() && Marker();
  }
  // The player chose a renderer or graphics API by hand: that choice wins, so recovery forgets its
  // fallback (the state file goes; the attempt journal stays).
  bool Clear() {
    selected.clear(); reason.clear(); crashes = 0;
    retry_requested = false; notice_shown = false;
    std::error_code ec;
    std::filesystem::remove(directory / "fallback.state", ec);
    return !ec && Marker();
  }
};

// The launcher's own start (task 228). The launcher's process never switched API for a crash, so a
// driver that crashed while the launcher started (players on AMD with a stale driver shader cache:
// black window, then back to the desktop) closed the game at every start with no message at all.
// launcher.marker = "<api> <phase> <count>", written when the launcher's process starts and cleared
// once its first frames are shown and no shader preparation runs, or at any clean exit. A marker
// found at the next start = that launcher ended without either (a crash, or the process was killed).
//   phase "window":  before the launcher's first frames (the renderer's own start).
//   phase "shaders": while the launcher prepared shaders (both APIs' pipelines, in this process).
struct LauncherGuard {
  struct Left {
    std::string api, phase;
    int count = 0;  // launcher starts in a row that ended this way (0 = the last one finished)
  };
  std::filesystem::path file;
  explicit LauncherGuard(const std::filesystem::path& recovery_dir) : file(recovery_dir / "launcher.marker") {}

  Left Read() const {
    Left left;
    std::ifstream in(file);
    if (!(in >> left.api >> left.phase >> left.count) || !RecoveryState::Native(left.api) ||
        (left.phase != "window" && left.phase != "shaders") || left.count < 1 || left.count > 100)
      return {};
    return left;
  }
  bool Write(const std::string& api, const std::string& phase, int count) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(file, std::ios::trunc);
    out << api << ' ' << phase << ' ' << count << '\n';
    out.flush();
    return bool(out);
  }
  void Clear() const {
    std::error_code ec;
    std::filesystem::remove(file, ec);
  }
  // The count this start writes: one more when the last start on the same API ended the same way.
  static int Next(const Left& left, const std::string& api) { return left.count > 0 && left.api == api ? left.count + 1 : 1; }
  // The launcher's renderer crashed twice in a row before it showed: switch API, as a game start does.
  static bool SwitchApi(const Left& left, const std::string& api) {
    return left.phase == "window" && left.api == api && left.count >= 2;
  }
};
}  // namespace kk::startup
