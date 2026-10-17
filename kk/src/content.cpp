#include "content.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef KK_VERSION
#define KK_VERSION "dev"
#endif
#ifndef KK_VERSION_W
#define KK_VERSION_W L"dev"
#endif

namespace kk::content {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

std::function<void(bool, const std::string&)>& LogSink() {
  static std::function<void(bool, const std::string&)> sink;
  return sink;
}
void Log(const std::string& line) {
  if (LogSink()) LogSink()(false, "KK content: " + line);
}
void Warn(const std::string& line) {
  if (LogSink()) LogSink()(true, "KK content: " + line);
}

std::string U8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
fs::path FromU8(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

uint64_t FileSize(const fs::path& p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? 0 : uint64_t(n);
}
bool Exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}
uint64_t DirBytes(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2)) total += uint64_t(it->file_size(e2));
  }
  return total;
}
bool RemoveAll(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);
  return !ec && !Exists(p);
}

std::map<std::string, std::string> ReadKeys(const fs::path& file) {
  std::map<std::string, std::string> keys;
  std::ifstream in(file, std::ios::binary);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq != std::string::npos) keys[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return keys;
}
bool WriteKeys(const fs::path& file, const std::map<std::string, std::string>& keys) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    for (const auto& [k, v] : keys) out << k << '=' << v << '\n';
    if (!out) return false;
  }
  fs::rename(tmp, file, ec);
  return !ec;
}

uint64_t ToU64(const std::string& s) {
  try {
    return s.empty() ? 0 : std::stoull(s);
  } catch (...) {
    return 0;
  }
}

// ------------------------------------------------------- child processes ---
// Runs a program (no window) and waits, calling `poll` about every 200 ms; when it returns false the
// program is stopped. Returns its exit code, -1 when it could not be started, -2 when stopped.
#if defined(_WIN32)
int RunTool(const std::wstring& command_line, const fs::path& cwd, const std::function<bool()>& poll) {
  std::wstring cmd = command_line;
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
    return -1;
  int result = -2;
  for (;;) {
    if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) {
      DWORD code = 1;
      GetExitCodeProcess(pi.hProcess, &code);
      result = int(code);
      break;
    }
    if (poll && !poll()) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 5000);
      break;
    }
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return result;
}
#else
// The AppImage's LD_LIBRARY_PATH is not passed on: system programs load their own libraries.
int RunTool(const std::vector<std::string>& argv, const std::function<bool()>& poll, std::string* out = nullptr) {
  int fds[2] = {-1, -1};
  if (out && pipe(fds) != 0) return -1;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (out) {
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
  } else {
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  }
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  std::vector<char*> args;
  for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  std::vector<char*> env;
  for (char** e = environ; *e; ++e)
    if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) != 0) env.push_back(*e);
  env.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), env.data());
  posix_spawn_file_actions_destroy(&actions);
  if (out) close(fds[1]);
  if (rc != 0) {
    if (out) close(fds[0]);
    return -1;
  }
  bool open_pipe = out != nullptr, stopped = false;
  int status = 0;
  for (;;) {
    if (open_pipe) {
      pollfd p{fds[0], POLLIN, 0};
      if (::poll(&p, 1, 200) > 0) {
        char buf[4096];
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0) out->append(buf, size_t(n));
        else if (n == 0 || errno != EINTR) open_pipe = false;
      }
    } else {
      usleep(200 * 1000);
    }
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid && !open_pipe) break;
    if (done == pid) {  // read what is left
      for (char buf[4096];;) {
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        out->append(buf, size_t(n));
      }
      open_pipe = false;
      break;
    }
    if (!stopped && poll && !poll()) {
      kill(pid, SIGTERM);
      stopped = true;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      break;
    }
  }
  if (out) close(fds[0]);
  if (stopped) return -2;
  if (WIFEXITED(status)) {
    const int code = WEXITSTATUS(status);
    return code == 127 ? -1 : code;  // 127: the shell's "not found" (posix_spawnp may report it this way)
  }
  return -3;
}
#endif

// ----------------------------------------------------------------- fetch ---
#if defined(_WIN32)
class WinHttpFetcher : public Fetcher {
 public:
  bool Probe(const std::string& url, uint64_t& total, std::string& detail) override {
    Request r;
    DWORD status = 0;
    if (!Open(url, "bytes=0-0", r, status, detail)) return false;
    const ContentRange range = ParseContentRange(Header(r.req, WINHTTP_QUERY_CONTENT_RANGE));
    total = SizeFromProbe(int(status), range, ToU64(Header(r.req, WINHTTP_QUERY_CONTENT_LENGTH)));
    if (!total) detail = "no size in the reply (HTTP " + std::to_string(status) + ")";
    return total != 0;
  }

