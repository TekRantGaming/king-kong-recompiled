// Photo mode (task 208): see photo_mode.h.

#include "photo_mode.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>

#include "play_internal.h"

namespace kk::photo {
namespace {

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

#if defined(KK_DEV_TOOLS)
// KK_DEV_PHOTO_TRACE=1: once a second, how often the world display (sub_82857E80) and its main view
// (sub_828732E8) ran, with the pause state, to see what the game skips while paused.
bool TraceOn() {
  static const bool on = std::getenv("KK_DEV_PHOTO_TRACE") != nullptr;
  return on;
}
std::atomic<uint32_t> g_world_calls{0}, g_view_calls{0}, g_world_lr{0}, g_world_r4{0};
#endif

}  // namespace

void Frame(uint8_t* base) {
#if defined(KK_DEV_TOOLS)
  if (!TraceOn()) return;
  static uint32_t frames = 0;
  static auto last = std::chrono::steady_clock::now();
  ++frames;
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(1)) return;
  last = now;
  const uint32_t ai = Load32(base, 0x82CCBF64);
  const uint32_t vars = ai ? Load32(base, ai + 0x40) : 0;
  const uint32_t paused = vars ? Load32(base, vars + 0x1C) : 0;
  REXLOG_INFO("KK photo trace: frames {} world {} (lr {:08X} r4 {:08X}) view {} paused {} menu {} universe+8 {:08X} "
              "menuflag {}",
              frames, g_world_calls.exchange(0), g_world_lr.load(), g_world_r4.load(), g_view_calls.exchange(0),
              paused, play::PauseMenuOpen(), Load32(base, 0x828E836C + 8), Load32(base, 0x828D1780));
  frames = 0;
#else
  (void)base;
#endif
}

}  // namespace kk::photo

#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_82857E80);
REX_HOOK_RAW(sub_82857E80) {
  ++kk::photo::g_world_calls;
  kk::photo::g_world_lr = uint32_t(ctx.lr);
  kk::photo::g_world_r4 = ctx.r4.u32;
  __imp__sub_82857E80(ctx, base);
}
REX_EXTERN(__imp__sub_828732E8);
REX_HOOK_RAW(sub_828732E8) {
  ++kk::photo::g_view_calls;
  __imp__sub_828732E8(ctx, base);
}
#endif
