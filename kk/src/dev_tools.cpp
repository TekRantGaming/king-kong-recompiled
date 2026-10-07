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

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

#include "art.h"
#include "menu_hook.h"
#include "shader_pack.h"

namespace kk {
extern bool (*g_dev_pad_input)(uint32_t user, uint8_t* state);

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
std::vector<Step> g_script;
double g_script_end = 0;

uint32_t ParseButton(const std::string& s) {
  static const std::pair<const char*, uint32_t> kNames[] = {
      {"UP", 0x0001},   {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010},
      {"BACK", 0x0020}, {"LS", 0x0040},   {"RS", 0x0080},   {"LB", 0x0100},    {"RB", 0x0200},
      {"A", 0x1000},    {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000},     {"LT", kLT},
      {"RT", kRT}};
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
// the seconds count from the save menu opening instead (to see menus).
bool ShotsFromMenu() {
  static const bool from_menu = [] {
    const char* v = std::getenv("KK_DEV_SHOTS_FROM");
    return v && std::string(v) == "menu";
  }();
  return from_menu;
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
    return g_wander && WanderPad(state, now);
  }
  uint16_t buttons = 0;
  uint8_t triggers[2] = {0, 0};
  if (const int64_t menu = g_menu_ms; menu < 0) {
    if (now % 3000 < kHold * 1000) buttons = kStart;  // intro videos
  } else {
    const double t = (now - menu) / 1000.0;
    if (ShotsFromMenu()) TakeShots(t);
    uint32_t held = 0;
    if (g_script.empty()) {
      for (const Press& p : kAfterMenu)
        if (t >= p.at && t < p.at + kHold) held = p.button;
    } else {
      for (const Step& s : g_script)
        if (t >= s.at && t < s.at + s.hold) held |= s.buttons;
    }
    buttons = uint16_t(held);
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
  OnSaveMenuShown([] { g_menu_ms = NowMs(); });
  return true;
}();

}  // namespace
}  // namespace kk