  Result Fetch(const std::string& url, const fs::path& part, uint64_t total, const std::atomic<bool>& stop,
               std::atomic<uint64_t>& have) override {
    uint64_t kept = FileSize(part);
    have = kept;
    Request r;
    DWORD status = 0;
    std::string detail;
    if (!Open(url, RangeValue(kept), r, status, detail)) {
      Warn("download request failed: " + detail);
      return Result::kConnection;
    }
    const ContentRange range = ParseContentRange(Header(r.req, WINHTTP_QUERY_CONTENT_RANGE));
    switch (DecideRangeReply(kept, int(status), range)) {
      case RangeReply::kComplete: return Result::kComplete;
      case RangeReply::kRestart: Log("the server sent the whole file: starting again"); return Result::kRestart;
      case RangeReply::kFail:
        Warn("download reply HTTP " + std::to_string(status));
        return status >= 500 || status == 408 || status == 429 ? Result::kConnection : Result::kServer;
      case RangeReply::kAppend: break;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, part.c_str(), kept ? L"ab" : L"wb") != 0 || !f) return Result::kWrite;
    std::vector<uint8_t> buf(1 << 20);
    Result result = Result::kConnection;
    auto logged = Clock::now();
    for (;;) {
      if (stop) {
        result = Result::kStopped;
        break;
      }
      DWORD read = 0;
      if (!WinHttpReadData(r.req, buf.data(), DWORD(buf.size()), &read)) {
        Warn("connection lost after " + std::to_string(kept) + " bytes (error " + std::to_string(GetLastError()) + ")");
        break;
      }
      if (read == 0) {
        result = kept == total ? Result::kComplete : Result::kConnection;
        break;
      }
      if (fwrite(buf.data(), 1, read, f) != read) {
        result = Result::kWrite;
        break;
      }
      kept += read;
      have = kept;
      if (Clock::now() - logged >= std::chrono::seconds(15)) {
        logged = Clock::now();
        Log("downloaded " + std::to_string(kept) + " of " + std::to_string(total) + " bytes");
      }
      if (const uint64_t drop = drop_at.load(); drop && kept >= drop) {
        drop_at = 0;
        Log("testing: connection dropped at " + std::to_string(kept) + " bytes");
        break;
      }
    }
    if (fclose(f) != 0 && result != Result::kStopped) result = Result::kWrite;
    have = FileSize(part);
    return result;
  }

 private:
  struct Request {
    HINTERNET session = nullptr, conn = nullptr, req = nullptr;
    ~Request() {
      if (req) WinHttpCloseHandle(req);
      if (conn) WinHttpCloseHandle(conn);
      if (session) WinHttpCloseHandle(session);
    }
  };
  static std::string Header(HINTERNET req, DWORD query) {
    wchar_t buf[256];
    DWORD len = sizeof(buf);
    if (!WinHttpQueryHeaders(req, query, WINHTTP_HEADER_NAME_BY_INDEX, buf, &len, WINHTTP_NO_HEADER_INDEX))
      return {};
    std::string out;
    for (DWORD i = 0; i < len / sizeof(wchar_t); ++i) out.push_back(char(buf[i]));
    return out;
  }
  static bool Open(const std::string& url, const std::string& range, Request& r, DWORD& status, std::string& detail) {
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256], path[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    const std::wstring wurl(url.begin(), url.end());
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return detail = "bad address", false;
    r.session = WinHttpOpen(L"KingKongRecomp/" KK_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!r.session) return detail = "WinHttpOpen failed", false;
    // Connect, send, wait for headers, each read: a stalled connection fails after 30 s and is retried.
    WinHttpSetTimeouts(r.session, 15000, 15000, 30000, 30000);
    r.conn = WinHttpConnect(r.session, host, parts.nPort, 0);
    if (!r.conn) return detail = "cannot connect (" + std::to_string(GetLastError()) + ")", false;
    const DWORD secure = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    r.req = WinHttpOpenRequest(r.conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure);
    if (!r.req) return detail = "cannot open the request", false;
    const std::wstring header = L"Range: " + std::wstring(range.begin(), range.end());
    DWORD len = sizeof(status);
    if (!WinHttpSendRequest(r.req, header.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r.req, nullptr) ||
        !WinHttpQueryHeaders(r.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len,
                             nullptr))
      return detail = "no reply (" + std::to_string(GetLastError()) + ")", false;
    return true;
  }
};
#else
class CurlFetcher : public Fetcher {
 public:
  bool Probe(const std::string& url, uint64_t& total, std::string& detail) override {
    std::string headers;
    const int code = RunTool({"curl", "-sS", "-L", "--max-time", "30", "-A", Agent(), "-r", "0-0", "-D", "-", "-o",
                              "/dev/null", url},
                             nullptr, &headers);
    if (code == -1) return detail = "curl is not installed", false;
    if (code != 0) return detail = "curl exit " + std::to_string(code), false;
    int status = 0;
    ContentRange range;
    uint64_t length = 0;
    std::stringstream lines(headers);
    for (std::string line; std::getline(lines, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string lower = line;
      for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
      if (lower.rfind("http/", 0) == 0) {  // a new reply (redirects come first)
        const size_t sp = line.find(' ');
        status = sp == std::string::npos ? 0 : std::atoi(line.c_str() + sp + 1);
        range = {};
        length = 0;
      } else if (lower.rfind("content-range:", 0) == 0) {
        range = ParseContentRange(line.substr(14));
      } else if (lower.rfind("content-length:", 0) == 0) {
        length = ToU64(std::string(line.c_str() + 15 + (line.size() > 15 && line[15] == ' ' ? 1 : 0)));
      }
    }
    total = SizeFromProbe(status, range, length);
    if (!total) detail = "no size in the reply (HTTP " + std::to_string(status) + ")";
    return total != 0;
  }

  Result Fetch(const std::string& url, const fs::path& part, uint64_t total, const std::atomic<bool>& stop,
               std::atomic<uint64_t>& have) override {
    const uint64_t kept = FileSize(part);
    have = kept;
    // -C appends to the kept file from its size; a server that ignores the range makes curl fail with
    // 33 instead of writing the whole file after it.
    std::vector<std::string> argv = {"curl", "-sS", "-f", "-L", "--connect-timeout", "20", "--speed-limit", "1024",
                                     "--speed-time", "60", "-A", Agent(), "-o", part.string()};
    if (kept) {
      argv.push_back("-C");
      argv.push_back(std::to_string(kept));
    }
    argv.push_back(url);
    bool dropped = false;
    const int code = RunTool(argv, [&] {
      have = FileSize(part);
      if (const uint64_t drop = drop_at.load(); drop && have >= drop) {
        drop_at = 0;
        dropped = true;
        Log("testing: connection dropped at " + std::to_string(have.load()) + " bytes");
        return false;
      }
      return !stop.load();
    });
    have = FileSize(part);
    if (dropped) return Result::kConnection;
    if (code == -2) return Result::kStopped;
    if (code == -1) {
      Warn("curl is not installed");
      return Result::kServer;
    }
    if (code == 0) return have == total ? Result::kComplete : Result::kConnection;
    Warn("curl exit " + std::to_string(code) + " at " + std::to_string(have.load()) + " bytes");
    if (code == 33) return Result::kRestart;
    if (code == 22) return Result::kServer;
    if (code == 23) return Result::kWrite;
    return Result::kConnection;
  }

 private:
  static std::string Agent() { return "KingKongRecomp/" KK_VERSION; }
};
#endif

}  // namespace

