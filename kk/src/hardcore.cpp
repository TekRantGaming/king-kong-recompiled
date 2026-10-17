// Hardcore switches (Beta 7 built-in mod "Hardcore (no HUD, no healing)").
//
// kk_hide_hud (freecam.cpp) hides the game's HUD with the same mechanism as F10.
// kk_no_healing: NOT WIRED YET (task 200 did not find Jack's health/regen code; the setting exists so the
// built-in mod and saves keep working once the game-logic hook lands). Dev aids below for that search.

#include <atomic>
#include <bit>
#include <cstdio>
#include <vector>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(kk_no_healing, false, "KK",
                    "Hardcore: Jack's health does not regenerate and health pickups do nothing");

namespace kk::hardcore {
namespace {
uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return std::byteswap(v);
}
float LoadF(const uint8_t* base, uint32_t addr) { return std::bit_cast<float>(Load32(base, addr)); }
}  // namespace

#if defined(KK_DEV_TOOLS)
// KK_DEV_HEALTH_LOG=<file>: snapshots of the player's AI variables (the instance that reads the pad,
// H_exec_read_joy) and of the game block every quarter second; every 10 s writes the 4-byte fields that
// changed 1 to 16 times, as time:value lists (floats and ints), to find the health value.
std::atomic<uint32_t> g_jack_v{0};
struct Snap { double t; std::vector<uint8_t> v, g, fx; };
void DevLog(uint8_t* base) {
  static const char* file = std::getenv("KK_DEV_HEALTH_LOG");
  if (!file || !*file) return;
  static const auto t0 = std::chrono::steady_clock::now();
  static std::vector<Snap> snaps;
  static double next = 0, next_write = 10;
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (t < next) return;
  next = t + 0.25;
  const uint32_t v = g_jack_v.load(), g = Load32(base, 0x82CC97C0);
  if (!v || !g || snaps.size() > 2000) return;
  const uint32_t fx = Load32(base, 0x82CB95AC);
  Snap s{t, std::vector<uint8_t>(base + v, base + v + 0x4000), std::vector<uint8_t>(base + g, base + g + 0x6000),
         fx ? std::vector<uint8_t>(base + fx, base + fx + 0x2000) : std::vector<uint8_t>(0x2000)};
  snaps.push_back(std::move(s));
  if (t < next_write) return;
  next_write = t + 10;
  FILE* out = std::fopen(file, "wb");
  if (!out) return;
  for (const Snap& sn : snaps) {
    std::fwrite(&sn.t, sizeof(double), 1, out);
    std::fwrite(sn.v.data(), 1, sn.v.size(), out);
    std::fwrite(sn.g.data(), 1, sn.g.size(), out);
    std::fwrite(sn.fx.data(), 1, sn.fx.size(), out);
  }
  std::fclose(out);
}
#endif

#if defined(KK_DEV_TOOLS)
// KK_DEV_HEALTH_SET="<seconds>:<G offset>:<float>[,...]": writes a float into the game block at that time
// (investigation of the injury state); KK_DEV_HEALTH_LOG also logs G+7088..7104 when they change.
void DevSet(uint8_t* base) {
  static const char* spec = std::getenv("KK_DEV_HEALTH_SET");
  static const auto t0 = std::chrono::steady_clock::now();
  static size_t done = 0;
  const uint32_t g = Load32(base, 0x82CC97C0);
  if (!g) return;
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (spec && *spec) {
    size_t i = 0;
    for (const char* p = spec; *p; ++i) {
      double at = 0; unsigned off = 0; float val = 0; int n = 0;
      if (std::sscanf(p, "%lf:%u:%f%n", &at, &off, &val, &n) < 3) break;
      if (i >= done && t >= at) {
        const uint32_t w = std::byteswap(std::bit_cast<uint32_t>(val));
        std::memcpy(base + g + off, &w, 4);
        done = i + 1;
        REXLOG_INFO("KK health dev: G+{} = {} at {:.1f} s", off, val, t);
      }
      p += n;
      if (*p == ',') ++p;
    }
  }
  static uint32_t last[5] = {1, 1, 1, 1, 1};
  if (!std::getenv("KK_DEV_HEALTH_LOG")) return;
  for (int k = 0; k < 5; ++k) {
    const uint32_t w = Load32(base, g + 7088 + 4 * k);
    if (w != last[k]) {
      last[k] = w;
      REXLOG_INFO("KK health dev: {:.2f} s G+{} = {}", t, 7088 + 4 * k, std::bit_cast<float>(w));
    }
  }
}
#endif

void Frame(uint8_t* base) {
#if defined(KK_DEV_TOOLS)
  DevSet(base);
  DevLog(base);
#endif
  (void)base;
}

}  // namespace kk::hardcore

#if defined(KK_DEV_TOOLS)
// H_exec_read_joy: the human AI instance that reads the pad (Jack). Its AI variables: *(*(0x82CCBF64)+0x40).
REX_EXTERN(__imp__sub_82687730);
REX_HOOK_RAW(sub_82687730) {
  const uint32_t vp = kk::hardcore::Load32(base, 0x82CCBF64);
  if (vp) kk::hardcore::g_jack_v = kk::hardcore::Load32(base, vp + 0x40);
  __imp__sub_82687730(ctx, base);
}
#endif
