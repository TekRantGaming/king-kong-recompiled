// Developer-only (task 224): which trigger tests the level scripts ask, and what they answer.
//
// KK_DEV_TRIG_TRACE=1: wraps a set of TrigTest_* functions (the condition half of the scripts'
// trigger records, run by PRG_SSLoop and friends). Each call: r3 = the trigger record. After the
// original, logs "KK dev trig: <name> rec <record> caller <lr> -> <result>" the first time a record is
// seen and whenever its result changes, with the record's first 8 words, so a script that waits on a
// condition that never comes true shows which test and which data it waits on.
#if defined(KK_DEV_TOOLS)

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"

namespace kk {
double DevSecondsSinceMenu();

namespace {
bool On() {
  static const bool on = std::getenv("KK_DEV_TRIG_TRACE") != nullptr;
  return on;
}
bool Ok(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && (a & 3) == 0; }
}  // namespace

void DevTrigNote(const char* name, uint8_t* base, uint32_t rec, uint32_t lr, uint32_t result) {
  static std::mutex m;
  static std::unordered_map<uint64_t, uint32_t> last;
  std::lock_guard<std::mutex> lock(m);
  const uint64_t key = (uint64_t(rec) << 32) | uint32_t(reinterpret_cast<uintptr_t>(name));
  auto it = last.find(key);
  if (it != last.end() && it->second == result) return;
  last[key] = result;
  uint32_t w[24] = {};
  if (Ok(rec))
    for (int i = 0; i < 24; ++i) w[i] = hfr::Load32(base, rec + i * 4);
  REXLOG_INFO("KK dev trig: {:.2f} s {} rec {:08X} caller {:08X} -> {} [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}] +84 {:08X} +88 {} +92 {}",
              DevSecondsSinceMenu(), name, rec, lr, int32_t(result), w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[21], int32_t(w[22]), int32_t(w[23]));
}

}  // namespace kk

#define KK_TRIG(addr, name)                                                   \
  REX_EXTERN(__imp__sub_##addr);                                              \
  REX_HOOK_RAW(sub_##addr) {                                                  \
    if (!kk::On()) {                                                          \
      __imp__sub_##addr(ctx, base);                                           \
      return;                                                                 \
    }                                                                         \
    const uint32_t rec = ctx.r3.u32, lr = uint32_t(ctx.lr);                   \
    __imp__sub_##addr(ctx, base);                                             \
    kk::DevTrigNote(name, base, rec, lr, ctx.r3.u32);                         \
  }

KK_TRIG(8244E408, "NbrOfDeath")
KK_TRIG(82453078, "ActorLifeState")
KK_TRIG(8244EA98, "Enemy")
KK_TRIG(8244E9A8, "EnemyGAO")
KK_TRIG(8245D2E0, "ObjOfAIModelInBV")
KK_TRIG(8244F0E8, "PivotInBV")
KK_TRIG(82448250, "Munition")
KK_TRIG(82448478, "Arme")
KK_TRIG(82449E50, "Timer")
KK_TRIG(82453528, "Simple")
KK_TRIG(8244F188, "Active")
KK_TRIG(8244F1F0, "MultiGaoActive")
KK_TRIG(82448760, "ALL_GAO")
KK_TRIG(8244E8E0, "PivotInGroupOfBV")
KK_TRIG(8244E2E8, "Pivot_GoesINOUT_BV")
KK_TRIG(82453358, "Paf")
KK_TRIG(824763E0, "Spawner")
KK_TRIG(8244F020, "IsNearEnough")
KK_TRIG(82466878, "On_Screen")
KK_TRIG(82449F18, "CapaBit")

#endif
