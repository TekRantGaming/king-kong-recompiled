// Developer-only test aids, built with -DKK_DEV_TOOLS=ON and never shipped.
//
// KK_DEV_AUTOSKIP=1: a virtual pad 1 presses through to gameplay. It taps
// Start until the save menu opens (skipping the intro videos), then A to pick
// the first save, A to confirm loading it, A for Play, then Start six times to
// skip the story videos. When it has finished it logs "KK dev: gameplay
// reached" (test scripts wait for that line) and lets go of the pad.
//
// KK_DEV_SCRIPT="12:LB+RB+LT+RT/6,12.5:DOWN,..." (with KK_DEV_AUTOSKIP): replaces
// the presses after the save menu with this list: seconds after the menu
// opened, buttons joined by '+', optional "/hold seconds" (default 0.12).
// Buttons: A B X Y START BACK UP DOWN LEFT RIGHT LB RB LT RT LS RS.
//
// KK_DEV_WANDER=1 (with KK_DEV_AUTOSKIP): after that, keeps walking forward
// while slowly turning the camera, jumping now and then, so a test run moves
// through the level and loads new areas, effects and shaders.
//
// Investigation aids (details beside each; times are seconds after gameplay is
// reached, or after the save menu opens where noted):
//   KK_DEV_PAD_USER    play as player n+1
//   KK_DEV_CRASH/HANG  test the crash dump / hang report
//   KK_DEV_SHOTS       save game frames (guest output only, never the desktop);
//                      _FROM=menu or boot changes where the seconds count from
//   KK_DEV_FIND        search guest memory for strings (save menu times)
//   KK_DEV_SNAP_AT     diff the game's static data between times
//   KK_DEV_HEAPDIFF_AT what changed in all guest memory around a code's Confirm
//   KK_DEV_DUMP_AT     write all guest memory to files, to compare runs offline
//   KK_DEV_WATCH       hardware watchpoints: which function touched an address (dev_watch.cpp)
//   KK_DEV_PROF_AT     per-function call counts (kk-prof preset, dev_prof.cpp)
// Findings so far: the engine's heap layout differs between runs, so a value
// found at one address must be reached through code (a hook) next time.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "art.h"
#include "guest.h"
#include "menu_hook.h"
#include "shader_pack.h"

namespace kk {
extern bool (*g_dev_pad_input)(uint32_t user, uint8_t* state);
void DevWatchTick(double seconds_since_menu);
#if defined(KK_DEV_PROFILE)
void DevProfTick(double seconds_since_menu);
#else
inline void DevProfTick(double) {}
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint16_t kA = 0x1000, kStart = 0x0010;

// After the save menu opens: when (seconds) to press which button.
struct Press {
  double at;
  uint16_t button;
};
constexpr Press kAfterMenu[] = {{1, kA},       {6, kA},       {10, kA},      {18, kStart}, {23, kStart},
                                {28, kStart}, {33, kStart}, {38, kStart}, {43, kStart}};
constexpr double kDoneAt = 48;
constexpr double kHold = 0.12;

// A press from KK_DEV_SCRIPT: buttons (XINPUT bits; triggers as bits 16/17).
struct Step {
  double at, hold;
  uint32_t buttons;
};
constexpr uint32_t kLT = 1u << 16, kRT = 1u << 17;
// Right stick in scripts: RSUP, RSDOWN, RSLEFT, RSRIGHT push it all the way;
// add RSHALF (e.g. RSUP+RSHALF) for half way.
constexpr uint32_t kRsUp = 1u << 18, kRsDown = 1u << 19, kRsLeft = 1u << 20, kRsRight = 1u << 21,
                   kRsHalf = 1u << 22;
// SKIP presses Start only while a movie is playing, so skipping a chapter's
// movies never opens the pause menu once they're over.
constexpr uint32_t kSkip = 1u << 23;

// The game's movie player (*(0x82CCAE80)): +268 and +272 are set while a movie
// is starting or playing (sub_82723AF0 won't start another until both are 0).
bool MoviePlaying() {
  if (!g_guest_base) return false;
  auto load = [](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, g_guest_base + a, 4);
    return std::byteswap(v);
  };
  const uint32_t player = load(0x82CCAE80);
  return player && (load(player + 268) || load(player + 272));
}
std::vector<Step> g_script;
double g_script_end = 0;

