#include "update.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "http.h"
#include "platform.h"

namespace kk {
namespace {

namespace fs = std::filesystem;

// The folders an update may write to, relative to the program (never game/).
const char* const kUpdatedDirs[] = {"", "glyphs", "launcher_art"};

// The string value of the first "key": "value" in a JSON text (no escapes needed for ours).
std::string JsonString(const std::string& json, const std::string& key, size_t from = 0, size_t* at = nullptr) {
  const std::string needle = "\"" + key + "\"";
  size_t k = json.find(needle, from);
  while (k != std::string::npos) {
    size_t c = json.find_first_not_of(" \t\r\n", k + needle.size());
    if (c != std::string::npos && json[c] == ':') {
      size_t q = json.find_first_not_of(" \t\r\n", c + 1);
      if (q != std::string::npos && json[q] == '"') {
        const size_t end = json.find('"', q + 1);
        if (end == std::string::npos) return {};
        if (at) *at = end;
        return json.substr(q + 1, end - q - 1);
      }
    }
    k = json.find(needle, k + 1);
  }
  return {};
}

// "v2.0.0-beta.3" -> {2, 0, 0}, with pre = 3 (-1 when it isn't a beta).
std::vector<int> VersionParts(std::string v, int* pre = nullptr) {
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
  if (pre) *pre = -1;
  if (const size_t dash = v.find('-'); dash != std::string::npos) {
    if (pre) {
      const size_t digit = v.find_first_of("0123456789", dash);
      *pre = digit == std::string::npos ? 0 : std::atoi(v.c_str() + digit);
    }
    v.erase(dash);
  }
  std::vector<int> parts;
  std::stringstream in(v);
  for (std::string p; std::getline(in, p, '.');) parts.push_back(std::atoi(p.c_str()));
  return parts;
}

// The latest release in one repo (its releases/latest API), for this platform.
bool LatestRelease(const char* api, UpdateInfo& info) {
  std::vector<uint8_t> data;
  if (!HttpGet(api, data)) return false;
  const std::string json(data.begin(), data.end());
  const std::string tag = JsonString(json, "tag_name");
  info.version = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
  info.page_url = JsonString(json, "html_url");  // the release's own page comes first
  for (size_t at = 0;;) {
    const std::string url = JsonString(json, "browser_download_url", at, &at);
    if (url.empty()) break;
#if defined(_WIN32)
    if (url.find("windows-x64.zip") != std::string::npos) info.zip_url = url;
#else
    if (url.find("linux-x86_64.AppImage") != std::string::npos) info.zip_url = url;
#endif
  }
  return !info.version.empty() && !info.zip_url.empty();
}

}  // namespace

bool IsNewerVersion(const std::string& a, const std::string& b) {
  int pre_a, pre_b;
  const auto pa = VersionParts(a, &pre_a), pb = VersionParts(b, &pre_b);
  for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
    const int x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
    if (x != y) return x > y;
  }
  if (pre_a == pre_b) return false;
  if (pre_a < 0 || pre_b < 0) return pre_a < 0;  // the release beats its betas
  return pre_a > pre_b;
}

std::optional<UpdateInfo> PickUpdate(const std::string& channel, bool switching, const std::string& current,
                                     const std::optional<UpdateInfo>& stable, const std::optional<UpdateInfo>& beta) {
  auto differs = [&](const UpdateInfo& u) {
    return IsNewerVersion(u.version, current) || IsNewerVersion(current, u.version);
  };
  if (channel != "beta") {
    if (!stable) return std::nullopt;
    if (switching ? differs(*stable) : IsNewerVersion(stable->version, current)) return stable;
    return std::nullopt;
  }
  if (switching) {  // Change version to beta: the newest beta
    if (beta && differs(*beta)) return beta;
    return std::nullopt;
  }
  std::optional<UpdateInfo> pick;
  if (beta && IsNewerVersion(beta->version, current)) pick = beta;
  if (stable && IsNewerVersion(stable->version, current)) {
    // Only the beta's own final release (or later): its numbers are at least the running build's.
    const auto s = VersionParts(stable->version), c = VersionParts(current);
    bool at_least = true;
    for (size_t i = 0; i < std::max(s.size(), c.size()); ++i) {
      const int x = i < s.size() ? s[i] : 0, y = i < c.size() ? c[i] : 0;
      if (x != y) {
        at_least = x > y;
        break;
      }
    }
    if (at_least && (!pick || IsNewerVersion(stable->version, pick->version))) pick = stable;
  }
  return pick;
}