std::unique_ptr<Fetcher> MakeSystemFetcher() {
#if defined(_WIN32)
  return std::make_unique<WinHttpFetcher>();
#else
  return std::make_unique<CurlFetcher>();
#endif
}

// ---------------------------------------------------------------- unpack ---
bool UnpackZip(const fs::path& zip, const fs::path& into, const std::function<bool()>& progress, std::string& detail) {
#if defined(_WIN32)
  // tar.exe (libarchive) reads its arguments in the ANSI code page: run it in the folder's parent with a
  // relative target, and give it the ZIP's short name when its path has other characters.
  wchar_t system_dir[MAX_PATH];
  GetSystemDirectoryW(system_dir, MAX_PATH);
  const std::wstring tar = std::wstring(system_dir) + L"\\tar.exe";
  if (!Exists(tar)) return detail = "tar.exe was not found", false;
  std::wstring source = zip.wstring();
  auto ascii = [](const std::wstring& s) {
    for (wchar_t c : s)
      if (c > 0x7e) return false;
    return true;
  };
  if (!ascii(source)) {
    wchar_t short_path[MAX_PATH * 2];
    const DWORD n = GetShortPathNameW(source.c_str(), short_path, DWORD(std::size(short_path)));
    if (n && n < std::size(short_path)) source = short_path;
  }
  const std::wstring target = into.filename().wstring();
  if (!ascii(source) || !ascii(target)) return detail = "the ZIP's path has characters tar.exe cannot read", false;
  const int code = RunTool(L"\"" + tar + L"\" -xf \"" + source + L"\" -C \"" + target + L"\"", into.parent_path(),
                           progress);
  if (code == 0) return true;
  detail = code == -2 ? "stopped" : code == -1 ? "tar.exe could not start" : "tar.exe exit " + std::to_string(code);
  return false;
#else
  const std::string z = zip.string(), d = into.string();
  const std::vector<std::vector<std::string>> tools = {
      {"bsdtar", "-xf", z, "-C", d},
      {"unzip", "-q", "-o", z, "-d", d},
      {"python3", "-I", "-m", "zipfile", "-e", z, d},
  };
  for (const auto& argv : tools) {
    const int code = RunTool(argv, progress);
    if (code == -1) continue;  // not installed: the next one
    if (code == 0) return true;
    detail = code == -2 ? "stopped" : argv[0] + " exit " + std::to_string(code);
    return false;
  }
  detail = "no unpacking tool (bsdtar, unzip or python3) was found";
  return false;
#endif
}

// ------------------------------------------------------------------ data ---
const Info& InfoFor(Kind kind) {
  static const Info kInfo[kKindCount] = {
      {"hd-textures", "hd-textures", "https://bobandersonv115.cypher.usbx.me/filebrowser/api/public/dl/buJAl_ZK",
       "kk-4x-usv2.zip", 2561065736ull, 5322454913ull,
       "a3e240f37f05f28a095bf464cbe634b39ceec11404734ee35577ef3a5dc23bdf"},
      {"ps2-cutscenes", "ps2-cutscenes", "https://bobandersonv115.cypher.usbx.me/filebrowser/api/public/dl/seTbs14N",
       "KK_Cuts_AI.zip", 1871168114ull, 1878520474ull,
       "197eb43b76b0ef6057d07e7db06b9566994ee849b17a935d00f6e55b52bd2df8"},
  };
  return kInfo[int(kind)];
}

fs::path ContentRoot(const fs::path& user_dir) { return user_dir / "content"; }
fs::path InstallDir(const fs::path& user_dir, Kind kind) { return ContentRoot(user_dir) / InfoFor(kind).folder; }

void SetLog(std::function<void(bool, const std::string&)> log) { LogSink() = std::move(log); }

// --------------------------------------------------------------- service ---
struct Service::Impl {
  enum class JobType { kDownload, kImport, kRemove };
  struct Job {
    JobType type;
    Kind kind;
    fs::path zip;
  };
  enum class Want { kNone, kPause, kCancel };
  struct Slot {
    Status status;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> progress{0};
    Want want = Want::kNone;
    bool importing = false;
    std::atomic<bool> for_play{false};  // paused because the game was started (PauseForPlay)
    std::string url, sha;
    std::string installed_setting;  // the folder as the settings hold it (UTF-8)
    // Speed: (time, bytes) samples of the last few seconds.
    std::deque<std::pair<Clock::time_point, uint64_t>> samples;
  };

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::deque<Job> jobs;
  bool running = false;  // a job is running
  bool quit = false;
  bool ready = false;
  fs::path user_dir;
  mutable Slot slots[kKindCount];
  std::vector<Change> changes;
  std::set<int> applied_on_take;  // kinds whose install settings are in `changes`
  std::thread worker;
  Fetcher* fetcher = nullptr;

