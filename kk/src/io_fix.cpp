// Synchronous completion for overlapped ReadFile.
//
// sub_82106DE0 is the title's kernel32-style ReadFile. The Jade sound streamer
// (sub_8272F200) reads music and voice lines with an OVERLAPPED that lives on
// its own stack and takes the byte count only from lpNumberOfBytesRead. On a
// real console these reads complete inside NtReadFile, so ReadFile returns TRUE
// and fills in the count. The runtime finishes the read immediately too but
// reports STATUS_PENDING, so ReadFile returns FALSE, the count stays 0 and the
// stream never decodes anything: no music, no voice lines, and scripted scenes
// that wait for a line to end (the "VENTURE" opening) never move on.
//
// Fix: when ReadFile reports pending but the OVERLAPPED already holds a
// successful status, finish it the way the console does.

#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>

namespace kk {
namespace {

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

// The XMV video player (0x82300000-0x82320000) already handles pending reads.
bool IsVideoPlayer(uint32_t caller) { return caller >= 0x82300000 && caller < 0x82320000; }

}  // namespace
}  // namespace kk

REX_EXTERN(__imp__sub_82106DE0);
REX_HOOK_RAW(sub_82106DE0) {
  const uint32_t bytes_read = ctx.r6.u32;
  const uint32_t overlapped = ctx.r7.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
#if defined(KK_DEV_TOOLS)
  // Developer aid: KK_DEV_LOG_READS=1 logs overlapped reads (the sound
  // streamer's), to find which part of Sound_Common.bf is playing.
  static const bool log_reads = std::getenv("KK_DEV_LOG_READS") != nullptr;
  if (log_reads && overlapped)
    REXLOG_INFO("KK dev: read h={:08X} offset={:#x} size={:#x} lr={:08X}", ctx.r3.u32,
                kk::Load32(base, overlapped + 8), ctx.r5.u32, caller);
#endif
  __imp__sub_82106DE0(ctx, base);
  if (ctx.r3.u32 != 0 || !overlapped || kk::IsVideoPlayer(caller)) return;
  // OVERLAPPED: Internal (status), InternalHigh (bytes transferred), ...
  if (kk::Load32(base, overlapped) != 0) return;  // a real error, or truly pending
  if (bytes_read) kk::Store32(base, bytes_read, kk::Load32(base, overlapped + 4));
  ctx.r3.u64 = 1;
}
