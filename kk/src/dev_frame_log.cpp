// Developer-only: the game's frame time, frame by frame, to compare runs.
//
// KK_DEV_DT_LOG="from,to" (seconds after the save menu opens): every game frame
// in that window writes one row to frame_times.csv in the working folder:
//   frame (game frames since the window began), t (s since the save menu),
//   host_ms (host time since the previous frame), raw_dt (the game's frame time
//   before its time scale, 0x82C4EDBC), dt (0x82C636E0, what the game steps
//   with), game_ms (the game's millisecond clock, 0x82CCAE1C), then the guest
//   floats named in KK_DEV_DT_ADDRS="82D64254,..." (Jack's height first).
//
// sub_8272CA20 (hooked in hfr_frame_time.cpp, which calls DevFrameLogTick after it) sets the frame time
// once a frame from the kernel tick count in
// whole milliseconds (docs/high-frame-rate.md), so two runs
// step the game alike only if their frames fall alike on millisecond ticks.

#if defined(KK_DEV_TOOLS)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/hook.h>
#include <rex/logging.h>

namespace kk {
double DevSecondsSinceMenu();
void DevFrameLogTick(const uint8_t* base);

namespace {

double g_game_seconds = -1;  // the game's clock (the sum of its frame times) since the save menu opened

constexpr uint32_t kFrameTime = 0x82C636E0;
constexpr uint32_t kRawFrameTime = 0x82C4EDBC;
constexpr uint32_t kGameMs = 0x82CCAE1C;

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

float LoadF(const uint8_t* base, uint32_t addr) {
  const uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

struct Window {
  double from = -1, to = -1;
  std::vector<uint32_t> addrs;
};

const Window& Settings() {
  static const Window w = [] {
    Window w;
    if (const char* v = std::getenv("KK_DEV_DT_LOG"); v && *v) {
      std::stringstream s(v);
      std::string a, b;
      std::getline(s, a, ',');
      std::getline(s, b, ',');
      w.from = std::stod(a);
      w.to = b.empty() ? w.from + 60 : std::stod(b);
    }
    if (const char* v = std::getenv("KK_DEV_DT_ADDRS"); v && *v) {
      std::stringstream s(v);
      for (std::string a; std::getline(s, a, ',');) w.addrs.push_back(uint32_t(std::stoul(a, nullptr, 16)));
    }
    return w;
  }();
  return w;
}

void LogFrame(const uint8_t* base) {
  if (g_game_seconds >= 0) {
    g_game_seconds += LoadF(base, kRawFrameTime);
  } else if (DevSecondsSinceMenu() >= 0) {
    g_game_seconds = 0;
  }
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last{};
  static FILE* f = nullptr;
  static bool over = false;
  static uint64_t frame = 0;
  const Clock::time_point now = Clock::now();
  const double host_ms = last == Clock::time_point{} ? 0.0 : std::chrono::duration<double, std::milli>(now - last).count();
  last = now;
  const Window& w = Settings();
  if (over || w.from < 0) return;
  const double t = DevSecondsSinceMenu();
  if (t < w.from) return;
  if (t > w.to) {
    if (f) std::fclose(f);
    f = nullptr;
    over = true;
    REXLOG_INFO("KK dev frame log: {} frames in frame_times.csv", frame);
    return;
  }
  if (!f) {
    f = std::fopen("frame_times.csv", "w");
    if (!f) {
      over = true;
      return;
    }
    std::fprintf(f, "frame,t,host_ms,raw_dt,dt,game_ms,cpu_ms");
    for (uint32_t a : w.addrs) std::fprintf(f, ",%08X", a);
    std::fprintf(f, "\n");
  }
  double cpu_ms = 0;
#if defined(_WIN32)
  FILETIME created{}, exited{}, kernel{}, user{};
  if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
    const uint64_t ticks = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                           ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
    cpu_ms = double(ticks) / 10000.0;
  }
#endif
  std::fprintf(f, "%llu,%.4f,%.3f,%.6f,%.6f,%u,%.4f", (unsigned long long)frame++, t, host_ms, LoadF(base, kRawFrameTime),
               LoadF(base, kFrameTime), Load32(base, kGameMs), cpu_ms);
  for (uint32_t a : w.addrs) std::fprintf(f, ",%.6f", LoadF(base, a));
  std::fprintf(f, "\n");
  if (frame % 30 == 0) std::fflush(f);
}

}  // namespace

// The game's own clock since the save menu opened: the sum of its frame times (each at most 63 ms, so a
// stall puts it behind real time by however long the stall was). -1 before the menu.
double DevGameSecondsSinceMenu() { return g_game_seconds; }

}  // namespace kk

// Called by the one hook on sub_8272CA20 (hfr_frame_time.cpp), after the frame time is set.
void kk::DevFrameLogTick(const uint8_t* base) { kk::LogFrame(base); }

#endif  // KK_DEV_TOOLS
