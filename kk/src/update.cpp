#include "update.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <mutex>
#include <thread>
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
#include "update_channel.h"
#include "update_files.h"

namespace kk {
namespace {

namespace fs = std::filesystem;
namespace uf = kk::update;

// The folders the old updater wrote to. Task 370: used only to install a release that has no kk-files.txt
// (stable 1.9.12 and older, when changing version back) and to clean up .old files.
const char* const kUpdatedDirs[] = {"", "glyphs", "launcher_art", "launcher", "mods-builtin"};

// Player facing messages (English; the launcher translates them, launcher_text_table.inc).
constexpr const char* kDownloadFailed = "The download failed. Check your internet connection and try again.";
constexpr const char* kDamaged = "The download did not pass its check, so nothing was changed. Try again later.";
constexpr const char* kUnpackFailed = "Could not unpack the update.";
constexpr const char* kNoProgram = "The update did not contain the game's program.";
constexpr const char* kListDamaged = "The update's file list is damaged, so nothing was changed.";
constexpr const char* kWriteFailed =
    "Could not write {0} into the game folder, so nothing was changed. If your antivirus blocked it, allow the King "
    "Kong folder and try again.";
constexpr const char* kInUse =
    "{0} is in use by another program, so the update was undone and nothing was changed. Close that program and "
    "try again.";

std::mutex g_protect_mutex;
std::vector<fs::path> g_protect;

#if defined(KK_DEV_TOOLS)
// Developer aid (restored from stable 1.9.12, task 370): KK_DEV_UPDATE_ZIP=<a release zip on this PC> is offered
// as the newest version (KK_DEV_UPDATE_VERSION, default 9.9.9) and installed from that file instead of a
// download; the first start check takes its missing files from it too.
const char* DevUpdateZip() {
  const char* v = std::getenv("KK_DEV_UPDATE_ZIP");
  return v && *v ? v : nullptr;
}
#endif

#if defined(_WIN32)
std::string Utf8(const fs::path& p) {
  const auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}

#endif

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

// The number of the last "key": <number> between `after` and `before` (an asset's "size"); 0 when none.
uint64_t JsonNumberBefore(const std::string& json, const std::string& key, size_t before, size_t after) {
  const size_t k = json.rfind("\"" + key + "\"", before);
  if (k == std::string::npos || k < after) return 0;
  size_t c = json.find(':', k);
  if (c == std::string::npos || c > before) return 0;
  c = json.find_first_not_of(" \t\r\n", c + 1);
  return c == std::string::npos ? 0 : std::strtoull(json.c_str() + c, nullptr, 10);
}

std::string UrlFileName(const std::string& url) {
  const size_t slash = url.find_last_of('/');
  return slash == std::string::npos ? url : url.substr(slash + 1);
}

bool EndsWith(const std::string& s, const char* tail) {
  const size_t n = std::strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
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

// One release's API answer: its version, page, this platform's download (with its size) and SHA256SUMS.txt.
bool ParseRelease(const std::string& json, UpdateInfo& info) {
  const std::string tag = JsonString(json, "tag_name");
  info.version = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
  info.page_url = JsonString(json, "html_url");  // the release's own page comes first
  for (size_t at = 0, prev = 0;;) {
    const std::string url = JsonString(json, "browser_download_url", at, &at);
    if (url.empty()) break;
#if defined(_WIN32)
    const std::string name = UrlFileName(url);
    // Only a name ending in windows-x64.zip (old updaters take any name containing it: new assets avoid it).
    if (EndsWith(name, "windows-x64.zip")) {
      info.zip_url = url;
      info.zip_size = JsonNumberBefore(json, "size", at, prev);
    }
    if (name == "SHA256SUMS.txt") info.sums_url = url;
#else
    if (url.find("linux-x86_64.AppImage") != std::string::npos) {
      info.zip_url = url;
      info.zip_size = JsonNumberBefore(json, "size", at, prev);
    }
    if (UrlFileName(url) == "SHA256SUMS.txt") info.sums_url = url;  // owner, 25 Oct: checked as on Windows
#endif
    prev = at;
  }
  return !info.version.empty() && !info.zip_url.empty();
}

// The latest release in one repo (its releases/latest API), for this platform. False only when the repo did not
// answer. A release without this platform's download (no AppImage: a Windows only release) is no release here, so
// Linux players get "no update" instead of "Could not check for updates" (and Windows likewise for a Linux only one).
bool LatestRelease(const char* api, std::optional<UpdateInfo>& out) {
  std::vector<uint8_t> data;
  if (!HttpGet(api, data)) return false;
  UpdateInfo info;
  if (ParseRelease(std::string(data.begin(), data.end()), info)) out = info;
  else if (!info.version.empty()) REXLOG_INFO("KK: update: v{} has no download for this platform", info.version);
  else return false;
  return true;
}

#if defined(_WIN32)
// The protected paths (update_files.h) plus the launcher's folders that are inside the program folder.
uf::Protected MakeProtected(const fs::path& exe_dir) {
  std::vector<std::string> extra;
  std::lock_guard lock(g_protect_mutex);
  for (const auto& p : g_protect) {
    std::error_code ec;
    const fs::path rel = fs::relative(fs::weakly_canonical(p, ec), fs::weakly_canonical(exe_dir, ec), ec);
    const std::string r = Utf8(rel);
    if (!ec && !rel.empty() && r != "." && r.rfind("..", 0) != 0) extra.push_back(r);
  }
  return uf::Protected(std::move(extra));
}

uf::Log LogTo(const char* what) {
  return [what](const std::string& line) { REXLOG_INFO("KK: {}: {}", what, line); };
}

bool Untar(const fs::path& zip, const fs::path& into) {
  wchar_t system_dir[MAX_PATH];
  GetSystemDirectoryW(system_dir, MAX_PATH);
  const std::wstring tar = std::wstring(system_dir) + L"\\tar.exe";  // Windows 10 and 11 include it
  return RunAndWait(L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + into.wstring() + L"\"");
}

// Downloads (or reuses a checked copy from an earlier try), checks and unpacks a release into `work`. `root` is
// the unpacked folder holding king_kong.exe. On failure `why` is a player facing message; a checked zip stays in
// `work` so a retry does not download it again.
bool FetchRelease(const UpdateInfo& info, const fs::path& work, std::atomic<uint64_t>& bytes,
                  std::atomic<uint64_t>& total, std::atomic<int>& phase, std::string& why, fs::path& root) {
  std::error_code ec;
  fs::create_directories(work, ec);
  fs::remove_all(work / "files", ec);
  fs::create_directories(work / "files", ec);
  const fs::path zip_path = work / "update.zip";
  phase = int(UpdatePhase::kDownloading);

  // The expected hash: the release's SHA256SUMS.txt line for the zip.
  std::string want;
  bool dev = false;
#if defined(KK_DEV_TOOLS)
  dev = DevUpdateZip() != nullptr;
  if (dev) {
    const fs::path sums = fs::path(info.zip_url).parent_path() / "SHA256SUMS.txt";
    if (std::ifstream in{sums, std::ios::binary}) {
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      want = uf::SumFor(text, Utf8(fs::path(info.zip_url).filename()));
      if (want.empty()) {
        REXLOG_WARN("KK: update: {} has no line for the zip", Utf8(sums));
        why = kDamaged;
        return false;
      }
    }
  }
#endif
  if (!dev && !info.sums_url.empty()) {
    std::vector<uint8_t> sums;
    if (!HttpGet(info.sums_url, sums)) return why = kDownloadFailed, false;
    want = uf::SumFor(std::string(sums.begin(), sums.end()), UrlFileName(info.zip_url));
    if (want.empty()) {
      REXLOG_WARN("KK: update: SHA256SUMS.txt has no line for {}", UrlFileName(info.zip_url));
      return why = kDamaged, false;
    }
  }
  if (want.empty()) REXLOG_WARN("KK: update: the release has no SHA256SUMS.txt: only its size is checked");

  bool reused = false;
  if (!want.empty() && fs::is_regular_file(zip_path, ec) &&
      (info.zip_size == 0 || fs::file_size(zip_path, ec) == info.zip_size)) {
    phase = int(UpdatePhase::kChecking);
    reused = uf::FileSha256(zip_path) == want;
    if (reused) REXLOG_INFO("KK: update: using the checked download from an earlier try");
  }
  if (!reused) {
    fs::remove(zip_path, ec);
    phase = int(UpdatePhase::kDownloading);
    std::string got;
    if (dev) {
#if defined(KK_DEV_TOOLS)
      // A local copy, in pieces so the screen shows progress.
      std::ifstream in(fs::path(info.zip_url), std::ios::binary);
      std::ofstream out(zip_path, std::ios::binary | std::ios::trunc);
      if (!in || !out) {
        REXLOG_WARN("KK_DEV_UPDATE_ZIP: could not read {}", info.zip_url);
        why = kDownloadFailed;
        return false;
      }
      total = fs::file_size(fs::path(info.zip_url), ec);
      std::vector<char> buf(4 << 20);
      while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        if (in.gcount() > 0) out.write(buf.data(), in.gcount()), bytes += uint64_t(in.gcount());
        if (std::getenv("KK_DEV_UPDATE_SLOW")) std::this_thread::sleep_for(std::chrono::milliseconds(60));  // to see it
      }
      if (!out) return why = kUnpackFailed, false;
      out.close();
      phase = int(UpdatePhase::kChecking);
      if (!want.empty()) got = uf::FileSha256(zip_path);
#endif
    } else {
      std::vector<uint8_t> zip;
      if (!HttpGet(info.zip_url, zip, &bytes, &total)) return why = kDownloadFailed, false;
      phase = int(UpdatePhase::kChecking);
      if (info.zip_size && zip.size() != info.zip_size) {
        REXLOG_WARN("KK: update: the download is {} bytes, the release lists {}", zip.size(), info.zip_size);
        return why = kDamaged, false;
      }
      if (!want.empty()) got = uf::Sha256Of(zip.data(), zip.size());
      std::ofstream out(zip_path, std::ios::binary | std::ios::trunc);
      if (!out.write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()))) {
        fs::remove(zip_path, ec);
        REXLOG_WARN("KK: update: could not save {}", Utf8(zip_path));
        why = kUnpackFailed;
        return false;
      }
    }
    if (!want.empty() && got != want) {
      REXLOG_WARN("KK: update: sha256 {} does not match SHA256SUMS.txt {}", got, want);
      fs::remove(zip_path, ec);
      return why = kDamaged, false;
    }
  }
  phase = int(UpdatePhase::kChecking);
  if (!Untar(zip_path, work / "files")) return why = kUnpackFailed, false;
  // The release zip holds a KingKong folder; the program's folder is the one nearest the top.
  root.clear();
  size_t depth = SIZE_MAX;
  for (auto& e : fs::recursive_directory_iterator(work / "files", ec)) {
    if (e.path().filename() != "king_kong.exe") continue;
    const size_t d = size_t(std::distance(e.path().begin(), e.path().end()));
    if (d < depth) depth = d, root = e.path().parent_path();
  }
  if (root.empty()) return why = kNoProgram, false;
  return true;
}