uint32_t ParseButton(const std::string& s) {
  static const std::pair<const char*, uint32_t> kNames[] = {
      {"UP", 0x0001},   {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010},
      {"BACK", 0x0020}, {"LS", 0x0040},   {"RS", 0x0080},   {"LB", 0x0100},    {"RB", 0x0200},
      {"A", 0x1000},    {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000},     {"LT", kLT},
      {"RT", kRT},      {"RSUP", kRsUp},  {"RSDOWN", kRsDown}, {"RSLEFT", kRsLeft}, {"RSRIGHT", kRsRight},
      {"RSHALF", kRsHalf}, {"SKIP", kSkip}};
  for (auto& [name, bit] : kNames)
    if (s == name) return bit;
  REXLOG_WARN("KK dev: unknown button '{}' in KK_DEV_SCRIPT", s);
  return 0;
}

void ParseScript(const char* text) {
  std::stringstream all(text);
  for (std::string entry; std::getline(all, entry, ',');) {
    const size_t colon = entry.find(':');
    if (colon == std::string::npos) continue;
    Step step{std::stod(entry.substr(0, colon)), kHold, 0};
    std::string rest = entry.substr(colon + 1);
    if (const size_t slash = rest.find('/'); slash != std::string::npos) {
      step.hold = std::stod(rest.substr(slash + 1));
      rest = rest.substr(0, slash);
    }
    std::stringstream names(rest);
    for (std::string name; std::getline(names, name, '+');) step.buttons |= ParseButton(name);
    g_script_end = std::max(g_script_end, step.at + step.hold);
    g_script.push_back(step);
  }
}

std::atomic<int64_t> g_menu_ms{-1};  // ms since start when the save menu opened
std::atomic<bool> g_done{false};
bool g_wander = false;
bool g_wander_calm = false;  // KK_DEV_WANDER=2

