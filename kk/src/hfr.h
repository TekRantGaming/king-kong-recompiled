// High frame rate fixes (kk_hfr_fixes): shared helpers.
//
// The game steps most of its systems with the frame time (seconds since the
// last frame, the float at 0x82C636E0, set by sub_8272CA20 from the tick count
// and clamped to 0.001-0.063 s), so they keep the right speed at any frame
// rate. A few assume a 1/30 s step. The fixes for those (hfr_*.cpp, listed in
// docs/high-frame-rate.md) are on when kk_hfr_fixes is set and the frame rate
// cap is not 30: at 30 FPS the game runs exactly as on the Xbox 360.

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>

namespace rex::memory {
class Memory;
}

REXCVAR_DECLARE(bool, kk_hfr_fixes);
REXCVAR_DECLARE(int32_t, kk_frame_rate);

namespace kk::hfr {

constexpr uint32_t kFrameTime = 0x82C636E0;  // seconds since the last frame (scaled by the game's time scale)

inline uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint16_t Load16(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint16_t(p[0] << 8 | p[1]);
}
inline void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
inline float LoadF(const uint8_t* base, uint32_t addr) {
  const uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void StoreF(uint8_t* base, uint32_t addr, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Store32(base, addr, v);
}

// The fixes apply: kk_hfr_fixes on and a frame rate other than 30 (0 = unlimited).
inline bool Active() { return REXCVAR_GET(kk_hfr_fixes) && REXCVAR_GET(kk_frame_rate) != 30; }

// One fix by name: Active(), and in dev builds not listed in KK_DEV_HFR_OFF
// ("particles,decay,..." or "all"), so each one can be measured on and off.
bool On(const char* name);

// Dev builds with KK_DEV_HFR_TRACE: count an event of a fix (logged once a
// second as "KK dev hfr <name>: n"). No-op otherwise.
void Count(const char* name);

// Once the runtime is up (OnPostSetup): what some fixes need from it.
void Setup(rex::memory::Memory* memory);

inline float FrameTime(const uint8_t* base) { return LoadF(base, kFrameTime); }

}  // namespace kk::hfr
