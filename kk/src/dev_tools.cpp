// Developer-only test aids, built with -DKK_DEV_TOOLS=ON and never shipped.
//
// KK_DEV_AUTOSKIP=1: a virtual pad 1 presses through to gameplay. It taps
// Start until the save menu opens (skipping the intro videos), then A to pick
// the first save, A to confirm loading it, A for Play, then Start six times to
// skip the story videos. When it has finished it logs "KK dev: gameplay
// reached" (test scripts wait for that line) and lets go of the pad.
//
// KK_DEV_WANDER=1 (with KK_DEV_AUTOSKIP): after that, keeps walking forward
// while slowly turning the camera, jumping now and then, so a test run moves
// through the level and loads new areas, effects and shaders.

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>

#include "menu_hook.h"

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
  if (now % 4000 < 120) StoreBE<uint16_t>(state + 4, kA);  // jump
  StoreBE<int16_t>(state + 10, 24000);                       // left stick: forward
  // Right stick: turn one way for 6 s, then the other, at a gentle pace.
  StoreBE<int16_t>(state + 12, int16_t((now / 6000) % 2 ? 9000 : -9000));
  return true;
}

bool AutoskipPad(uint32_t user, uint8_t* state) {
  if (user != 0) return false;
  const int64_t now = NowMs();
  if (g_done) return g_wander && WanderPad(state, now);
  uint16_t buttons = 0;
  if (const int64_t menu = g_menu_ms; menu < 0) {
    if (now % 3000 < kHold * 1000) buttons = kStart;  // intro videos
  } else {
    const double t = (now - menu) / 1000.0;
    for (const Press& p : kAfterMenu)
      if (t >= p.at && t < p.at + kHold) buttons = p.button;
    if (t >= kDoneAt && !g_done.exchange(true)) {
      REXLOG_INFO("KK dev: gameplay reached");
      if (const char* c = std::getenv("KK_DEV_CRASH"); c && *c == '1') {  // test the crash dump
        volatile int* p = nullptr;
        *p = 1;
      }
    }
  }
  std::memset(state, 0, 16);
  StoreBE<uint32_t>(state, uint32_t(now / 50));  // changes whenever the buttons might
  StoreBE<uint16_t>(state + 4, buttons);
  return true;
}

const bool g_installed = [] {
  const char* v = std::getenv("KK_DEV_AUTOSKIP");
  if (!v || !*v || *v == '0') return false;
  g_dev_pad_input = AutoskipPad;
  const char* w = std::getenv("KK_DEV_WANDER");
  g_wander = w && *w && *w != '0';
  OnSaveMenuShown([] { g_menu_ms = NowMs(); });
  return true;
}();

}  // namespace
}  // namespace kk