int64_t NowMs() {
  static const auto start = Clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

template <typename T>
void StoreBE(uint8_t* p, T v) {
  v = std::byteswap(v);
  std::memcpy(p, &v, sizeof(v));
}

// XINPUT_STATE (big-endian): +0 packet number, +4 buttons, then triggers/sticks.
bool WanderPad(uint8_t* state, int64_t now) {
  std::memset(state, 0, 16);
  StoreBE<uint32_t>(state, uint32_t(now / 50));
  // Act like a player: jump, then cycle through the other buttons (pick up,
  // throw, reload and so on), aim with the left trigger and fire with the
  // right one, so combat effects get drawn too.
  static constexpr uint16_t kActions[] = {kA, 0x4000 /*X*/, 0x8000 /*Y*/, 0x2000 /*B*/};
  if (now % 1000 < 120) StoreBE<uint16_t>(state + 4, kActions[(now / 1000) % 4]);
  const int64_t combat = now % 6000;
  if (combat >= 2000 && combat < 4500) state[6] = 255;  // aim
  if (combat >= 3000 && combat < 4500 && now % 500 < 150) state[7] = 255;  // fire
  // Left stick: walk in a direction that circles round every 16 s, so walls
  // don't stop it for long. Right stick: keep turning the camera, so every
  // direction gets drawn.
  if (g_wander_calm) {
    // KK_DEV_WANDER=2 (filming): walk mostly forward, steering gently, and
    // pan the camera slowly from side to side.
    const double t = now / 1000.0;
    StoreBE<int16_t>(state + 8, int16_t(9000 * std::sin(t * 0.45)));
    StoreBE<int16_t>(state + 10, 24000);
    StoreBE<int16_t>(state + 12, int16_t(5000 * std::sin(t * 0.3)));
    return true;
  }
  const double angle = (now % 16000) / 16000.0 * 6.283185307;
  StoreBE<int16_t>(state + 8, int16_t(26000 * std::sin(angle)));
  StoreBE<int16_t>(state + 10, int16_t(26000 * std::cos(angle)));
  StoreBE<int16_t>(state + 12, 14000);
  StoreBE<int16_t>(state + 14, int16_t((now / 5000) % 2 ? 6000 : -6000));  // look up and down
  return true;
}

// KK_DEV_SHOTS="5,20" (with KK_DEV_AUTOSKIP): save the game's frame (guest
// output only) that many seconds after gameplay is reached, as shot_<s>.bmp in
// KK_DEV_SHOTS_DIR (default: the working folder). With KK_DEV_SHOTS_FROM=menu
// the seconds count from the save menu opening instead (to see menus). With
// KK_DEV_SHOTS_FROM=boot they count from the start, and nothing is pressed
// before the save menu (to see the startup movies).
std::string ShotsFrom() {
  const char* v = std::getenv("KK_DEV_SHOTS_FROM");
  return v ? v : "";
}
bool ShotsFromMenu() {
  static const bool from_menu = ShotsFrom() == "menu";
  return from_menu;
}
bool ShotsFromBoot() {
  static const bool from_boot = ShotsFrom() == "boot";
  return from_boot;
}

// t: seconds since gameplay was reached (or since the save menu opened).
void TakeShots(double t) {
  static std::vector<double> times = [] {
    std::vector<double> t;
    if (const char* v = std::getenv("KK_DEV_SHOTS"); v && *v) {
      std::stringstream all(v);
      for (std::string s; std::getline(all, s, ',');) t.push_back(std::stod(s));
    }
    return t;
  }();
  for (auto it = times.begin(); it != times.end();) {
    if (t < *it) {
      ++it;
      continue;
    }
    const char* dir = std::getenv("KK_DEV_SHOTS_DIR");
    art::CaptureFrame(std::filesystem::path(dir && *dir ? dir : ".") / ("shot_" + std::to_string(int(*it * 10)) + ".bmp"));
    it = times.erase(it);
  }
}

// KK_DEV_FIND="KKst0ry,8wonder" with KK_DEV_FIND_AT="70,95" (seconds from the
// save menu opening): search guest memory for those strings (ASCII and UTF-16,
// both byte orders) and log every hit with the bytes around it.
void FindInMemory() {
#if defined(_WIN32)
  if (!g_guest_base) return;
  std::vector<std::pair<std::string, std::string>> needles;  // (label, bytes)
  if (const char* v = std::getenv("KK_DEV_FIND"); v && *v) {
    std::stringstream all(v);
    for (std::string w; std::getline(all, w, ',');) {
      needles.push_back({w + " ascii", w});
      std::string be, le;
      for (char c : w) {
        be += '\0', be += c;
        le += c, le += '\0';
      }
      needles.push_back({w + " utf16be", be});
      needles.push_back({w + " utf16le", le});
    }
  }
  int hits = 0;
  for (uint64_t a = 0x40000000; a < 0xC0000000;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(g_guest_base + a, &mbi, sizeof(mbi))) break;
    const uint64_t begin = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - g_guest_base);
    const uint64_t end = std::min<uint64_t>(begin + mbi.RegionSize, 0xC0000000);
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                          (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
    if (readable && end > a) {
      const char* base = reinterpret_cast<const char*>(g_guest_base + a);
      const std::string_view region(base, size_t(end - a));
      for (const auto& [label, bytes] : needles) {
        for (size_t pos = region.find(bytes); pos != std::string_view::npos && hits < 200;
             pos = region.find(bytes, pos + 1)) {
          const size_t from = pos >= 32 ? pos - 32 : 0, to = std::min(region.size(), pos + bytes.size() + 32);
          std::string hex, text;
          for (size_t i = from; i < to; ++i) {
            char b[4];
            std::snprintf(b, sizeof(b), "%02X", uint8_t(region[i]));
            hex += b;
            text += (region[i] >= 32 && region[i] < 127) ? region[i] : '.';
          }
          REXLOG_INFO("KK dev find: '{}' at {:08X} (context from {:08X}) {} | {}", label, uint32_t(a + pos),
                      uint32_t(a + from), hex, text);
          ++hits;
        }
      }
    }
    a = end;
  }
  REXLOG_INFO("KK dev find: done, {} hits", hits);
#endif
}