std::string InstallMessage(const uf::InstallResult& r, std::string& detail) {
  detail.clear();
  switch (r.outcome) {
    case uf::Outcome::kOk: return {};
    case uf::Outcome::kNoSource: return kDamaged;
    case uf::Outcome::kWrite:
      detail = r.error.empty() ? r.path : r.path + " (" + r.error + ")";
      return kWriteFailed;
    case uf::Outcome::kInUse:
      detail = r.path;
      return kInUse;
  }
  return kWriteFailed;
}
#endif

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

void SetProtectedFolders(std::vector<std::filesystem::path> folders) {
  std::lock_guard lock(g_protect_mutex);
  g_protect = std::move(folders);
}

void CheckForUpdate(UpdateStatus& status, const std::string& wanted, bool switching) {
  status.busy = true;
#if defined(KK_DEV_TOOLS)
  if (const char* zip = DevUpdateZip()) {
    if (std::getenv("KK_DEV_UPDATE_RELAUNCHED")) {  // the program it just installed: no update (and no GitHub)
      REXLOG_INFO("KK dev: KK_DEV_UPDATE_ZIP installed; not offered again");
      status.done = true;
      status.busy = false;
      return;
    }
    const char* v = std::getenv("KK_DEV_UPDATE_VERSION");
    status.found = UpdateInfo{v && *v ? v : "9.9.9", "", zip};
    REXLOG_INFO("KK dev: KK_DEV_UPDATE_ZIP offers v{} from {}", status.found->version, zip);
    status.done = true;
    status.busy = false;
    return;
  }
#endif
  // A stable build never asks the beta repo, whatever channel it is asked for (update_channel.h).
  const std::string channel = CheckChannel(kBuildChannel, wanted);
  // Stable is read on both channels (a beta's final release is a stable one); beta only on beta.
  std::optional<UpdateInfo> stable, beta;
  const bool stable_answered = LatestRelease(kReleasesApi, stable);
  const bool beta_answered = channel == "beta" && LatestRelease(kBetaReleasesApi, beta);
  // Neither answered: say so. (A beta repo without releases, with stable answering, is "no update".)
  if (!stable_answered && !beta_answered) {
    REXLOG_WARN("KK: update check: GitHub did not answer");
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
  status.detail.clear();
  auto fail = [&](std::string why) {
    REXLOG_WARN("KK: update: {}{}", why, status.detail.empty() ? "" : " (" + status.detail + ")");
    status.message = std::move(why);
    status.failed = true;
    status.busy = false;
  };
#if defined(_WIN32)
  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  const fs::path work = fs::temp_directory_path(ec) / "king_kong_update";
  fs::path root;
  std::string why;
  if (!FetchRelease(info, work, status.bytes, status.total, status.phase, why, root)) {
    fs::remove_all(work / "files", ec);
    return fail(why);
  }
  status.phase = int(UpdatePhase::kInstalling);
  const uf::Protected keep = MakeProtected(exe_dir);
  const auto log = LogTo("update");
  uf::InstallResult result;
  if (fs::exists(root / uf::kFilesList, ec)) {
    // Every file the release lists (task 370), except protected paths and the extra zip for old updaters.
    std::string error;
    const auto list = uf::ReadManifest(root / uf::kFilesList, &error);
    if (!list) {
      REXLOG_WARN("KK: update: kk-files.txt: {}", error);
      fs::remove_all(work, ec);
      return fail(kListDamaged);
    }
    std::vector<std::string> plan = uf::MissingOrWrong(exe_dir, *list, keep, UINT64_MAX);
    plan.push_back(uf::kFilesList);
    REXLOG_INFO("KK: update: v{} lists {} files; {} to install", list->version, list->files.size(), plan.size());
    result = uf::InstallFiles(root, exe_dir, plan, log);
    if (result.outcome == uf::Outcome::kOk) {
      // Its content is installed directly now: an old copy must never be unpacked over the new files.
      fs::remove(exe_dir / uf::kExtraFilesZip, ec);
      if (const auto retired = uf::ReadManifest(root / uf::kRetiredList, &error)) {
        const auto removed = uf::RetireFiles(exe_dir, *retired, *list, keep, log);
        REXLOG_INFO("KK: update: {} retired files removed", removed.size());
      } else if (fs::exists(root / uf::kRetiredList, ec)) {
        REXLOG_WARN("KK: update: kk-retired.txt: {} (nothing removed)", error);
      }
    }
  } else {
    // A release from before the file list: the old folder list, nothing removed (now with undo).
    std::vector<std::string> plan;
    for (auto& e : fs::recursive_directory_iterator(root, ec)) {
      if (!e.is_regular_file()) continue;
      const std::string rel = Utf8(fs::relative(e.path(), root, ec));
      const std::string top = rel.find('/') == std::string::npos ? "" : rel.substr(0, rel.find('/'));
      bool allowed = false;
      for (const char* d : kUpdatedDirs) allowed |= top == d;
      if (allowed && uf::SafePath(rel) && !keep(rel)) plan.push_back(rel);  // never game/ or anywhere else
    }
    REXLOG_INFO("KK: update: v{} has no kk-files.txt: {} files by the old folder list", info.version, plan.size());
    result = uf::InstallFiles(root, exe_dir, plan, log);
  }
  if (result.outcome != uf::Outcome::kOk) {
    fs::remove_all(work / "files", ec);  // the checked zip stays for a retry
    const std::string message = InstallMessage(result, status.detail);
    return fail(message);
  }
  fs::remove_all(work, ec);
  status.message = "Updated to v" + info.version + ". Restarting...";
  REXLOG_INFO("KK: {}", status.message);
  status.done = true;
  status.busy = false;
#else
  // Linux (unchanged by task 370): the port is one AppImage file. Download the new one beside it and
  // rename it over the old (allowed while it runs); the restart starts it.
  const char* appimage = std::getenv("APPIMAGE");
  if (!appimage || !*appimage)
    return fail("Updates install themselves in the AppImage only. Download the new version from the release page.");
  const fs::path target = appimage;
  // The expected hash: the release's SHA256SUMS.txt line for the AppImage (owner, 25 Oct: the same check as
  // the Windows zip's). A release without SHA256SUMS.txt is checked by its size only, as on Windows.
  std::string want;
  std::vector<uint8_t> data;
  bool dev = false;
#if defined(KK_DEV_TOOLS)
  // KK_DEV_UPDATE_ZIP=<an AppImage on this PC>, with the SHA256SUMS.txt beside it when there is one.
  if (DevUpdateZip()) {
    dev = true;
    const fs::path file(info.zip_url), sums = file.parent_path() / "SHA256SUMS.txt";
    if (std::ifstream in{sums, std::ios::binary}) {
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      want = uf::SumFor(text, file.filename().string());
      if (want.empty()) return fail(kDamaged);
    }
    std::ifstream in(file, std::ios::binary);
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    status.total = status.bytes = data.size();
  }
#endif
  if (!dev) {
    if (!info.sums_url.empty()) {
      std::vector<uint8_t> sums;
      if (!HttpGet(info.sums_url, sums)) return fail(kDownloadFailed);
      want = uf::SumFor(std::string(sums.begin(), sums.end()), UrlFileName(info.zip_url));
      if (want.empty()) {
        REXLOG_WARN("KK: update: SHA256SUMS.txt has no line for {}", UrlFileName(info.zip_url));
        return fail(kDamaged);
      }
    } else {
      REXLOG_WARN("KK: update: the release has no SHA256SUMS.txt: only its size is checked");
    }
    if (!HttpGet(info.zip_url, data, &status.bytes, &status.total)) return fail(kDownloadFailed);
  }
  if (data.size() < (1u << 20) || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0)
    return fail("The download wasn't the game's AppImage.");
  if (info.zip_size && data.size() != info.zip_size) {
    REXLOG_WARN("KK: update: the download is {} bytes, the release lists {}", data.size(), info.zip_size);
    return fail(kDamaged);
  }
  if (!want.empty()) {
    if (const std::string got = uf::Sha256Of(data.data(), data.size()); got != want) {
      REXLOG_WARN("KK: update: sha256 {} does not match SHA256SUMS.txt {}", got, want);
      return fail(kDamaged);
    }
  }
  fs::path tmp = target;
  tmp += ".new";
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size())))
      return fail("Could not save the update beside the AppImage. Is its folder read only?");
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
  for (const char* d : kUpdatedDirs)
    if (*d)
      for (auto& e : fs::recursive_directory_iterator(exe_dir / d, ec))
        if (e.is_regular_file()) clean(e.path());
  // Task 370: beside every file the lists name (folders the old list did not have too).
  for (const char* name : {uf::kFilesList, uf::kRetiredList})
    if (const auto list = uf::ReadManifest(exe_dir / name)) uf::RemoveOldCopies(exe_dir, *list);
