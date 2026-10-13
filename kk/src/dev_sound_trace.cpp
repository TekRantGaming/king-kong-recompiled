// Developer-only: KK_DEV_SOUND_TRACE=1 logs the arguments of the sound engine
// calls the game's menus make when they play a sound (found by profiling menu
// presses: sub_825FC978 -> sub_823FC940 -> sub_8275DDA8 -> ... -> the key
// lookup sub_82730078), to find which sound in SoundHeaders.db each one is.
#if defined(KK_DEV_TOOLS)

#include <cstdio>
#include <cstdlib>
#include <string>
#include <map>

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

// The profile's four faders reach the game's sound engine on buses 9..12.
// Task 032: prove the real engine consumes changes made by the new pages.
REX_EXTERN(__imp__sub_82753978);
REX_HOOK_RAW(sub_82753978) {
  if (std::getenv("KK_DEV_AUDIO_TRACE") && ctx.r3.u32 >= 9 && ctx.r3.u32 <= 12)
    REXLOG_INFO("KK audio engine: bus {} gain {}", ctx.r3.u32, ctx.f1.f64);
  __imp__sub_82753978(ctx, base);
}

REX_EXTERN(__imp__sub_826037A0);
REX_HOOK_RAW(sub_826037A0) {
  if (ctx.lr == 0x824CDC0C && std::getenv("KK_DEV_AUDIO_TRACE")) {
    static std::map<uint32_t, double> shown;
    const auto it = shown.find(ctx.r3.u32);
    if (it == shown.end() || it->second != ctx.f1.f64) {
      shown[ctx.r3.u32] = ctx.f1.f64;
      REXLOG_INFO("KK audio old page: volume bar {} value {}", ctx.r3.u32, ctx.f1.f64);
    }
  }
  __imp__sub_826037A0(ctx, base);
}

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
