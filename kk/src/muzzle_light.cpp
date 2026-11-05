// Muzzle flashes that cast light (native-renderer/backend/muzzle_light.h): every gun's flame goes through
// H_exec_GFX_Flamme, which sets the flame's position as effect value 13200 once. That position, turned into the
// camera's view space, is what the renderer needs. Script arguments are popped inside the function, so the position
// is taken from that one effect call.

#include <cmath>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"

REX_EXTERN(__imp__sub_826916C8);  // H_exec_GFX_Flamme
REX_EXTERN(__imp__sub_826FC178);  // an effect's vector value: object, effect, key, vector

namespace {

constexpr uint32_t kEffectPosition = 13200;
constexpr uint32_t kWorld = 0x828E8388;
constexpr uint32_t kPointerTop = 0x90000000;
constexpr float kReach = 30.0f;  // a flame farther from the camera than this lights nothing near it worth a pass

thread_local bool g_in_flame = false;

using FlashFn = void (*)(float, float, float);
FlashFn FindFlash() {
#if defined(_WIN32)
  for (const char* module : {"rexgpu-nativerd.dll", "rexgpu-native.dll", "rexgpu-natived.dll"})
    if (HMODULE m = GetModuleHandleA(module))
      if (FARPROC f = GetProcAddress(m, "nr_muzzle_flash")) return reinterpret_cast<FlashFn>(f);
#else
  for (const char* module : {"librexgpu-nativerd.so", "librexgpu-native.so", "librexgpu-natived.so"}) {
    if (void* m = dlopen(module, RTLD_NOW | RTLD_NOLOAD)) {
      void* f = dlsym(m, "nr_muzzle_flash");
      dlclose(m);  // still loaded by the runtime
      if (f) return reinterpret_cast<FlashFn>(f);
    }
  }
#endif
  return nullptr;
}

bool GuestPointer(uint32_t p) { return p >= 0x40000000 && p < kPointerTop; }

struct Vec {
  float x, y, z;
};
float Dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec ReadVec(const uint8_t* base, uint32_t at) {
  return {kk::hfr::LoadF(base, at), kk::hfr::LoadF(base, at + 4), kk::hfr::LoadF(base, at + 8)};
}
Vec Unit(Vec v) {
  const float l = std::sqrt(Dot(v, v));
  return l > 1e-6f ? Vec{v.x / l, v.y / l, v.z / l} : Vec{0, 0, 0};
}

void Flame(const uint8_t* base, uint32_t position) {
  static const FlashFn flash = FindFlash();
  if (!flash || !GuestPointer(position)) return;
  // The camera: the world's +112, its +160. Its matrix: rows 0 left, 1 back, 2 up, then its position.
  const uint32_t world = kk::hfr::Load32(base, kWorld);
  const uint32_t view = GuestPointer(world) ? kk::hfr::Load32(base, world + 112) : 0;
  const uint32_t camera = GuestPointer(view) ? kk::hfr::Load32(base, view + 160) : 0;
  const uint32_t matrix = GuestPointer(camera) ? kk::hfr::Load32(base, camera + 12) : 0;
  if (!GuestPointer(matrix)) return;
  const Vec left = Unit(ReadVec(base, matrix)), back = Unit(ReadVec(base, matrix + 16)), up = Unit(ReadVec(base, matrix + 32));
  const Vec eye = ReadVec(base, matrix + 48), p = ReadVec(base, position);
  const Vec d{p.x - eye.x, p.y - eye.y, p.z - eye.z};
  if (!std::isfinite(Dot(d, d)) || Dot(d, d) > kReach * kReach) return;
  const float x = -Dot(d, left), y = Dot(d, up), z = -Dot(d, back);
#if defined(KK_DEV_TOOLS)
  static int lines = 0;
  if (lines < 8) {
    ++lines;
    REXLOG_INFO("KK muzzle: flame at view {:.2f} {:.2f} {:.2f}", x, y, z);
  }
#endif
  flash(x, y, z);
}

}  // namespace

REX_HOOK_RAW(sub_826916C8) {
  g_in_flame = true;
  __imp__sub_826916C8(ctx, base);
  g_in_flame = false;
}

REX_HOOK_RAW(sub_826FC178) {
  if (g_in_flame && ctx.r5.u32 == kEffectPosition) Flame(base, ctx.r6.u32);
  __imp__sub_826FC178(ctx, base);
}