  fs::path Downloads() const { return ContentRoot(user_dir) / "downloads"; }
  fs::path Part(Kind k) const { return Downloads() / (std::string(InfoFor(k).id) + ".part"); }
  fs::path Zip(Kind k) const { return Downloads() / (std::string(InfoFor(k).id) + ".zip"); }
  fs::path StateFile(Kind k) const { return Downloads() / (std::string(InfoFor(k).id) + ".state"); }
  fs::path Root(Kind k) const { return InstallDir(user_dir, k); }
  fs::path InstalledFile(Kind k) const { return Root(k) / "installed.txt"; }
  Slot& S(Kind k) const { return slots[int(k)]; }

  // Under the lock.
  void SetStage(Kind k, Stage stage, Error error = Error::kNone) {
    auto& st = S(k).status;
    st.stage = stage;
    st.error = error;
    if (error == Error::kNone) st.problem = Problem::kNone;
  }
  void Update(Kind k, const std::function<void(Status&)>& fn) {
    std::lock_guard lock(mutex);
    fn(S(k).status);
  }

  void WriteState(Kind k, const std::string& state, uint64_t total) {
    WriteKeys(StateFile(k), {{"state", state},
                             {"total", std::to_string(total)},
                             {"url", S(k).url},
                             {"for_play", S(k).for_play.load() ? "1" : "0"}});
  }
  void ClearDownload(Kind k) {
    std::error_code ec;
    fs::remove(Part(k), ec);
    fs::remove(Zip(k), ec);
    fs::remove(StateFile(k), ec);
    S(k).for_play = false;
  }

  void Changes(Kind k, bool install, const std::string& folder) {
    const bool tex = k == Kind::kTexturePack;
    const char* on = tex ? "native_texture_pack" : "kk_ps2_cutscenes";
    const char* dir = tex ? "native_texture_pack_folder" : "kk_ps2_movie_folder";
    std::lock_guard lock(mutex);
    if (install) {
      changes.push_back({dir, folder, false, {}, {}});
      changes.push_back({on, "true", false, {}, {}});
      applied_on_take.insert(int(k));
    } else {
      changes.push_back({on, "false", false, dir, folder});
      changes.push_back({dir, {}, true, dir, folder});
    }
  }

  void ReadInstalled(Kind k, bool queue_changes) {
    const auto keys = ReadKeys(InstalledFile(k));
    auto& slot = S(k);
    std::lock_guard lock(mutex);
    slot.status.installed = false;
    if (keys.empty()) return;
    const fs::path folder = FromU8(keys.count("folder") ? keys.at("folder") : "");
    const bool ok = !folder.empty() && Exists(folder) &&
                    (k != Kind::kTexturePack || Exists(folder / "pack.txt"));
    if (!ok) return;
    slot.status.installed = true;
    slot.status.stage = Stage::kInstalled;
    slot.status.folder = folder;
    slot.status.installed_name = keys.count("name") ? keys.at("name") : "";
    slot.status.imported = keys.count("source") && keys.at("source") == "import";
    slot.installed_setting = U8(folder);
    if (queue_changes && keys.count("applied") && keys.at("applied") == "0") {
      const bool tex = k == Kind::kTexturePack;
      changes.push_back({tex ? "native_texture_pack_folder" : "kk_ps2_movie_folder", slot.installed_setting, false, {}, {}});
      changes.push_back({tex ? "native_texture_pack" : "kk_ps2_cutscenes", "true", false, {}, {}});
      applied_on_take.insert(int(k));
    }
  }

  bool Stopped(Kind k) { return S(k).stop.load(); }

  // A stop asked for while a job runs: Pause keeps the partial file, Cancel deletes it.
  void FinishStopped(Kind k, uint64_t total) {
    Want want;
    {
      std::lock_guard lock(mutex);
      want = S(k).want;
    }
    if (want == Want::kCancel || quit) {
      if (want == Want::kCancel) ClearDownload(k);
      std::lock_guard lock(mutex);
      SetStage(k, S(k).status.installed ? Stage::kInstalled : Stage::kNone);
      if (want == Want::kCancel) S(k).status.done = 0;
      Log(std::string(InfoFor(k).id) + (want == Want::kCancel ? ": cancelled" : ": stopped (closing)"));
      return;
    }
    WriteState(k, "paused", total);
    std::lock_guard lock(mutex);
    SetStage(k, Stage::kPaused);
    S(k).status.done = FileSize(Part(k)) ? FileSize(Part(k)) : FileSize(Zip(k));
    Log(std::string(InfoFor(k).id) + ": paused at " + std::to_string(S(k).status.done) + " bytes");
  }
  void Fail(Kind k, Error error, const std::string& why, Problem problem = Problem::kNone) {
    Warn(std::string(InfoFor(k).id) + ": " + why);
    std::lock_guard lock(mutex);
    SetStage(k, Stage::kFailed, error);
    S(k).status.problem = problem;
  }
  // Waits up to `seconds` unless a stop is asked for. True when stopped.
  bool Sleep(Kind k, double seconds) {
    std::unique_lock lock(mutex);
    cv.wait_for(lock, std::chrono::milliseconds(int(seconds * 1000)), [&] { return S(k).stop.load() || quit; });
    return S(k).stop.load() || quit;
  }

