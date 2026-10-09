#include "steam_shortcut.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#else
#include <spawn.h>
#include <unistd.h>  // environ (declared there with C linkage)
#endif

#include <rex/logging.h>

#include "http.h"

namespace kk::steam {
namespace {

namespace fs = std::filesystem;

constexpr const char* kAppName = "Peter Jackson's King Kong";

// King Kong on SteamGridDB (https://www.steamgriddb.com/game/5249080): its
// highest-scored grid, wide grid, hero, logo and icon, each saved under the
// shortcut's app ID with the suffix Steam looks for.
constexpr struct {
  const char* suffix;
  const char* url;
} kArt[] = {
    {"p.png", "https://cdn2.steamgriddb.com/grid/95eb83542315080493f9a00e01920fe6.png"},       // capsule, 600x900
    {".png", "https://cdn2.steamgriddb.com/grid/3b7dbfa5b5fd2ad4cedaf1dbf2d1f693.png"},        // wide, 920x430
    {"_hero.png", "https://cdn2.steamgriddb.com/hero/e3fc72518f213ad1f39ed9306da322b0.png"},  // header, 3840x1240
    {"_logo.png", "https://cdn2.steamgriddb.com/logo/5785c3651a5d911fca2b311b48c0896d.png"},  // over the header
    {"_icon.png", "https://cdn2.steamgriddb.com/icon/9638d91ecb50d03683088611e5e65d9f.png"},  // 512x512
};

std::string Utf8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
fs::path FromUtf8(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

#if defined(KK_DEV_TOOLS)
// Developer aid: KK_DEV_STEAM_ROOT=<folder> uses that folder as Steam's (a
// test copy with userdata/<id>/config), never closes or opens the real Steam,
// and KK_DEV_STEAM_NO_ART=1 skips the artwork downloads.
const char* DevRoot() {
  const char* v = std::getenv("KK_DEV_STEAM_ROOT");
  return v && *v ? v : nullptr;
}
#else
const char* DevRoot() { return nullptr; }
#endif

// ------------------------------------------------------------ binary VDF ---
// A map is a list of fields in file order, and values are kept as read (floats
// and 64-bit numbers as their bytes), so everything Steam or another tool
// wrote comes back out unchanged.
enum : uint8_t { kMap = 0, kString = 1, kInt32 = 2, kFloat32 = 3, kUint64 = 7, kEnd = 8 };

struct Node {
  uint8_t type = kMap;
  std::string key;
  std::string text;            // kString
  int32_t number = 0;          // kInt32
  std::vector<uint8_t> raw;    // kFloat32, kUint64
  std::vector<Node> children;  // kMap
};

bool ParseMap(const std::vector<uint8_t>& d, size_t& at, std::vector<Node>& out) {
  auto text = [&](std::string& s) {
    const auto end = std::find(d.begin() + ptrdiff_t(at), d.end(), uint8_t(0));
    if (end == d.end()) return false;
    s.assign(d.begin() + ptrdiff_t(at), end);
    at = size_t(end - d.begin()) + 1;
    return true;
  };
  for (;;) {
    if (at >= d.size()) return false;
    Node n;
    n.type = d[at++];
    if (n.type == kEnd) return true;
    if (!text(n.key)) return false;
    switch (n.type) {
      case kMap:
        if (!ParseMap(d, at, n.children)) return false;
        break;
      case kString:
        if (!text(n.text)) return false;
        break;
      case kInt32:
      case kFloat32:
      case kUint64: {
        const size_t size = n.type == kUint64 ? 8 : 4;
        if (at + size > d.size()) return false;
        if (n.type == kInt32)
          n.number = int32_t(uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 |
                             uint32_t(d[at + 3]) << 24);
        else
          n.raw.assign(d.begin() + ptrdiff_t(at), d.begin() + ptrdiff_t(at + size));
        at += size;
        break;
      }
      default:
        return false;  // a type this doesn't know: leave the file alone
    }
    out.push_back(std::move(n));
  }
}

bool ParseVdf(const std::vector<uint8_t>& d, std::vector<Node>& root) {
  size_t at = 0;
  return ParseMap(d, at, root) && at == d.size();
}

void WriteMap(const std::vector<Node>& map, std::vector<uint8_t>& out) {
  for (const Node& n : map) {
    out.push_back(n.type);
    out.insert(out.end(), n.key.begin(), n.key.end());
    out.push_back(0);
    if (n.type == kMap) {
      WriteMap(n.children, out);
    } else if (n.type == kString) {
      out.insert(out.end(), n.text.begin(), n.text.end());
      out.push_back(0);
    } else if (n.type == kInt32) {
      const uint32_t v = uint32_t(n.number);
      for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
    } else {
      out.insert(out.end(), n.raw.begin(), n.raw.end());
    }
  }
  out.push_back(kEnd);
}

// Steam's keys vary in case between versions (Exe, exe).
Node* Field(std::vector<Node>& map, std::string_view key) {
  for (Node& n : map) {
    if (n.key.size() == key.size() && std::equal(n.key.begin(), n.key.end(), key.begin(), [](char a, char b) {
          return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b));
        }))
      return &n;
  }
  return nullptr;
}
void SetString(std::vector<Node>& map, std::string_view key, std::string value) {
  Node* n = Field(map, key);
  if (!n) n = &map.emplace_back(Node{kString, std::string(key)});
  n->type = kString;
  n->text = std::move(value);
}
Node Str(const char* key, std::string value) { return Node{kString, key, std::move(value)}; }
Node Int(const char* key, int32_t value) { return Node{kInt32, key, {}, value}; }

uint32_t Crc32(std::string_view s) {
  uint32_t crc = 0xFFFFFFFFu;
  for (const char c : s) {
    crc ^= uint8_t(c);
    for (int k = 0; k < 8; ++k) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
  }
  return ~crc;
}

// The app ID Steam and other tools give a non-Steam game: the CRC-32 of its
// quoted exe and name, with the top bit set (no Steam app has it). Artwork is
// named after it unsigned; shortcuts.vdf stores it as a signed number.
uint32_t ShortcutAppId(const std::string& quoted_exe, const std::string& name) {
  return Crc32(quoted_exe + name) | 0x80000000u;
}

std::string Unquote(std::string s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

bool SamePath(const std::string& a, const fs::path& b) {
  std::error_code ec;
  auto normal = [&](const fs::path& p) {
#if defined(_WIN32)
    std::wstring w = fs::absolute(p, ec).lexically_normal().wstring();
    CharLowerBuffW(w.data(), DWORD(w.size()));
    return w;
#else
    return fs::absolute(p, ec).lexically_normal().native();
#endif
  };
  return !a.empty() && normal(FromUtf8(a)) == normal(b);
}

// --------------------------------------------------------- this program ---
// The file Steam starts: this exe, or the AppImage on Linux.
fs::path ThisExe() {
#if defined(_WIN32)
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  return n ? fs::path(std::wstring(buf, n)) : fs::path();
#else
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) return appimage;
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec);
#endif
}