#if defined(KK_DEV_TOOLS)
bool RunUpdatePickTests() {
  auto release = [](const char* v) { return std::optional<UpdateInfo>(UpdateInfo{v, "", ""}); };
  const std::optional<UpdateInfo> none;
  struct Case {
    const char* name;
    const char* channel;
    bool switching;
    const char* current;
    std::optional<UpdateInfo> stable, beta;
    const char* expect;  // "" for no update
  };
  const Case cases[] = {
      {"beta on 2.0.0-beta.1 sees beta.2", "beta", false, "2.0.0-beta.1", release("1.9.11"), release("2.0.0-beta.2"),
       "2.0.0-beta.2"},
      {"beta ignores stable 1.9.12", "beta", false, "2.0.0-beta.1", release("1.9.12"), release("2.0.0-beta.1"), ""},
      {"beta ignores stable 1.9.12 with no beta", "beta", false, "2.0.0-beta.1", release("1.9.12"), none, ""},
      {"beta takes stable 2.0.0", "beta", false, "2.0.0-beta.3", release("2.0.0"), release("2.0.0-beta.3"), "2.0.0"},
      {"beta takes stable 2.0.0 over beta.2", "beta", false, "2.0.0-beta.1", release("2.0.0"), release("2.0.0-beta.2"),
       "2.0.0"},
      {"beta takes stable 2.1.0", "beta", false, "2.0.0-beta.3", release("2.1.0"), none, "2.1.0"},
      {"beta repo empty: no update", "beta", false, "2.0.0-beta.1", release("1.9.11"), none, ""},
      {"beta repo unreachable, stable too: no update", "beta", false, "2.0.0-beta.1", none, none, ""},
      {"stable ignores betas", "stable", false, "1.9.11", release("1.9.11"), release("2.0.0-beta.2"), ""},
      {"stable sees 1.9.12", "stable", false, "1.9.11", release("1.9.12"), release("2.0.0-beta.2"), "1.9.12"},
      {"switch to stable from a beta", "stable", true, "2.0.0-beta.1", release("1.9.11"), release("2.0.0-beta.1"),
       "1.9.11"},
      {"switch to beta from stable", "beta", true, "1.9.11", release("1.9.11"), release("2.0.0-beta.1"),
       "2.0.0-beta.1"},
      {"switch to beta, no beta", "beta", true, "1.9.11", release("1.9.11"), none, ""},
  };
  bool all = true;
  for (const Case& c : cases) {
    const auto got = PickUpdate(c.channel, c.switching, c.current, c.stable, c.beta);
    const std::string v = got ? got->version : "";
    const bool ok = v == c.expect;
    all = all && ok;
    REXLOG_INFO("KK dev: update pick {}: {} ({})", c.name, ok ? "ok" : "FAILED", v.empty() ? "no update" : "v" + v);
  }
  REXLOG_INFO("KK dev: update pick tests {}", all ? "all ok" : "FAILED");
  return all;
}
#endif

void CheckForUpdate(UpdateStatus& status, const std::string& channel, bool switching) {
  status.busy = true;
  // Stable is read on both channels (a beta's final release is a stable one); beta only on beta.
  std::optional<UpdateInfo> stable, beta;
  if (UpdateInfo s; LatestRelease(kReleasesApi, s)) stable = s;
  if (channel == "beta")
    if (UpdateInfo b; LatestRelease(kBetaReleasesApi, b)) beta = b;
  // Neither answered: say so. (A beta repo without releases, with stable answering, is "no update".)
  if (!stable && !beta) {
    status.message = "Could not check for updates.";
    status.failed = true;
    status.busy = false;
    return;
  }
  std::string current = KK_VERSION;
#if defined(KK_DEV_TOOLS)
  // Developer aid: KK_DEV_UPDATE_FROM=1.9.1 acts as that version, to test
  // updating to the latest release.
  if (const char* v = std::getenv("KK_DEV_UPDATE_FROM"); v && *v) current = v;
#endif
  if (const auto info = PickUpdate(channel, switching, current, stable, beta)) {
    status.found = *info;
    REXLOG_INFO("KK: {} {}: v{} (this is v{})", channel, switching ? "version" : "update available", info->version,
                current);
  } else {
    REXLOG_INFO("KK: {}: no update (stable v{}, beta {}; this is v{})", channel, stable ? stable->version : "?",
                beta ? "v" + beta->version : std::string(channel == "beta" ? "none" : "not asked"), current);
  }
  status.done = true;
  status.busy = false;
}