  uint64_t FreeSpace() const {
    std::error_code ec;
    fs::create_directories(ContentRoot(user_dir), ec);
    const auto info = fs::space(ContentRoot(user_dir), ec);
    return ec ? UINT64_MAX : uint64_t(info.available);
  }
  bool CheckSpace(Kind k, uint64_t needed) {
    const uint64_t free = FreeSpace();
    if (free >= needed) return true;
    {
      std::lock_guard lock(mutex);
      S(k).status.space_needed = needed;
      S(k).status.space_free = free;
    }
    Fail(k, Error::kSpace, "not enough free space: " + std::to_string(needed) + " needed, " + std::to_string(free) + " free");
    return false;
  }

  // ---------------------------------------------------------- download ---
  void RunDownload(Kind k) {
    const Info& info = InfoFor(k);
    std::string url, sha;
    {
      std::lock_guard lock(mutex);
      url = S(k).url;
      sha = S(k).sha;
    }
    std::error_code ec;
    fs::create_directories(Downloads(), ec);
    uint64_t total = ToU64(ReadKeys(StateFile(k))["total"]);
    WriteState(k, "downloading", total);
    if (!(Exists(Zip(k)) && total && FileSize(Zip(k)) == total)) {
      Update(k, [&](Status& st) {
        st.stage = Stage::kStarting;
        st.error = Error::kNone;
        st.done = FileSize(Part(k));
        st.total = total ? total : info.zip_size;
      });
      std::string detail;
      uint64_t probed = 0;
      for (int attempt = 0; attempt < 3 && !fetcher->Probe(url, probed, detail); ++attempt) {
        Warn(std::string(info.id) + ": size request failed: " + detail);
        if (attempt == 2 || Sleep(k, 2.0 * (attempt + 1))) break;
      }
      if (Stopped(k) || quit) return FinishStopped(k, total);
      if (!probed) {
        WriteState(k, "paused", total);
        return Fail(k, Error::kConnection, "no answer from the server: " + detail);
      }
      if (probed != info.zip_size) Log(std::string(info.id) + ": server size " + std::to_string(probed) +
                                       " (expected " + std::to_string(info.zip_size) + ")");
      total = probed;
      WriteState(k, "downloading", total);
      uint64_t have = FileSize(Part(k));
      if (have > total) {
        fs::remove(Part(k), ec);
        have = 0;
      }
      if (!CheckSpace(k, SpaceNeeded(total, have, info.unpacked))) return WriteState(k, "paused", total);
      {
        std::lock_guard lock(mutex);
        SetStage(k, Stage::kDownloading);
        S(k).status.total = total;
        S(k).progress = have;
        S(k).samples.clear();
      }
      Log(std::string(info.id) + ": downloading " + url + (have ? " from byte " + std::to_string(have) : ""));
      int failures = 0, restarts = 0;
      uint64_t last = have;
      while (have < total) {
        const Fetcher::Result r = fetcher->Fetch(url, Part(k), total, S(k).stop, S(k).progress);
        have = FileSize(Part(k));
        S(k).progress = have;
        if (r == Fetcher::Result::kStopped || Stopped(k) || quit) return FinishStopped(k, total);
        if (r == Fetcher::Result::kComplete && have == total) break;
        if (r == Fetcher::Result::kRestart) {
          fs::remove(Part(k), ec);
          have = 0;
          if (++restarts > 2) {
            WriteState(k, "paused", total);
            return Fail(k, Error::kServer, "the server does not continue downloads");
          }
          continue;
        }
        if (r == Fetcher::Result::kWrite) {
          WriteState(k, "paused", total);
          return Fail(k, Error::kWrite, "cannot write " + U8(Part(k)));
        }
        if (r == Fetcher::Result::kServer) {
          WriteState(k, "paused", total);
          return Fail(k, Error::kServer, "the server refused the download");
        }
        // Lost connection: try again (the count starts over while bytes keep arriving).
        if (have > last) failures = 0;
        last = have;
        if (++failures > 5) {
          WriteState(k, "paused", total);
          return Fail(k, Error::kConnection, "connection lost at " + std::to_string(have) + " bytes");
        }
        Log(std::string(info.id) + ": connection lost at " + std::to_string(have) + " bytes; continuing (try " +
            std::to_string(failures) + ")");
        if (Sleep(k, std::min(30.0, 1.0 * (1 << (failures - 1))))) return FinishStopped(k, total);
      }
      fs::remove(Zip(k), ec);
      fs::rename(Part(k), Zip(k), ec);
      if (ec) {
        WriteState(k, "paused", total);
        return Fail(k, Error::kWrite, "cannot rename the download");
      }
      Log(std::string(info.id) + ": downloaded " + std::to_string(total) + " bytes");
    }
    // The whole ZIP is here: its SHA-256 first.
    {
      std::lock_guard lock(mutex);
      SetStage(k, Stage::kVerifying);
      S(k).status.total = total;
      S(k).progress = 0;
    }
    std::ifstream in(Zip(k), std::ios::binary);
    Sha256 hash;
    std::vector<char> buf(4 << 20);
    uint64_t done = 0;
    while (in) {
      in.read(buf.data(), std::streamsize(buf.size()));
      const auto n = in.gcount();
      if (n <= 0) break;
      hash.Update(buf.data(), size_t(n));
      done += uint64_t(n);
      S(k).progress = done;
      if (Stopped(k) || quit) return FinishStopped(k, total);
    }
    in.close();
    const std::string digest = hash.HexDigest();
    if (done != total || (!sha.empty() && digest != sha)) {
      ClearDownload(k);
      return Fail(k, Error::kChecksum, "SHA-256 " + digest + " does not match " + sha + "; the download was deleted");
    }
    Log(std::string(info.id) + ": SHA-256 ok (" + digest + ")");
    if (Install(k, Zip(k), /*download=*/true)) ClearDownload(k);
  }