void FindAtTimes(double t) {
  static std::vector<double> times = [] {
    std::vector<double> out;
    if (const char* v = std::getenv("KK_DEV_FIND_AT"); v && *v) {
      std::stringstream all(v);
      for (std::string s; std::getline(all, s, ',');) out.push_back(std::stod(s));
    }
    return out;
  }();
  for (auto it = times.begin(); it != times.end();) {
    if (t < *it) {
      ++it;
      continue;
    }
    REXLOG_INFO("KK dev find: searching at {:.1f} s", t);
    FindInMemory();
    it = times.erase(it);
  }
}

// KK_DEV_SNAP_AT="57,60,63" (seconds from the save menu opening): copy the
// game's static data (0x82000000-0x83100000) at each time, and at the last one
// write snap_diff.txt in the working folder: every byte that changed between
// snapshots, with its value in each one.
void SnapAtTimes(double t) {
  constexpr uint32_t kBegin = 0x82000000, kEnd = 0x83100000;
  static std::vector<double> times = [] {
    std::vector<double> out;
    if (const char* v = std::getenv("KK_DEV_SNAP_AT"); v && *v) {
      std::stringstream all(v);
      for (std::string s; std::getline(all, s, ',');) out.push_back(std::stod(s));
    }
    return out;
  }();
  static std::vector<std::vector<uint8_t>> snaps;
  static const size_t wanted = times.size();
  if (!g_guest_base || times.empty() || t < times.front()) return;
  times.erase(times.begin());
  snaps.emplace_back(g_guest_base + kBegin, g_guest_base + kEnd);
  REXLOG_INFO("KK dev snap {} at {:.1f} s", snaps.size(), t);
  if (snaps.size() != wanted) return;
  FILE* f = std::fopen("snap_diff.txt", "w");
  if (!f) return;
  size_t changed = 0;
  for (size_t i = 0; i < kEnd - kBegin; ++i) {
    bool differs = false;
    for (size_t k = 1; k < snaps.size() && !differs; ++k) differs = snaps[k][i] != snaps[0][i];
    if (!differs) continue;
    std::fprintf(f, "%08X", unsigned(kBegin + i));
    for (const auto& s : snaps) std::fprintf(f, " %02X", s[i]);
    std::fprintf(f, "\n");
    ++changed;
  }
  std::fclose(f);
  REXLOG_INFO("KK dev snap: wrote snap_diff.txt, {} bytes changed", changed);
}

// KK_DEV_HEAPDIFF_AT="a,b,c,d" (seconds from the save menu opening): what a
// cheat code changes. a = right code typed, b = after its Confirm, c = wrong
// code typed, d = after its Confirm. Writes heapdiff.txt: every byte of guest
// memory that changed from a to b and then kept its new value through c and d
// (so the wrong code's Confirm and the screen change did not touch it).
void HeapDiffAtTimes(double t) {
#if defined(_WIN32)
  struct Region {
    uint32_t guest, size;
  };
  struct Cand {
    uint32_t addr;
    uint8_t before, after;
  };
  static std::vector<double> times = [] {
    std::vector<double> out;
    if (const char* v = std::getenv("KK_DEV_HEAPDIFF_AT"); v && *v) {
      std::stringstream all(v);
      for (std::string s; std::getline(all, s, ',');) out.push_back(std::stod(s));
    }
    return out;
  }();
  static int step = 0;
  static std::vector<Region> regions;
  static std::vector<uint8_t> first;
  static std::vector<Cand> cands;
  if (!g_guest_base || times.empty() || t < times.front()) return;
  times.erase(times.begin());
  if (step == 0) {
    for (uint64_t a = 0x40000000; a < 0xC0000000;) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(g_guest_base + a, &mbi, sizeof(mbi))) break;
      const uint64_t begin = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - g_guest_base);
      const uint64_t end = std::min<uint64_t>(begin + mbi.RegionSize, 0xC0000000);
      const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE));
      if (readable && end > a) regions.push_back({uint32_t(a), uint32_t(end - a)});
      a = end;
    }
    for (const auto& r : regions) first.insert(first.end(), g_guest_base + r.guest, g_guest_base + r.guest + r.size);
    REXLOG_INFO("KK dev heapdiff: snapshot a, {} MB", first.size() >> 20);
  } else if (step == 1) {
    size_t o = 0;
    for (const auto& r : regions) {
      const uint8_t* now = g_guest_base + r.guest;
      for (uint32_t i = 0; i < r.size; ++i)
        if (now[i] != first[o + i]) cands.push_back({r.guest + i, first[o + i], now[i]});
      o += r.size;
    }
    std::vector<uint8_t>().swap(first);
    REXLOG_INFO("KK dev heapdiff: b, {} bytes changed", cands.size());
  } else {
    size_t kept = 0;
    for (const auto& c : cands)
      if (g_guest_base[c.addr] == c.after) cands[kept++] = c;
    cands.resize(kept);
    REXLOG_INFO("KK dev heapdiff: {}, {} bytes still hold their new value", step == 2 ? "c" : "d", kept);
    if (step == 3) {
      if (FILE* f = std::fopen("heapdiff.txt", "w")) {
        for (const auto& c : cands) std::fprintf(f, "%08X %02X %02X\n", c.addr, c.before, c.after);
        std::fclose(f);
      }
      REXLOG_INFO("KK dev heapdiff: wrote heapdiff.txt");
    }
  }
  ++step;
