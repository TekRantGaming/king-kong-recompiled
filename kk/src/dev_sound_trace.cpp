// Developer-only: KK_DEV_SOUND_TRACE=1 logs the arguments of the sound engine
// calls the game's menus make when they play a sound (found by profiling menu
// presses: sub_825FC978 -> sub_823FC940 -> sub_8275DDA8 -> ... -> the key
// lookup sub_82730078), to find which sound in SoundHeaders.db each one is.
#if defined(KK_DEV_TOOLS)

#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

namespace {
bool Tracing() {
  static const bool on = std::getenv("KK_DEV_SOUND_TRACE") != nullptr;
  return on;
}
}  // namespace

#define KK_TRACE_HOOK(addr)                                                                                  \
  REX_EXTERN(__imp__sub_##addr);                                                                             \
  REX_HOOK_RAW(sub_##addr) {                                                                                 \
    if (Tracing())                                                                                           \
      REXLOG_INFO("KK dev sound: sub_" #addr " r3={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X}",         \
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr));            \
    __imp__sub_##addr(ctx, base);                                                                            \
  }

KK_TRACE_HOOK(825FC978)
KK_TRACE_HOOK(823FC940)
KK_TRACE_HOOK(82730078)

// sub_8275DDA8(0, slot) plays a loaded sound: slot tables at 0x82C15070 + 248,
// 32 bytes each; the first word is the sound's key in SoundHeaders.db.
namespace {
std::string Words(uint8_t* base, uint32_t addr) {
  std::string s;
  for (int i = 0; i < 8; ++i) {
    const uint8_t* p = base + addr + i * 4;
    char buf[12];
    std::snprintf(buf, sizeof(buf), "%08X ", uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]);
    s += buf;
  }
  return s;
}
}  // namespace

REX_EXTERN(__imp__sub_8275DDA8);
REX_HOOK_RAW(sub_8275DDA8) {
  if (Tracing()) {
    REXLOG_INFO("KK dev sound: sub_8275DDA8 r3={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X}", ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr));
    static bool listed = false;
    if (!listed) {
      listed = true;
      std::string keys;
      for (uint32_t i = 0; i < 48; ++i) {
        const uint8_t* q = base + 0x82C15070u + 248 + i * 32;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%X=%08X ", i,
                      uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3]);
        keys += buf;
      }
      REXLOG_INFO("KK dev sound: slot keys {}", keys);
    }
    if (ctx.r3.u32 == 0 && ctx.r4.s32 >= 0 && ctx.r4.s32 < 48) {
      const uint32_t slot = 0x82C15070u + 248 + ctx.r4.u32 * 32;
      REXLOG_INFO("KK dev sound: slot {:X} at {:08X}: {}", ctx.r4.u32, slot, Words(base, slot));
    }
  }
  __imp__sub_8275DDA8(ctx, base);
}

#endif