  // ----------------------------------------------------------- install ---
  bool Install(Kind k, const fs::path& zip, bool download) {
    const Info& info = InfoFor(k);
    std::vector<ZipEntry> entries;
    std::string error;
    {
      std::ifstream f(zip, std::ios::binary);
      const uint64_t size = FileSize(zip);
      auto read_at = [&](uint64_t at, uint8_t* out, size_t n) {
        f.clear();
        f.seekg(std::streamoff(at));
        f.read(reinterpret_cast<char*>(out), std::streamsize(n));
        return uint64_t(f.gcount()) == n;
      };
      if (!f || !ReadZipDirectory(read_at, size, entries, error)) {
        if (download) ClearDownload(k);
        Fail(k, Error::kBadPack, "not a ZIP file we can read: " + error, Problem::kNotZip);
        return false;
      }
    }
    const Layout layout = k == Kind::kTexturePack ? CheckPackLayout(entries) : ChooseMovieFolder(entries);
    if (layout.problem != Problem::kNone) {
      if (download) ClearDownload(k);
      Fail(k, Error::kBadPack, "the ZIP's layout is not usable (problem " + std::to_string(int(layout.problem)) + ")",
           layout.problem);
      return false;
    }
    if (!CheckSpace(k, layout.unpacked + kSpaceMargin)) return false;
    const fs::path root = Root(k);
    fs::path staging = root;
    staging += ".new";
    {
      std::lock_guard lock(mutex);
      SetStage(k, Stage::kUnpacking);
      S(k).status.total = layout.unpacked;
      S(k).progress = 0;
    }
    if (!RemoveAll(staging)) {
      Fail(k, Error::kWrite, "cannot clear " + U8(staging));
      return false;
    }
    std::error_code ec;
    fs::create_directories(staging, ec);
    Log(std::string(info.id) + ": unpacking " + U8(zip) + " (" + std::to_string(entries.size()) + " entries, " +
        std::to_string(layout.unpacked) + " bytes)");
    std::string detail;
    auto logged = Clock::now();
    const bool unpacked = UnpackZip(zip, staging, [&] {
      S(k).progress = DirBytes(staging);
      if (Clock::now() - logged >= std::chrono::seconds(15)) {
        logged = Clock::now();
        Log(std::string(info.id) + ": unpacked " + std::to_string(S(k).progress.load()) + " of " +
            std::to_string(layout.unpacked) + " bytes");
      }
      return !Stopped(k) && !quit;
    }, detail);
    if (!unpacked) {
      RemoveAll(staging);
      if (Stopped(k) || quit) {
        FinishStopped(k, FileSize(zip));
        return false;
      }
      Fail(k, Error::kUnpack, "unpacking failed: " + detail);
      return false;
    }
    S(k).progress = layout.unpacked;
    // Only what the directory listed, as regular files and folders inside the staging folder.
    std::map<std::string, uint64_t> expected;
    std::set<std::string> folders;
    for (const auto& e : entries) {
      std::string name = e.name;
      if (e.directory) {
        name.pop_back();
        folders.insert(name);
      } else {
        expected[name] = e.size;
      }
      for (std::string p = ParentOf(name); !p.empty(); p = ParentOf(p)) folders.insert(p);
    }
    size_t found = 0;
    for (fs::recursive_directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string rel = U8(it->path().lexically_relative(staging).generic_u8string());
      const auto st = it->symlink_status(ec);
      bool ok = false;
      if (fs::is_directory(st)) ok = folders.count(rel) != 0;
      else if (fs::is_regular_file(st)) {
        const auto e = expected.find(rel);
        ok = e != expected.end() && FileSize(it->path()) == e->second;
        found += ok;
      }
      if (!ok) {
        RemoveAll(staging);
        Fail(k, Error::kBadPack, "unexpected item after unpacking: " + rel, Problem::kUnsafeName);
        return false;
      }
    }
    if (ec || found != expected.size()) {
      RemoveAll(staging);
      Fail(k, Error::kUnpack, "unpacking left out " + std::to_string(expected.size() - found) + " files");
      return false;
    }
    const fs::path source = layout.folder.empty() ? staging : staging / FromU8(layout.folder);
    if (k == Kind::kTexturePack) {
      std::stringstream text;
      {
        std::ifstream m(source / "pack.txt", std::ios::binary);  // closed before anything is deleted
        text << m.rdbuf();
      }
      size_t rows = 0;
      const Problem problem = CheckManifest(text.str(), [&](const std::string& file) {
        std::error_code e2;
        return fs::is_regular_file(source / FromU8(file), e2);
      }, &rows);
      if (problem != Problem::kNone) {
        RemoveAll(staging);
        if (download) ClearDownload(k);
        Fail(k, Error::kBadPack, "pack.txt check failed (problem " + std::to_string(int(problem)) + ")", problem);
        return false;
      }
      Log(std::string(info.id) + ": pack.txt ok, " + std::to_string(rows) + " replacements");
    }
    // Swap in: the old install (if any) aside, the new folder in, then the old one deleted.
    fs::path old = root;
    old += ".old";
    RemoveAll(old);
    if (Exists(root)) fs::rename(root, old, ec);
    fs::create_directories(root, ec);
    const std::string name = layout.folder.empty() ? U8(zip.stem()) : LeafOf(layout.folder);
    const fs::path dest = (root / FromU8(name)).lexically_normal().make_preferred();
    fs::rename(source, dest, ec);
    if (ec) {
      RemoveAll(root);
      if (Exists(old)) fs::rename(old, root, ec);
      RemoveAll(staging);
      Fail(k, Error::kWrite, "cannot move the unpacked folder into " + U8(root));
      return false;
    }
    RemoveAll(staging);
    RemoveAll(old);
    WriteKeys(InstalledFile(k), {{"name", name},
                                 {"folder", U8(dest)},
                                 {"source", download ? "download" : "import"},
                                 {"applied", "0"}});
    ReadInstalled(k, /*queue_changes=*/true);
    Log(std::string(info.id) + ": installed in " + U8(dest));
    return true;
  }