// The folder options this copy was started with (the test copies' own user
// folder, say), so Steam starts it the same way.
std::string LaunchOptions() {
  static const char* const kKeep[] = {"--user_data_root=", "--game_data_root=", "--cache_root="};
  std::vector<std::string> args;
#if defined(_WIN32)
  int argc = 0;
  if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) args.push_back(Utf8(fs::path(argv[i])));
    LocalFree(argv);
  }
#else
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  bool first = true;
  for (std::string arg; std::getline(cmdline, arg, '\0'); first = false)
    if (!first) args.push_back(arg);
#endif
  std::string out;
  for (const std::string& a : args) {
    for (const char* k : kKeep) {
      if (a.rfind(k, 0) != 0) continue;
      std::string value = a.substr(std::strlen(k));
      while (!value.empty() && (value.back() == '\\' || value.back() == '/')) value.pop_back();
      out += (out.empty() ? "" : " ") + std::string(k) + "\"" + value + "\"";
    }
  }
  return out;
}

// ------------------------------------------------------------- Steam ---
std::vector<fs::path> SteamRoots() {
  std::vector<fs::path> roots;
#if defined(_WIN32)
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) ==
      ERROR_SUCCESS)
    roots.push_back(fs::path(buf).lexically_normal());
  if (const wchar_t* pf = _wgetenv(L"ProgramFiles(x86)")) roots.push_back(fs::path(pf) / L"Steam");
  roots.push_back(L"C:\\Program Files (x86)\\Steam");
