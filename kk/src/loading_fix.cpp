// Loading screen hang.
//
// When a load starts, sub_8253DB80 starts the loading screen thread
// (sub_8278E660 -> sub_8278E450), which takes the D3D device (the owner lock at
// 0x82D62320 + 284) as soon as it runs and keeps it until the load is over. In
// the same frame the device's worker thread presents a frame (sub_8278D128 ->
// sub_8278D0C8), which needs the device too, and the main thread waits for that
// present before it goes on with the load and stops the loading screen. On a
// console the worker always gets the device first: it presents, lets go, and the
// loading screen takes over. Where threads start faster (Linux), the new
// loading screen thread can get there first; the worker then waits for the
// device forever, the main thread waits for the worker, and the game sits on
// the black LOADING screen.
//
// Fix: while a loading screen is up, it draws every frame itself, so the
// worker skips its present (the rest of sub_8278D0C8 still happens).

#include <cstdint>

#include <rex/hook.h>

namespace kk {
namespace {

constexpr uint32_t kDevice = 0x82D62320;
constexpr uint32_t kLoadingScreen = 0x82D64080;  // +0 thread handle, +12 state (2 starting, 1 shown, 3 stopping)

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

bool LoadingScreenUp(const uint8_t* base) {
  const uint32_t state = Load32(base, kLoadingScreen + 12);
  return Load32(base, kLoadingScreen) != 0 && (state == 1 || state == 2);
}

}  // namespace
}  // namespace kk

REX_EXTERN(__imp__sub_8278D0C8);
REX_HOOK_RAW(sub_8278D0C8) {
  if (kk::LoadingScreenUp(base)) {
    kk::Store32(base, kk::kDevice + 712, 0);  // what the present does last
    return;
  }
  __imp__sub_8278D0C8(ctx, base);
}