  void RunImport(const fs::path& zip) {
    const Kind k = Kind::kTexturePack;
    {
      std::lock_guard lock(mutex);
      SetStage(k, Stage::kStarting);
      S(k).importing = true;
    }
    const bool ok = Install(k, zip, /*download=*/false);
    std::lock_guard lock(mutex);
    S(k).importing = false;
    // A failed import leaves an earlier install as it was.
    if (!ok && S(k).status.stage == Stage::kStarting) SetStage(k, S(k).status.installed ? Stage::kInstalled : Stage::kNone);
  }

  void RunRemove(Kind k) {
    std::string setting;
    {
      std::lock_guard lock(mutex);
      SetStage(k, Stage::kRemoving);
      setting = S(k).installed_setting;
    }
    if (!setting.empty()) Changes(k, /*install=*/false, setting);
    ClearDownload(k);
    const fs::path root = Root(k);
    fs::path staging = root, old = root;
    staging += ".new";
    old += ".old";
    RemoveAll(staging);
    RemoveAll(old);
    if (!RemoveAll(root)) return Fail(k, Error::kWrite, "cannot delete " + U8(root));
    std::lock_guard lock(mutex);
    auto& st = S(k).status;
    st = Status{};
    S(k).installed_setting.clear();
    Log(std::string(InfoFor(k).id) + ": removed");
  }

  void WorkerLoop() {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return quit || !jobs.empty(); });
        if (quit) return;
        job = jobs.front();
        jobs.pop_front();
        running = true;
        S(job.kind).stop = false;
        S(job.kind).want = Want::kNone;
      }
      switch (job.type) {
        case JobType::kDownload: RunDownload(job.kind); break;
        case JobType::kImport: RunImport(job.zip); break;
        case JobType::kRemove: RunRemove(job.kind); break;
      }
      std::lock_guard lock(mutex);
      running = false;
      cv.notify_all();
    }
  }

  void Push(Job job) {
    std::lock_guard lock(mutex);
    if (job.type != JobType::kRemove) SetStage(job.kind, Stage::kQueued);
    jobs.push_back(std::move(job));
    if (!worker.joinable()) worker = std::thread([this] { WorkerLoop(); });
    cv.notify_all();
  }
  bool Queued(Kind k) const {
    for (const auto& j : jobs)
      if (j.kind == k) return true;
    return false;
  }
};

Service& Service::Get() {
  static Service* service = new Service();  // never destroyed (see content.h)
  return *service;
}

Service::Service() : impl_(std::make_unique<Impl>()) {}

Service::~Service() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->quit = true;
    for (auto& s : impl_->slots) s.stop = true;
    impl_->cv.notify_all();
  }
  if (impl_->worker.joinable()) impl_->worker.join();
}

void Service::Init(const fs::path& user_dir, std::unique_ptr<Fetcher> fetcher) {
  if (impl_->ready || user_dir.empty()) return;
  fetcher_ = fetcher ? std::move(fetcher) : MakeSystemFetcher();
  impl_->fetcher = fetcher_.get();
  impl_->user_dir = user_dir;
#if defined(KK_DEV_TOOLS)
  if (const char* v = std::getenv("KK_DEV_CONTENT_DROP_AT"); v && *v) fetcher_->drop_at = ToU64(v);
#endif
  for (int i = 0; i < kKindCount; ++i) {
    const Kind k = Kind(i);
    auto& slot = impl_->S(k);
    if (slot.url.empty()) slot.url = InfoFor(k).url;
    if (slot.sha.empty()) slot.sha = InfoFor(k).sha256;
    // An install that was cut short: the old folder back, the half-made one away.
    const fs::path root = impl_->Root(k);
    fs::path staging = root, old = root;
    staging += ".new";
    old += ".old";
    RemoveAll(staging);
    std::error_code ec;
    if (Exists(old)) {
      if (!Exists(root)) fs::rename(old, root, ec);
      else RemoveAll(old);
    }
    impl_->ReadInstalled(k, /*queue_changes=*/true);
    const auto state = ReadKeys(impl_->StateFile(k));
    if (state.count("state")) {
      const uint64_t total = ToU64(state.count("total") ? state.at("total") : "");
      const uint64_t have = std::max(FileSize(impl_->Part(k)), FileSize(impl_->Zip(k)));
      slot.for_play = state.count("for_play") && state.at("for_play") == "1";
      if (state.at("state") == "downloading") {
        Log(std::string(InfoFor(k).id) + ": continuing the download from the last session (" + std::to_string(have) +
            " bytes kept)");
        impl_->Push({Impl::JobType::kDownload, k, {}});
      } else {
        std::lock_guard lock(impl_->mutex);
        impl_->SetStage(k, Stage::kPaused);
        slot.status.done = have;
        slot.status.total = total ? total : InfoFor(k).zip_size;
      }
    }
  }
  impl_->ready = true;
}

bool Service::Ready() const { return impl_->ready; }

void Service::OverrideUrl(Kind kind, std::string url, std::string sha256) {
  std::lock_guard lock(impl_->mutex);
  impl_->S(kind).url = std::move(url);
  impl_->S(kind).sha = std::move(sha256);
}