#else
  if (const char* home = std::getenv("HOME")) {
    for (const char* dir : {".steam/steam", ".local/share/Steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam",
                            "snap/steam/common/.local/share/Steam"})
      roots.push_back(fs::path(home) / dir);
  }
#endif
  return roots;
}

bool ReadFile(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool WriteAtomically(const fs::path& p, const std::vector<uint8_t>& data) {
  fs::path temp = p;
  temp += ".kk-new";
  {
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) return false;
  }
#if defined(_WIN32)
  return MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code ec;
  fs::rename(temp, p, ec);
  return !ec;
#endif
}

// ------------------------------------------------------------ text VDF ---
// Steam Input is a per-game choice in localconfig.vdf (text VDF):
// UserLocalConfigStore/apps/<app id>/UseSteamControllerConfig, "0" for off.
// With it on, Steam turns the controller into its own virtual one and the
// game stops seeing it. The file is edited in place, so everything else in it
// stays exactly as Steam wrote it.
struct Token {
  enum Kind { kText, kOpen, kClose } kind;
  size_t begin, end;  // in the file (quotes included)
  std::string text;
  size_t match = 0;  // kOpen: its kClose
};

bool Tokenize(const std::string& s, std::vector<Token>& out) {
  std::vector<size_t> open;
  for (size_t i = 0; i < s.size();) {
    const char c = s[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++i;
    } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      i = s.find('\n', i);
      if (i == std::string::npos) i = s.size();
    } else if (c == '{' || c == '}') {
      out.push_back({c == '{' ? Token::kOpen : Token::kClose, i, i + 1});
      if (c == '{') {
        open.push_back(out.size() - 1);
      } else {
        if (open.empty()) return false;
        out[open.back()].match = out.size() - 1;
        open.pop_back();
      }
      ++i;
    } else if (c == '"') {
      size_t j = i + 1;
      while (j < s.size() && s[j] != '"') j += s[j] == '\\' ? 2 : 1;
      if (j >= s.size()) return false;
      out.push_back({Token::kText, i, j + 1, s.substr(i + 1, j - i - 1)});
      i = j + 1;
    } else {
      return false;  // unquoted text: not a file this expects
    }
  }
  return open.empty();
}

bool SameKey(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
         });
}

// The block `key { ... }` directly inside tokens [from, to): the index of its
// "{", or 0 when there is none.
size_t FindBlock(const std::vector<Token>& t, size_t from, size_t to, std::string_view key) {
  for (size_t i = from; i + 1 < to;) {
    if (t[i].kind != Token::kText) return 0;
    if (t[i + 1].kind == Token::kOpen) {
      if (SameKey(t[i].text, key)) return i + 1;
      i = t[i + 1].match + 1;
    } else {
      i += 2;  // a key and its value
    }
  }
  return 0;
}

// The value token of `key "value"` directly inside tokens [from, to), or 0.
size_t FindValue(const std::vector<Token>& t, size_t from, size_t to, std::string_view key) {
  for (size_t i = from; i + 1 < to;) {
    if (t[i].kind != Token::kText) return 0;
    if (t[i + 1].kind == Token::kOpen) {
      i = t[i + 1].match + 1;
    } else {
      if (SameKey(t[i].text, key)) return i + 1;
      i += 2;
    }
  }
  return 0;
}