#if defined(_WIN32)
  // Unpack the files an older updater left out (kExtraFilesZip). Kept for the next start if it fails.
  const fs::path extra = exe_dir / uf::kExtraFilesZip;
  if (fs::exists(extra, ec)) {
    if (Untar(extra, exe_dir)) {
      fs::remove(extra, ec);
      REXLOG_INFO("KK: update: unpacked {}", uf::kExtraFilesZip);
    } else {
      REXLOG_WARN("KK: update: could not unpack {}; trying again next start", uf::kExtraFilesZip);
    }
  }
#endif
}

void FinishUpdate(FinishStatus& s, const std::filesystem::path& state_file) {
  s.busy = true;
  auto finish = [&](bool ok) {
    (ok ? s.done : s.failed) = true;
    s.downloading = false;
    s.busy = false;
  };
#if defined(_WIN32)
  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  std::string text;
  if (std::ifstream in{exe_dir / uf::kFilesList, std::ios::binary})
    text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (text.empty()) {
    REXLOG_INFO("KK: files check: no kk-files.txt beside the program (not a packaged release): skipped");
    return finish(true);
  }
  // Remembered: this list, in this folder, passed before.
  const std::string passed = uf::Sha256Of(text.data(), text.size()) + "\n" + Utf8(exe_dir) + "\n";
  if (std::ifstream in{state_file, std::ios::binary}) {
    const std::string was((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (was == passed) return finish(true);
  }
  auto remember = [&] {
    fs::create_directories(state_file.parent_path(), ec);
    std::ofstream(state_file, std::ios::binary | std::ios::trunc) << passed;
  };
  std::string error;
  const auto list = uf::ParseManifest(text, &error);
  if (!list) {
    REXLOG_WARN("KK: files check: kk-files.txt: {}", error);
    return finish(false);
  }
  if (list->version != KK_VERSION) {
    REXLOG_WARN("KK: files check: kk-files.txt is for v{}, this program is v{}: skipped", list->version, KK_VERSION);
    return finish(false);
  }
  const uf::Protected keep = MakeProtected(exe_dir);
  const auto log = LogTo("files check");
  if (const auto retired = uf::ReadManifest(exe_dir / uf::kRetiredList, &error)) {
    const auto removed = uf::RetireFiles(exe_dir, *retired, *list, keep, log);
    if (!removed.empty()) REXLOG_INFO("KK: files check: {} retired files removed", removed.size());
  }
  const auto needed = uf::MissingOrWrong(exe_dir, *list, keep, uf::kCheapHashLimit);
  if (needed.empty()) {
    REXLOG_INFO("KK: files check: all {} files of v{} are in place", list->files.size(), KK_VERSION);
    remember();
    return finish(true);
  }
  std::string names;
  for (size_t i = 0; i < needed.size() && i < 8; ++i) names += (i ? ", " : "") + needed[i];
  REXLOG_INFO("KK: files check: {} files missing or wrong ({}{}): downloading v{} again", needed.size(), names,
              needed.size() > 8 ? ", ..." : "", KK_VERSION);
  s.needed = int(needed.size());
  {  // A folder it cannot write to would mean downloading the release at every start for nothing.
    const fs::path probe = exe_dir / "kk-write-test.tmp";
    const bool writable = static_cast<bool>(std::ofstream(probe, std::ios::binary | std::ios::trunc));
    fs::remove(probe, ec);
    if (!writable) {
      REXLOG_WARN("KK: files check: the program folder is read only: nothing downloaded");
      return finish(false);
    }
  }

  UpdateInfo info;
#if defined(KK_DEV_TOOLS)
  if (const char* zip = DevUpdateZip()) info = UpdateInfo{KK_VERSION, "", zip};
#endif
  if (info.zip_url.empty()) {
    // This version's own release: the beta repo for a beta version.
    std::string api = std::string(std::string(KK_VERSION).find("-beta") != std::string::npos ? kBetaReleasesApi
                                                                                                : kReleasesApi);
    api = api.substr(0, api.rfind('/') + 1) + "tags/v" KK_VERSION;
    std::vector<uint8_t> data;
    if (!HttpGet(api, data) || !ParseRelease(std::string(data.begin(), data.end()), info) ||
        info.version != KK_VERSION) {
      REXLOG_WARN("KK: files check: could not find the v{} release; trying again next start", KK_VERSION);
      return finish(false);
    }
  }
  s.downloading = true;
  const fs::path work = fs::temp_directory_path(ec) / "king_kong_finish";
  fs::path root;
  std::string why;
  if (!FetchRelease(info, work, s.bytes, s.total, s.phase, why, root)) {
    fs::remove_all(work / "files", ec);
    REXLOG_WARN("KK: files check: {} Trying again next start.", why);
    return finish(false);
  }
  s.phase = int(UpdatePhase::kInstalling);
  const auto r = uf::InstallFromSource(root, exe_dir, *list, needed, log);
  if (r.install.outcome != uf::Outcome::kOk) {
    fs::remove_all(work / "files", ec);
    REXLOG_WARN("KK: files check: could not install the files ({}); trying again next start", r.install.path);
    return finish(false);
  }
  fs::remove_all(work, ec);
  s.installed = int(r.installed.size());
  REXLOG_INFO("KK: files check: {} files installed{}", r.installed.size(),
              r.unavailable.empty() ? "" : ", " + std::to_string(r.unavailable.size()) + " not in the release");
  // Files the release itself lacks would only be downloaded again at every start: remembered as checked.
  remember();
  finish(true);
#else
  (void)state_file;
  finish(true);  // Linux: the AppImage is always complete
#endif
}

}  // namespace kk
