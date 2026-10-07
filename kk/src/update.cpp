#include "update.h"

#include <cstdio>
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

std::vector<int> VersionParts(std::string v) {
  if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
  std::vector<int> parts;
  std::stringstream in(v);
  for (std::string p; std::getline(in, p, '.');) parts.push_back(std::atoi(p.c_str()));
  return parts;
}

}  // namespace

bool IsNewerVersion(const std::string& a, const std::string& b) {
  const auto pa = VersionParts(a), pb = VersionParts(b);
  for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
    const int x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
    if (x != y) return x > y;
  }
  return false;
}

void CheckForUpdate(UpdateStatus& status) {
  status.busy = true;
  std::vector<uint8_t> data;
  if (!HttpGet(kReleasesApi, data)) {
    status.message = "Could not check for updates.";
    status.failed = true;
    status.busy = false;
    return;
  }
  const std::string json(data.begin(), data.end());
  const std::string tag = JsonString(json, "tag_name");
  UpdateInfo info;
  info.version = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
  info.page_url = JsonString(json, "html_url");  // the release's own page comes first
  for (size_t at = 0;;) {
    const std::string url = JsonString(json, "browser_download_url", at, &at);
    if (url.empty()) break;
    if (url.find("windows-x64.zip") != std::string::npos) info.zip_url = url;
  }
  if (!info.version.empty() && IsNewerVersion(info.version, KK_VERSION) && !info.zip_url.empty()) {
    status.found = info;
    REXLOG_INFO("KK: update available: v{} (this is v{})", info.version, KK_VERSION);
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
  (void)info;
  fail("Automatic updates are only available on Windows for now.");
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