bool DisableSteamInput(const fs::path& config_dir, uint32_t app_id) {
  const fs::path file = config_dir / "localconfig.vdf";
  std::vector<uint8_t> raw;
  if (!ReadFile(file, raw)) return false;
  std::string s(raw.begin(), raw.end());
  std::vector<Token> t;
  if (!Tokenize(s, t)) return false;
  const std::string nl = s.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  const std::string id = std::to_string(app_id);
  const std::string setting = "\"UseSteamControllerConfig\"\t\t\"0\"";
  // Where a "}" line starts, to insert before it.
  auto line_of = [&](size_t pos) {
    const size_t p = s.rfind('\n', pos);
    return p == std::string::npos ? 0 : p + 1;
  };
  const size_t root = FindBlock(t, 0, t.size(), "UserLocalConfigStore");
  if (!root) return false;
  const size_t apps = FindBlock(t, root + 1, t[root].match, "apps");
  const size_t app = apps ? FindBlock(t, apps + 1, t[apps].match, id) : 0;
  if (app) {
    if (const size_t v = FindValue(t, app + 1, t[app].match, "UseSteamControllerConfig")) {
      if (t[v].text == "0") return true;
      s.replace(t[v].begin, t[v].end - t[v].begin, "\"0\"");
    } else {
      s.insert(line_of(t[t[app].match].begin), "\t\t\t" + setting + nl);
    }
  } else if (apps) {
    s.insert(line_of(t[t[apps].match].begin),
             "\t\t\"" + id + "\"" + nl + "\t\t{" + nl + "\t\t\t" + setting + nl + "\t\t}" + nl);
  } else {
    s.insert(line_of(t[t[root].match].begin), "\t\"apps\"" + nl + "\t{" + nl + "\t\t\"" + id + "\"" + nl + "\t\t{" +
                                                  nl + "\t\t\t" + setting + nl + "\t\t}" + nl + "\t}" + nl);
  }
  std::error_code ec;
  fs::path backup = file;
  backup += ".kk-backup";
  fs::copy_file(file, backup, fs::copy_options::overwrite_existing, ec);
  return WriteAtomically(file, std::vector<uint8_t>(s.begin(), s.end()));
}

fs::path ShortcutsFile(const Location& where, const std::string& account) {
  return where.root / "userdata" / account / "config" / "shortcuts.vdf";
}

// The shortcut in a shortcuts.vdf list that starts exe, added by this or by hand.
Node* FindShortcut(std::vector<Node>& list, const fs::path& exe) {
  for (Node& n : list) {
    if (n.type != kMap) continue;
    const Node* e = Field(n.children, "exe");
    if (e && e->type == kString && SamePath(Unquote(e->text), exe)) return &n;
  }
  return nullptr;
}

void OpenSteamUrl(const char* url) {
#if defined(_WIN32)
  const std::string s(url);
  ShellExecuteW(nullptr, L"open", std::wstring(s.begin(), s.end()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
  const char* argv[] = {"xdg-open", url, nullptr};
  pid_t pid = 0;
  posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(argv), environ);
#endif
}

}  // namespace