#else
  (void)t;
#endif
}

// KK_DEV_DUMP_AT="80,84" with KK_DEV_DUMP_DIR (seconds from the save menu
// opening): write all readable guest memory (0x40000000-0xC0000000) to
// dump_<n>.bin there, with dump_regions.txt listing "guest_start size" per
// region in file order. For comparing runs offline.
void DumpAtTimes(double t) {
#if defined(_WIN32)
  static std::vector<double> times = [] {
    std::vector<double> out;
    if (const char* v = std::getenv("KK_DEV_DUMP_AT"); v && *v) {
      std::stringstream all(v);
      for (std::string s; std::getline(all, s, ',');) out.push_back(std::stod(s));
    }
    return out;
  }();
  static int n = 0;
  if (!g_guest_base || times.empty() || t < times.front()) return;
  times.erase(times.begin());
  const char* d = std::getenv("KK_DEV_DUMP_DIR");
  const std::filesystem::path dir = d && *d ? d : ".";
  FILE* bin = std::fopen((dir / ("dump_" + std::to_string(n) + ".bin")).string().c_str(), "wb");
  FILE* map = n == 0 ? std::fopen((dir / "dump_regions.txt").string().c_str(), "w") : nullptr;
  size_t total = 0;
  for (uint64_t a = 0x40000000; bin && a < 0xC0000000;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(g_guest_base + a, &mbi, sizeof(mbi))) break;
    const uint64_t begin = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - g_guest_base);
    const uint64_t end = std::min<uint64_t>(begin + mbi.RegionSize, 0xC0000000);
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                          (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE));
    if (readable && end > a) {
      std::fwrite(g_guest_base + a, 1, size_t(end - a), bin);
      if (map) std::fprintf(map, "%08X %llu\n", unsigned(a), static_cast<unsigned long long>(end - a));
      total += size_t(end - a);
    }
    a = end;
  }
  if (bin) std::fclose(bin);
  if (map) std::fclose(map);
  REXLOG_INFO("KK dev dump: dump_{} at {:.1f} s, {} MB", n, t, total >> 20);
  ++n;
#else
  (void)t;
#endif
}

