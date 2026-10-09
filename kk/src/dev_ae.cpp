// Developer-only test aid (KK_DEV_TOOLS): find out which of the game's after
// effects is which, by skipping them one at a time.
//
// The after effects are classes in the engine's post-processing code, with
// vtables at 0x820A3CF0-0x820A400C; slot 10 of each runs the effect every
// frame. Left out here: motion blur's (sub_82768DE8, motion_blur.cpp), and the
// distance fog's (sub_827648A8, fog.cpp) and the screen blur's (sub_82762A38,
// big_blur.cpp), which this found.
// KK_DEV_AE_CYCLE=<seconds> skips effect 0 for that long, then effect 1, and
// so on, then none, and starts over, logging
// "KK dev: AE skip <i> (<address>)" at each change. KK_DEV_AE_SKIP=<i> skips
// one effect for the whole run.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>

namespace {

constexpr uint32_t kApply[] = {0x82769868, 0x82769278, 0x827689A0, 0x82767A00, 0x82766828, 0x82765D78,
                               0x82765318, 0x82764DE0, 0x82764448, 0x82763A68, 0x827623C0, 0x82761E70,
                               0x827619E8, 0x827614E0, 0x82760DE8, 0x82760710};
constexpr int kCount = int(sizeof(kApply) / sizeof(kApply[0]));

int Skipped() {
  static const int fixed = [] {
    const char* v = std::getenv("KK_DEV_AE_SKIP");
    return v && *v ? std::atoi(v) : -1;
  }();
  if (fixed >= 0) return fixed;
  static const double cycle = [] {
    const char* v = std::getenv("KK_DEV_AE_CYCLE");
    return v && *v ? std::atof(v) : 0.0;
  }();
  if (cycle <= 0.0) return -1;
  static const auto start = std::chrono::steady_clock::now();
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  const int index = int(t / cycle) % (kCount + 1);
  static std::atomic<int> last{-2};
  if (last.exchange(index) != index) {
    if (index < kCount) {
      REXLOG_INFO("KK dev: AE skip {} ({:08X})", index, kApply[index]);
    } else {
      REXLOG_INFO("KK dev: AE skip none");
    }
  }
  return index;
}

}  // namespace

#define KK_DEV_AE_HOOK(N, ADDR)                      \
  REX_EXTERN(__imp__sub_##ADDR);                     \
  REX_HOOK_RAW(sub_##ADDR) {                         \
    if (Skipped() == N) return;                      \
    __imp__sub_##ADDR(ctx, base);                    \
  }

KK_DEV_AE_HOOK(0, 82769868)
KK_DEV_AE_HOOK(1, 82769278)
KK_DEV_AE_HOOK(2, 827689A0)
KK_DEV_AE_HOOK(3, 82767A00)
KK_DEV_AE_HOOK(4, 82766828)
KK_DEV_AE_HOOK(5, 82765D78)
KK_DEV_AE_HOOK(6, 82765318)
KK_DEV_AE_HOOK(7, 82764DE0)
KK_DEV_AE_HOOK(8, 82764448)
KK_DEV_AE_HOOK(9, 82763A68)
KK_DEV_AE_HOOK(10, 827623C0)
KK_DEV_AE_HOOK(11, 82761E70)
KK_DEV_AE_HOOK(12, 827619E8)
KK_DEV_AE_HOOK(13, 827614E0)
KK_DEV_AE_HOOK(14, 82760DE8)
KK_DEV_AE_HOOK(15, 82760710)