Status Service::Get(Kind kind) const {
  std::lock_guard lock(impl_->mutex);
  auto& slot = impl_->S(kind);
  Status st = slot.status;
  if (st.stage == Stage::kDownloading || st.stage == Stage::kVerifying || st.stage == Stage::kUnpacking)
    st.done = slot.progress.load();
  if (st.stage == Stage::kDownloading) {
    const auto now = Clock::now();
    if (slot.samples.empty() || now - slot.samples.back().first >= std::chrono::milliseconds(250))
      slot.samples.emplace_back(now, st.done);
    while (slot.samples.size() > 2 && now - slot.samples.front().first > std::chrono::seconds(5))
      slot.samples.pop_front();
    const double dt = std::chrono::duration<double>(slot.samples.back().first - slot.samples.front().first).count();
    if (dt >= 1.0 && slot.samples.back().second >= slot.samples.front().second)
      st.speed = double(slot.samples.back().second - slot.samples.front().second) / dt;
    st.seconds_left = SecondsLeft(st.done, st.total, st.speed);
  }
  return st;
}

void Service::Download(Kind kind) {
  if (!impl_->ready) return;
  {
    std::lock_guard lock(impl_->mutex);
    const auto& st = impl_->S(kind).status;
    if (st.Busy() || impl_->Queued(kind) || st.installed) return;
  }
  impl_->S(kind).for_play = false;
  // Kept from now on: a download still waiting for the one before it also goes on after a restart.
  impl_->WriteState(kind, "downloading", ToU64(ReadKeys(impl_->StateFile(kind))["total"]));
  impl_->Push({Impl::JobType::kDownload, kind, {}});
}

void Service::Pause(Kind kind) {
  std::lock_guard lock(impl_->mutex);
  auto& slot = impl_->S(kind);
  // Still waiting in the queue: out of it, kept as paused.
  for (auto it = impl_->jobs.begin(); it != impl_->jobs.end(); ++it)
    if (it->kind == kind && it->type == Impl::JobType::kDownload) {
      impl_->jobs.erase(it);
      const uint64_t total = ToU64(ReadKeys(impl_->StateFile(kind))["total"]);
      impl_->WriteState(kind, "paused", total);
      impl_->SetStage(kind, Stage::kPaused);
      slot.status.done = FileSize(impl_->Part(kind));
      return;
    }
  if (slot.status.stage == Stage::kStarting || slot.status.stage == Stage::kDownloading ||
      slot.status.stage == Stage::kVerifying) {
    slot.want = Impl::Want::kPause;
    slot.stop = true;
    impl_->cv.notify_all();
  }
}

void Service::PauseForPlay(Kind kind) {
  if (!impl_->ready) return;
  impl_->S(kind).for_play = true;
  Log(std::string(InfoFor(kind).id) + ": pausing while the game runs");
  Pause(kind);
}

bool Service::ResumeAfterPlay() {
  if (!impl_->ready) return false;
  bool any = false;
  for (int i = 0; i < kKindCount; ++i) {
    const Kind k = Kind(i);
    if (!impl_->S(k).for_play.exchange(false)) continue;
    Status st = Get(k);
    if (st.stage != Stage::kPaused && st.stage != Stage::kFailed) continue;
    Log(std::string(InfoFor(k).id) + ": continuing after the game (paused while it ran)");
    Download(k);
    any = true;
  }
  return any;
}

void Service::Cancel(Kind kind) {
  std::lock_guard lock(impl_->mutex);
  auto& slot = impl_->S(kind);
  for (auto it = impl_->jobs.begin(); it != impl_->jobs.end();)
    it = it->kind == kind && it->type == Impl::JobType::kDownload ? impl_->jobs.erase(it) : it + 1;
  const Stage stage = slot.status.stage;
  if (stage == Stage::kStarting || stage == Stage::kDownloading || stage == Stage::kVerifying ||
      (stage == Stage::kUnpacking && !slot.importing)) {
    slot.want = Impl::Want::kCancel;
    slot.stop = true;
    impl_->cv.notify_all();
    return;
  }
  if (stage == Stage::kPaused || stage == Stage::kQueued || (stage == Stage::kFailed && !slot.status.installed)) {
    impl_->ClearDownload(kind);
    impl_->SetStage(kind, slot.status.installed ? Stage::kInstalled : Stage::kNone);
    slot.status.done = 0;
  }
}

void Service::Remove(Kind kind) {
  if (!impl_->ready) return;
  Cancel(kind);
  impl_->Push({Impl::JobType::kRemove, kind, {}});
}

void Service::Import(const fs::path& zip) {
  if (!impl_->ready) return;
  {
    std::lock_guard lock(impl_->mutex);
    auto& st = impl_->S(Kind::kTexturePack).status;
    if (st.Busy() || impl_->Queued(Kind::kTexturePack)) {
      st.error = Error::kBusy;
      return;
    }
  }
  Log("importing " + U8(zip));
  impl_->Push({Impl::JobType::kImport, Kind::kTexturePack, zip});
}

std::vector<Change> Service::TakeChanges() {
  std::vector<Change> out;
  std::set<int> applied;
  {
    std::lock_guard lock(impl_->mutex);
    out.swap(impl_->changes);
    applied.swap(impl_->applied_on_take);
  }
  for (int k : applied) {
    auto keys = ReadKeys(impl_->InstalledFile(Kind(k)));
    if (keys.empty()) continue;
    keys["applied"] = "1";
    WriteKeys(impl_->InstalledFile(Kind(k)), keys);
  }
  return out;
}

bool Service::Idle() const {
  std::lock_guard lock(impl_->mutex);
  return !impl_->running && impl_->jobs.empty();
}

}  // namespace kk::content
