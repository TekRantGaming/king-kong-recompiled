// Developer-only: KK_DEV_LOAD_TRACE=1 logs the loading screen's handshake with
// the rest of the game. The loading screen thread (sub_8278E450, started by
// sub_8278E660) owns the D3D device while it draws; the block at 0x82D64080
// holds its state (+12), whether it owns the device (+16) and a request (+20:
// 1 release, 2 take back). sub_8278CF00 asks the device's worker thread to
// present a frame (sub_8278D128), which needs the device.
#if defined(KK_DEV_TOOLS)

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>

namespace {
bool Tracing() {
  static const bool on = std::getenv("KK_DEV_LOAD_TRACE") != nullptr;
  return on;
}

uint32_t Word(uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void Log(const char* what, const char* when, PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kBlock = 0x82D64080, kDevice = 0x82D62320;
  REXLOG_INFO("KK dev load: {} {} r3={:08X} r4={:08X} lr={:08X} | state {} owns {} request {} mode {} | device owner {} count {}",
              what, when, ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr), int32_t(Word(base, kBlock + 12)),
              int32_t(Word(base, kBlock + 16)), int32_t(Word(base, kBlock + 20)), int32_t(Word(base, kBlock + 8)),
              int32_t(Word(base, kDevice + 284)), int32_t(Word(base, kDevice + 288)));
}
}  // namespace

#define KK_LOAD_HOOK(addr, name)                    \
  REX_EXTERN(__imp__sub_##addr);                    \
  REX_HOOK_RAW(sub_##addr) {                        \
    if (Tracing()) Log(name, "in", ctx, base);      \
    __imp__sub_##addr(ctx, base);                   \
    if (Tracing()) Log(name, "out", ctx, base);     \
  }

// sub_8253DB80 (uni_go, the level load) is hooked in dev_chapter_jump.cpp (KK_DEV_JUMP_TRACE).
KK_LOAD_HOOK(8278E660, "start loading screen")
KK_LOAD_HOOK(8278D368, "stop loading screen")
KK_LOAD_HOOK(8278D270, "request release")
KK_LOAD_HOOK(8278D240, "request take back")
KK_LOAD_HOOK(8278D2F0, "state = 1")
KK_LOAD_HOOK(8278D220, "set mode")
KK_LOAD_HOOK(8278CF00, "post present (8278CF00)")
// The worker present, written out (same steps as the original: present, then state = 1 unless the state is 0)
// so the trace can see the store that can overwrite a stop request (kk-decomp docs/bugs/bug2-swamp-load-freeze.md,
// race b). KK_DEV_LOAD_RACE_MS=n waits n ms between the read and the store, to make the race happen on purpose.
REX_EXTERN(sub_8278D0C8);
REX_HOOK_RAW(sub_8278D128) {
  constexpr uint32_t kState = 0x82D64080 + 12;
  static const int race_ms = [] {
    const char* e = std::getenv("KK_DEV_LOAD_RACE_MS");
    return e ? std::atoi(e) : 0;
  }();
  if (Tracing()) Log("worker present (8278D128)", "in", ctx, base);
  const uint64_t lr = ctx.lr;
  ctx.lr = 0x8278D134;  // as the original's bl
  sub_8278D0C8(ctx, base);
  ctx.lr = lr;
  const uint32_t seen = Word(base, kState);
  if (seen != 0) {
    if (race_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(race_ms));
    const uint32_t now = Word(base, kState);
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + kState)).store(std::byteswap(uint32_t(1)));
    if (now == 3)
      REXLOG_WARN("KK dev load: LOST STOP: the worker present overwrote state 3 (stop) with 1 (read {} before); "
                  "the loading screen thread will not end and the stop waits forever", int32_t(seen));
  }
  if (Tracing()) Log("worker present (8278D128)", "out", ctx, base);
}

#endif