std::optional<Location> Locate() {
  std::error_code ec;
  Location where;
  if (const char* dev = DevRoot()) {
    where.root = FromUtf8(dev);
  } else {
    for (const fs::path& root : SteamRoots()) {
      if (fs::is_directory(root / "userdata", ec)) {
        where.root = root;
        break;
      }
    }
  }
  if (where.root.empty()) return std::nullopt;
  std::vector<std::string> ids;
  for (const auto& e : fs::directory_iterator(where.root / "userdata", ec)) {
    const std::string name = Utf8(e.path().filename());
    if (e.is_directory(ec) && !name.empty() && name != "0" &&
        std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
      ids.push_back(name);
  }
  // The account signed in last, from loginusers.vdf: marked MostRecent, or
  // else the latest Timestamp (current Steam no longer writes MostRecent).
  std::vector<uint8_t> login;
  ReadFile(where.root / "config" / "loginusers.vdf", login);
  const std::string text(login.begin(), login.end());
  static const std::regex kUser(R"re("(\d{17})"\s*\{([^}]*)\})re");
  static const std::regex kRecent(R"re("MostRecent"\s*"1")re", std::regex::icase);
  static const std::regex kTime(R"re("Timestamp"\s*"(\d+)")re", std::regex::icase);
  std::string best;
  bool best_recent = false;
  uint64_t best_time = 0;
  for (std::sregex_iterator it(text.begin(), text.end(), kUser), end; it != end; ++it) {
    const std::string account = std::to_string(std::stoull((*it)[1]) - 76561197960265728ull);
    if (std::find(ids.begin(), ids.end(), account) == ids.end()) continue;
    const std::string body = (*it)[2];
    std::smatch m;
    const bool recent = std::regex_search(body, kRecent);
    const uint64_t time = std::regex_search(body, m, kTime) ? std::stoull(m[1]) : 0;
    if (best.empty() || (recent && !best_recent) || (recent == best_recent && time > best_time)) {
      best = account;
      best_recent = recent;
      best_time = time;
    }
  }
  where.accounts = best.empty() ? ids : std::vector<std::string>{best};
  if (where.accounts.empty()) return std::nullopt;
  return where;
}

bool StartedFromSteam() {
  if (DevRoot()) return false;
  for (const char* v : {"SteamGameId", "SteamAppId"})
    if (const char* s = std::getenv(v); s && *s) return true;
  return false;
}

bool HasShortcut(const Location& where) {
  const fs::path exe = ThisExe();
  for (const std::string& account : where.accounts) {
    std::vector<uint8_t> data;
    std::vector<Node> root;
    if (!ReadFile(ShortcutsFile(where, account), data) || !ParseVdf(data, root)) return false;
    Node* list = Field(root, "shortcuts");
    if (!list || list->type != kMap || !FindShortcut(list->children, exe)) return false;
  }
  return !where.accounts.empty();
}

bool SteamRunning() {
  if (DevRoot()) return false;
#if defined(_WIN32)
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W e{sizeof(e)};
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
    found = _wcsicmp(e.szExeFile, L"steam.exe") == 0;
  CloseHandle(snap);
  return found;
#else
  std::error_code ec;
  for (const auto& e : fs::directory_iterator("/proc", ec)) {
    std::ifstream comm(e.path() / "comm");
    std::string name;
    if (comm && std::getline(comm, name) && name == "steam") return true;
  }
  return false;
#endif
}

void CloseSteam() {
  if (!DevRoot()) OpenSteamUrl("steam://exit");
}

void OpenSteam() {
  if (!DevRoot()) OpenSteamUrl("steam://open/games");
}

std::vector<ArtPiece> DownloadArt() {
  std::vector<ArtPiece> art;
  bool skip = false, fake = false;
#if defined(KK_DEV_TOOLS)
  // KK_DEV_STEAM_NO_ART=1: none; =fake: placeholder files instead of downloads.
  if (const char* v = std::getenv("KK_DEV_STEAM_NO_ART")) skip = *v == '1', fake = std::strcmp(v, "fake") == 0;
#endif
  static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  for (const auto& a : kArt) {
    ArtPiece piece{a.suffix, a.url, {}};
    if (fake) {
      piece.data.assign(std::begin(kPng), std::end(kPng));
      art.push_back(std::move(piece));
      continue;
    }
    // Straight into memory (WinHTTP keeps no cache); a PNG only, so an error
    // page is never saved as artwork.
    if (!skip && (!HttpGet(a.url, piece.data) || piece.data.size() < 8 ||
                  std::memcmp(piece.data.data(), kPng, 8) != 0)) {
      REXLOG_WARN("KK: Steam artwork {} did not download", a.url);
      piece.data.clear();
    }
    art.push_back(std::move(piece));
  }
  return art;
}

Result AddShortcut(const Location& where, const std::vector<ArtPiece>& art) {
  Result r;
  const fs::path exe = ThisExe();
  if (exe.empty()) {
    r.error = "The port's own file wasn't found.";
    return r;
  }
  const std::string quoted_exe = "\"" + Utf8(exe) + "\"";
  const std::string start_dir = "\"" + Utf8(exe.parent_path()) + "\"";
  const std::string options = LaunchOptions();
  r.art = int(std::size(kArt));
  for (const std::string& account : where.accounts) {
    std::error_code ec;
    const fs::path vdf = ShortcutsFile(where, account);
    const fs::path grid = vdf.parent_path() / "grid";
    fs::create_directories(grid, ec);
    std::vector<uint8_t> old;
    const bool existed = ReadFile(vdf, old);
    std::vector<Node> root;
    if (!existed) root.push_back(Node{kMap, "shortcuts"});
    else if (!ParseVdf(old, root)) {
      r.error = "Steam's list of non-Steam games couldn't be read, so it was left as it is.";
      return r;
    }
    Node* list = Field(root, "shortcuts");
    if (!list) list = &root.emplace_back(Node{kMap, "shortcuts"});
    // Already there (added before, or by hand): keep its app ID and name, so
    // its play time, collections and the player's own name for it stay.
    uint32_t app_id = 0;
    std::vector<Node>* fields = nullptr;
    if (Node* existing = FindShortcut(list->children, exe)) {
      fields = &existing->children;
      Node* name = Field(*fields, "appname");
      Node* id = Field(*fields, "appid");
      if (!id || id->type != kInt32) {
        const std::string n = name && !name->text.empty() ? name->text : kAppName;
        fields->push_back(Int("appid", int32_t(ShortcutAppId(quoted_exe, n))));
        id = &fields->back();
      }
      app_id = uint32_t(id->number);
      if (const Node* sd = Field(*fields, "StartDir"); !sd || Unquote(sd->text).empty())
        SetString(*fields, "StartDir", start_dir);
      if (const Node* lo = Field(*fields, "LaunchOptions"); !options.empty() && (!lo || lo->text.empty()))
        SetString(*fields, "LaunchOptions", options);
    } else {
      app_id = ShortcutAppId(quoted_exe, kAppName);
      int next = 0;
      for (const Node& n : list->children)
        if (!n.key.empty() && std::all_of(n.key.begin(), n.key.end(), [](char c) { return c >= '0' && c <= '9'; }))
          next = std::max(next, std::atoi(n.key.c_str()) + 1);
      // The fields, names and order Steam itself writes.
      Node entry{kMap, std::to_string(next)};
      entry.children = {Int("appid", int32_t(app_id)), Str("appname", kAppName), Str("exe", quoted_exe),
                        Str("StartDir", start_dir), Str("icon", ""), Str("ShortcutPath", ""),
                        Str("LaunchOptions", options), Int("IsHidden", 0), Int("AllowDesktopConfig", 1),
                        Int("AllowOverlay", 1), Int("OpenVR", 0), Int("Devkit", 0), Str("DevkitGameID", ""),
                        Int("DevkitOverrideAppID", 0), Int("LastPlayTime", 0), Str("FlatpakAppID", ""),
                        Str("sortas", ""), Node{kMap, "tags"}};
      list->children.push_back(std::move(entry));
      fields = &list->children.back().children;
      r.added = true;
    }
    int saved = 0;
    for (const ArtPiece& piece : art) {
      if (piece.data.empty()) continue;
      const fs::path file = grid / (std::to_string(app_id) + piece.suffix);
      if (!WriteAtomically(file, piece.data)) continue;
      ++saved;
      if (std::strcmp(piece.suffix, "_icon.png") == 0) SetString(*fields, "icon", Utf8(file));
    }
    r.art = std::min(r.art, saved);
    if (existed) {
      fs::path backup = vdf;
      backup += ".kk-backup";
      fs::copy_file(vdf, backup, fs::copy_options::overwrite_existing, ec);
    }
    std::vector<uint8_t> out;
    WriteMap(root, out);
    if (!WriteAtomically(vdf, out)) {
      r.error = "Steam's list of non-Steam games couldn't be saved.";
      return r;
    }
    // The port reads controllers itself, so Steam Input stays off for it.
    const bool input_off = DisableSteamInput(vdf.parent_path(), app_id);
    REXLOG_INFO("KK: Steam: {} {} for account {} (app {}, {} artwork pictures, Steam Input {})",
                r.added ? "added" : "updated", Utf8(exe), account, app_id, saved,
                input_off ? "off" : "not changed");
  }
  r.ok = true;
  return r;
}

}  // namespace kk::steam