bool AutoskipPad(uint32_t user, uint8_t* state) {
  // KK_DEV_PAD_USER=n: be player n+1 instead of player 1.
  static const uint32_t pad_user = [] {
    const char* u = std::getenv("KK_DEV_PAD_USER");
    return u && *u ? uint32_t(std::atoi(u)) : 0u;
  }();
  if (user != pad_user) return false;
  const int64_t now = NowMs();
  if (g_done) {
    static const int64_t done_ms = now;
    TakeShots((now - (ShotsFromMenu() ? g_menu_ms.load() : done_ms)) / 1000.0);
    FindAtTimes((now - g_menu_ms.load()) / 1000.0);
    SnapAtTimes((now - g_menu_ms.load()) / 1000.0);
    DevWatchTick((now - g_menu_ms.load()) / 1000.0);
    DevProfTick((now - g_menu_ms.load()) / 1000.0);
    HeapDiffAtTimes((now - g_menu_ms.load()) / 1000.0);
    DumpAtTimes((now - g_menu_ms.load()) / 1000.0);
    return g_wander && WanderPad(state, now);
  }
  uint16_t buttons = 0;
  uint8_t triggers[2] = {0, 0};
  uint32_t held = 0;
  if (const int64_t menu = g_menu_ms; menu < 0) {
    if (ShotsFromBoot()) {
      TakeShots(now / 1000.0);
      return false;
    }
    if (now % 3000 < kHold * 1000) buttons = kStart;  // intro videos
  } else {
    const double t = (now - menu) / 1000.0;
    if (ShotsFromMenu()) TakeShots(t);
    FindAtTimes(t);
    SnapAtTimes(t);
    DevWatchTick(t);
    DevProfTick(t);
    HeapDiffAtTimes(t);
    DumpAtTimes(t);
    if (g_script.empty()) {
      for (const Press& p : kAfterMenu)
        if (t >= p.at && t < p.at + kHold) held = p.button;
    } else {
      for (const Step& s : g_script)
        if (t >= s.at && t < s.at + s.hold) held |= s.buttons;
    }
    buttons = uint16_t(held);
    if ((held & kSkip) && MoviePlaying()) buttons |= kStart;
    if (held & kLT) triggers[0] = 255;
    if (held & kRT) triggers[1] = 255;
    const double done_at = g_script.empty() ? kDoneAt : g_script_end + 1;
    if (t >= done_at && !g_done.exchange(true)) {
      REXLOG_INFO("KK dev: gameplay reached");
      if (const char* c = std::getenv("KK_DEV_CRASH"); c && *c == '1') {  // test the crash dump
        volatile int* p = nullptr;
        *p = 1;
      }
      if (const char* h = std::getenv("KK_DEV_HANG"); h && *h == '1') {  // test the hang report
        REXLOG_INFO("KK dev: freezing the game thread for 25 s");
        std::this_thread::sleep_for(std::chrono::seconds(25));
      }
    }
  }
  std::memset(state, 0, 16);
  StoreBE<uint32_t>(state, uint32_t(now / 50));  // changes whenever the buttons might
  StoreBE<uint16_t>(state + 4, buttons);
  state[6] = triggers[0];
  state[7] = triggers[1];
  const int16_t push = (held & kRsHalf) ? 16384 : 32767;
  if (held & (kRsLeft | kRsRight)) StoreBE<int16_t>(state + 12, int16_t((held & kRsLeft) ? -push : push));
  if (held & (kRsUp | kRsDown)) StoreBE<int16_t>(state + 14, int16_t((held & kRsDown) ? -push : push));
  return true;
}

// KK_DEV_MERGE="from|into": merge one shader storage file into another and exit
// (tests the launcher's shader pack merge).
const bool g_merge_test = [] {
  const char* v = std::getenv("KK_DEV_MERGE");
  if (!v || !*v) return false;
  const std::string s = v;
  const size_t bar = s.find('|');
  const int added = MergeShaderStorageFile(s.substr(0, bar), s.substr(bar + 1));
  if (FILE* f = std::fopen("merge_result.txt", "w")) {  // a GUI app has no console
    std::fprintf(f, "merged: %d\n", added);
    std::fclose(f);
  }
  std::exit(added < 0 ? 1 : 0);
}();

const bool g_installed = [] {
  const char* v = std::getenv("KK_DEV_AUTOSKIP");
  if (!v || !*v || *v == '0') return false;
  g_dev_pad_input = AutoskipPad;
  if (const char* s = std::getenv("KK_DEV_SCRIPT"); s && *s) ParseScript(s);
  const char* w = std::getenv("KK_DEV_WANDER");
  g_wander = w && *w && *w != '0';
  g_wander_calm = w && *w == '2';
  OnSaveMenuShown([] { g_menu_ms = NowMs(); });
  return true;
}();

}  // namespace
}  // namespace kk
