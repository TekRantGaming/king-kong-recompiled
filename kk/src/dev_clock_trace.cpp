// Developer-only: the game clock from start-up to the first gameplay frames (task 158).
//
// KK_DEV_CLOCK_TRACE=1 with KK_DEV_SAMPLE_DIR writes clock.csv there, for the first 120 s:
//   kind (frame / event), frame (game frames so far), host (s since the first row), clock
//   (0x82CCAE1C, real seconds since its last reset), base (0x82CCC02C, pause time taken off),
//   pause (0x82CCC028, when the current pause began), gt (clock - base, the game time the
//   world reads), dt (0x82C636E0), raw (0x82C4EDBC), scale (0x828D178C), world
//   (0x828E836C +12), ai (AI passes, sub_8271D498, since the last frame), what, lr.
// sub_8272CA20 sets the clock each frame as (ticks - start) / 1000; sub_8272C990 (and
// sub_8285B160 / sub_8287DFE8) reset it to 0. The pause pair adds the paused span to the base:
// start = sub_824126A0 / sub_824125F0 (and the pause menu, sub_824B44E8), end = sub_825E66D0 / sub_82412670 /
// sub_824125C8 / sub_826EA8C0 (each logged on entry and exit).

#if defined(KK_DEV_TOOLS)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <rex/hook.h>

#include "hfr.h"

namespace kk {
void DevClockFrame(const uint8_t* base);
void DevClockMark(const uint8_t* base, const char* what, uint32_t lr);
void DevClockAi();

namespace {
using Clock = std::chrono::steady_clock;
FILE* g_file = nullptr;
uint64_t g_frame = 0;
uint32_t g_ai = 0;
Clock::time_point g_start{};

FILE* File() {
  static bool tried = false;
  if (!tried) {
    tried = true;
    const char* d = std::getenv("KK_DEV_SAMPLE_DIR");
    if (std::getenv("KK_DEV_CLOCK_TRACE") && d) {
      char path[512];
      std::snprintf(path, sizeof path, "%s/clock.csv", d);
      g_file = std::fopen(path, "w");
      if (g_file) {
        std::fprintf(g_file, "kind,frame,host,clock,base,pause,gt,dt,raw,scale,world,ai,what,lr\n");
        g_start = Clock::now();
      }
    }
  }
  if (g_file && std::chrono::duration<double>(Clock::now() - g_start).count() > 120.0) {
    std::fclose(g_file);
    g_file = nullptr;
  }
  return g_file;
}

void Row(const uint8_t* base, const char* kind, const char* what, uint32_t lr) {
  using namespace kk::hfr;
  FILE* f = File();
  if (!f) return;
  const double host = std::chrono::duration<double>(Clock::now() - g_start).count();
  const float clock = LoadF(base, 0x82CCAE1C), b = LoadF(base, 0x82CCC02C);
  std::fprintf(f, "%s,%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%.3f,%08X,%u,%s,%08X\n", kind,
               static_cast<unsigned long long>(g_frame), host, clock, b, LoadF(base, 0x82CCC028), clock - b,
               LoadF(base, kFrameTime), LoadF(base, 0x82C4EDBC), LoadF(base, 0x828D178C), Load32(base, 0x828E836C + 12),
               g_ai, what, lr);
}
}  // namespace

void DevClockFrame(const uint8_t* base) {
  static const bool on = std::getenv("KK_DEV_CLOCK_TRACE") != nullptr;
  if (!on) return;
  ++g_frame;
  Row(base, "frame", "", 0);
  g_ai = 0;
  if (g_file && g_frame % 120 == 0) std::fflush(g_file);
}

void DevClockMark(const uint8_t* base, const char* what, uint32_t lr) {
  static const bool on = std::getenv("KK_DEV_CLOCK_TRACE") != nullptr;
  if (on) Row(base, "event", what, lr);
}

void DevClockAi() { ++g_ai; }

}  // namespace kk

#define KK_CLOCK_HOOK(addr, name)                                   \
  REX_EXTERN(__imp__sub_##addr);                                    \
  REX_HOOK_RAW(sub_##addr) {                                        \
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);              \
    kk::DevClockMark(base, name " in", lr);                         \
    __imp__sub_##addr(ctx, base);                                   \
    kk::DevClockMark(base, name " out", lr);                        \
  }

KK_CLOCK_HOOK(8272C990, "reset 8272C990")
KK_CLOCK_HOOK(8285B160, "reset 8285B160")
KK_CLOCK_HOOK(8287DFE8, "reset 8287DFE8")
KK_CLOCK_HOOK(824126A0, "pause 824126A0")
KK_CLOCK_HOOK(824125F0, "pause 824125F0")
KK_CLOCK_HOOK(825E66D0, "resume 825E66D0")
KK_CLOCK_HOOK(82412670, "resume 82412670")
KK_CLOCK_HOOK(824125C8, "resume 824125C8")
KK_CLOCK_HOOK(826EA8C0, "resume 826EA8C0")

#endif
