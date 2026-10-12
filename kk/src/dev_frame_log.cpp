// Developer-only: the game's frame time, frame by frame, to compare runs.
//
// KK_DEV_DT_LOG="from,to" (seconds after the save menu opens): every game frame
// in that window writes one row to frame_times.csv in the working folder:
//   frame (game frames since the window began), t (s since the save menu),
//   host_ms (host time since the previous frame), raw_dt (the game's frame time
//   before its time scale, 0x82C4EDBC), dt (0x82C636E0, what the game steps
//   with), game_ms (the game's millisecond clock, 0x82CCAE1C), then the guest
//   floats named in KK_DEV_DT_ADDRS="82D64254,...".
//
// KK_DEV_OBJ_AT="40,60,119" (seconds after the save menu opens, real time): at the first frame
// past each, objects_<t>.csv gets the world's active objects: index, address, identity flags (+0),
// position (global matrix translation) and first axis. Layout from the high frame rate work
// (dev_sample.cpp on nr-hfr): the universe 0x828E836C, +12 the current world; the world's active
// table runs from +304 to +292, 8 bytes an entry, the object first; an object's +12 points to its
// matrices, the global one first (axes, then the translation at +48).
//
// sub_8272CA20 sets the frame time once a frame from the kernel tick count in
// whole milliseconds (docs/high-frame-rate.md on branch nr-hfr), so two runs
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

#include <rex/hook.h>
#include <rex/logging.h>

namespace kk {
double DevSecondsSinceMenu();

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

bool Valid(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && (a & 3) == 0; }

void DumpObjects(const uint8_t* base) {
  static std::vector<double> times = [] {
    std::vector<double> t;
    if (const char* v = std::getenv("KK_DEV_OBJ_AT"); v && *v) {
      std::stringstream all(v);
      for (std::string x; std::getline(all, x, ',');) t.push_back(std::stod(x));
    }
    return t;
  }();
  if (times.empty()) return;
  const double now = DevSecondsSinceMenu();
  if (now < 0 || now < times.front()) return;
  const double at = times.front();
  times.erase(times.begin());
  const uint32_t world = Load32(base, 0x828E836C + 12);
  const uint32_t end = Valid(world) ? Load32(base, world + 292) : 0, start = Valid(world) ? Load32(base, world + 304) : 0;
  REXLOG_INFO("KK dev objects at {:.1f} s: world {:08X}, active table {:08X}..{:08X}", at, world, start, end);
  if (!Valid(world) || !Valid(start) || !Valid(end) || end < start || end - start > 8 * 20000) return;
  FILE* f = std::fopen(("objects_" + std::to_string(int(at * 10)) + ".csv").c_str(), "w");
  if (!f) return;
  std::fprintf(f, "t,i,obj,flags,x,y,z,ax,ay,az\n");
  int i = 0;
  for (uint32_t e = start; e + 8 <= end; e += 8, ++i) {
    const uint32_t obj = Load32(base, e);
    if (!Valid(obj)) continue;
    const uint32_t m = Load32(base, obj + 12);
    if (!Valid(m)) continue;
    std::fprintf(f, "%.4f,%d,%08X,%08X,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", now, i, obj, Load32(base, obj), LoadF(base, m + 48),
                 LoadF(base, m + 52), LoadF(base, m + 56), LoadF(base, m + 0), LoadF(base, m + 4), LoadF(base, m + 8));
  }
  std::fclose(f);
}

void LogFrame(const uint8_t* base) {
  DumpObjects(base);
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
    std::fprintf(f, "frame,t,host_ms,raw_dt,dt,game_ms");
    for (uint32_t a : w.addrs) std::fprintf(f, ",%08X", a);
    std::fprintf(f, "\n");
  }
  std::fprintf(f, "%llu,%.4f,%.3f,%.6f,%.6f,%u", (unsigned long long)frame++, t, host_ms, LoadF(base, kRawFrameTime),
               LoadF(base, kFrameTime), Load32(base, kGameMs));
  for (uint32_t a : w.addrs) std::fprintf(f, ",%.6f", LoadF(base, a));
  std::fprintf(f, "\n");
  if (frame % 30 == 0) std::fflush(f);
}

}  // namespace

// The game's own clock since the save menu opened: the sum of its frame times (each at most 63 ms, so a
// stall puts it behind real time by however long the stall was). -1 before the menu.
double DevGameSecondsSinceMenu() { return g_game_seconds; }

}  // namespace kk

REX_EXTERN(__imp__sub_8272CA20);
REX_HOOK_RAW(sub_8272CA20) {
  __imp__sub_8272CA20(ctx, base);
  kk::LogFrame(base);
}

#endif  // KK_DEV_TOOLS