void InstallUpdate(const UpdateInfo& info, UpdateStatus& status) {
  status.busy = true;
  status.bytes = 0;
  status.total = 0;
  auto fail = [&](std::string why) {
    REXLOG_WARN("KK: update: {}", why);
    status.message = std::move(why);
    status.failed = true;
    status.busy = false;
  };
#if defined(_WIN32)
  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  const fs::path work = fs::temp_directory_path(ec) / "king_kong_update";
  fs::remove_all(work, ec);
  fs::create_directories(work / "files", ec);

  std::vector<uint8_t> zip;
  if (!HttpGet(info.zip_url, zip, &status.bytes, &status.total))
    return fail("The download failed. Check your internet connection and try again.");
  const fs::path zip_path = work / "update.zip";
  std::ofstream(zip_path, std::ios::binary).write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()));

  // Windows 10 and 11 include tar, which unpacks zip files.
  wchar_t system_dir[MAX_PATH];
  GetSystemDirectoryW(system_dir, MAX_PATH);
  const std::wstring tar = std::wstring(system_dir) + L"\\tar.exe";
  if (!RunAndWait(L"\"" + tar + L"\" -xf \"" + zip_path.wstring() + L"\" -C \"" + (work / "files").wstring() + L"\""))
    return fail("Could not unpack the update.");

  // The release zip holds a KingKong folder; find the one with the program in it.
  fs::path root;
  for (auto& e : fs::recursive_directory_iterator(work / "files", ec))
    if (e.path().filename() == "king_kong.exe") root = e.path().parent_path();
  if (root.empty()) return fail("The update did not contain the game's program.");

  // 1. Stage every file as <name>.new beside the one it replaces. If anything
  //    fails here the installed version is untouched.
  std::vector<fs::path> staged;
  auto unstage = [&] {
    for (const auto& p : staged) fs::remove(p, ec);
  };
  for (auto& e : fs::recursive_directory_iterator(root, ec)) {
    if (!e.is_regular_file()) continue;
    const fs::path rel = fs::relative(e.path(), root, ec);
    const std::string top = rel.has_parent_path() ? rel.begin()->string() : "";
    bool allowed = false;
    for (const char* d : kUpdatedDirs) allowed |= top == d;
    if (!allowed) continue;  // never write into game/ or anywhere else
    const fs::path dst = exe_dir / rel;
    fs::create_directories(dst.parent_path(), ec);
    fs::path tmp = dst;
    tmp += ".new";
    if (!fs::copy_file(e.path(), tmp, fs::copy_options::overwrite_existing, ec)) {
      unstage();
      return fail("Could not write the update into the game folder. Is it read-only?");
    }
    staged.push_back(tmp);
  }
  // 2. Swap them in. Files in use (the running program, its DLLs) can be
  //    renamed but not overwritten, so the old ones become <name>.old.
  for (const auto& tmp : staged) {
    fs::path dst = tmp;
    dst.replace_extension();  // drop ".new"
    if (fs::exists(dst, ec)) {
      fs::path old = dst;
      old += ".old";
      fs::remove(old, ec);
      fs::rename(dst, old, ec);
    }
    fs::rename(tmp, dst, ec);
    if (ec) return fail("Could not finish installing the update: " + dst.filename().string());
  }
  fs::remove_all(work, ec);
  status.message = "Updated to v" + info.version + ". Restarting...";
  REXLOG_INFO("KK: {}", status.message);
  status.done = true;
  status.busy = false;
#else
  // Linux: the port is one AppImage file. Download the new one beside it and
  // rename it over the old (allowed while it runs); the restart starts it.
  const char* appimage = std::getenv("APPIMAGE");
  if (!appimage || !*appimage)
    return fail("Updates install themselves in the AppImage only. Download the new version from the release page.");
  const fs::path target = appimage;
  std::vector<uint8_t> data;
  if (!HttpGet(info.zip_url, data, &status.bytes, &status.total))
    return fail("The download failed. Check your internet connection and try again.");
  if (data.size() < (1u << 20) || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0)
    return fail("The download wasn't the game's AppImage.");
  fs::path tmp = target;
  tmp += ".new";
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size())))
      return fail("Could not save the update beside the AppImage. Is its folder read-only?");
  }
  fs::permissions(tmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                           fs::perms::others_exec, ec);
  fs::rename(tmp, target, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return fail("Could not replace the AppImage with the new version.");
  }
  status.message = "Updated to v" + info.version + ". Restarting...";
  REXLOG_INFO("KK: {}", status.message);
  status.done = true;
  status.busy = false;
#endif
}

void CleanUpAfterUpdate() {
  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  auto clean = [&](const fs::path& p) {
    if (p.extension() == ".old") fs::remove(p, ec);
  };
  for (auto& e : fs::directory_iterator(exe_dir, ec))  // the program's own folder, not game/
    if (e.is_regular_file()) clean(e.path());
  for (const char* d : {"glyphs", "launcher_art"})
    for (auto& e : fs::recursive_directory_iterator(exe_dir / d, ec))
      if (e.is_regular_file()) clean(e.path());
}

}  // namespace kk
