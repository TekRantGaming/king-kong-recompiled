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

  explicit RecoveryState(const std::filesystem::path& root) : directory(root / "renderer-recovery") {}
  static bool Native(const std::string& api) { return api == "vulkan" || api == "d3d12"; }
  static std::string Name(const std::string& api) {
    return api == "vulkan" ? "Vulkan" : api == "d3d12" ? "Direct3D 12" : "Classic";
  }
  bool Load() {
    std::ifstream in(directory / "fallback.state");
    std::string p, s, r;
    int c = 0;
    if (!(in >> p >> s >> c)) return false;
    in.ignore(1, '\n');
    std::getline(in, r);
    if (!Native(p) || (!Native(s) && s != "classic") || c < 0 || c > 1) return false;
    preferred = p; selected = s; crashes = c; reason = r;
    in >> retry_requested;
    return true;
  }
  bool Save() const {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return false;
    std::ofstream out(directory / "fallback.state", std::ios::trunc);
    out << preferred << '\n' << selected << '\n' << crashes << '\n' << reason << '\n' << retry_requested << '\n';
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
    if (!Native(selected)) return false;  // Classic is the end, never a restart loop.
    const std::string failed = selected;
    selected = selected == preferred ? (preferred == "vulkan" ? "d3d12" : "vulkan") : "classic";
    crashes = 0;
    retry_requested = false;
    reason = Name(failed) + " " + why + ". Using " + Name(selected) + " for safe startup.";
    return Save() && Marker();
  }
  bool RecoverCrash() {
    std::ifstream in(directory / "startup.marker");
    std::string api;
    in >> api;
    if (!Native(api) || api != selected) return false;
    if (++crashes >= 2) return Advance("crashed twice before a chapter was reached");
    reason = Name(api) + " stopped before a chapter was reached. Retrying once before switching API.";
    return Save() && Marker();
  }
  bool Retry() {
    selected = preferred; crashes = 0; reason.clear();
    retry_requested = true;
    return Save() && Marker();
  }
};
}  // namespace kk::startup
