// The CPU ripple grid used by vswater advances with a hardcoded 1/30 step
// (8281F4BC..C8), not FrameTime. 8281E098 uses that step for both wave
// integration and its elapsed-time splash accumulator. Velocity and height
// damping are also per call, with distance/low-amplitude branches selecting
// different factors. Keep the original writer and its selected factors, using
// the game's delta and equivalent retention for the elapsed interval.
// Configured 30 FPS and disabled fixes execute the original verbatim.
#include <cmath>
#include <xmmintrin.h>
#include <rex/hook.h>
#include "hfr.h"


REX_EXTERN(__imp__sub_8281E098);
REX_HOOK_RAW(sub_8281E098) {
  using namespace kk::hfr;
  constexpr uint32_t step_address = 0x8292FC28;
  const uint32_t system = ctx.r3.u32, tile = ctx.r4.u32;
  if (!On("water")) {
    __imp__sub_8281E098(ctx, base);
  } else {
    const float dt = FrameTime(base);
    // The caller owns these structures, and the original immediately reads
    // them too. No retained pointer/state, AI call, or shared frame delta.
    const uint32_t step_bits = Load32(base, step_address);
    // Preserve the original early sleep predicate before normalizing kicks.
    // A tiny authored splash must not wake a tile merely because dt is small.
    bool sleeping = false;
    if (Load32(base, tile + 316) & 2) {
      const unsigned fp = _mm_getcsr();
      _mm_setcsr(fp | 0x1F80);
      const float strength = float(int32_t(Load32(base, system + 36))) * LoadF(base, system + 32);
      sleeping = std::fabs(strength) < LoadF(base, 0x820B0E48) && !base[tile + 292];
      _mm_setcsr(fp);
    }
    if (step_bits != 0x3D08882F || !std::isfinite(dt) || dt <= 0 || dt > .063f || sleeping) {
      __imp__sub_8281E098(ctx, base);
    } else {
      // Random splashes (system+32) and queued contact splashes (+24)
      // inject an impulse into the acceleration scratch grid. The original
      // multiplies that scratch by its fixed step. Preserve each kick's
      // strength when the integration interval changes; wave propagation
      // itself still uses dt. Restore both authored words after the call.
      const uint32_t random_kick_bits = Load32(base, system + 32);
      const uint32_t contact_kick_bits = Load32(base, system + 24);
      const unsigned kick_fp = _mm_getcsr();
      _mm_setcsr(kick_fp | 0x1F80);
      const double kick_scale = double(LoadF(base, step_address)) / dt;
      const float random_kick = float(LoadF(base, system + 32) * kick_scale);
      const float contact_kick = float(LoadF(base, system + 24) * kick_scale);
      _mm_setcsr(kick_fp);
      StoreF(base, system + 32, random_kick);
      StoreF(base, system + 24, contact_kick);
      StoreF(base, step_address, dt);
      __imp__sub_8281E098(ctx, base);
      Store32(base, step_address, step_bits);
      Store32(base, system + 32, random_kick_bits);
      Store32(base, system + 24, contact_kick_bits);
      // In the integrated branch, f9 is the actual velocity retention and
      // f13 the actual height retention (8281E7B4..E850, E8CC..E8E0).
      // They are volatile results, untouched by the original epilogue.
      // Sleeping/cleared tiles retain bit 2; culled tiles retain bit 1.
      const uint32_t width = base[tile + 304], height = base[tile + 305];
      const uint32_t owner = Load32(base, system + 4);
      if (!(Load32(base, tile + 316) & 3) && (Load32(base, owner + 4) & 2) &&
          width > 2 && height > 2) {
        const unsigned fp = _mm_getcsr();
        // Host-only bookkeeping must not raise exceptions or leave FP flags
        // in the original guest result.
        _mm_setcsr(fp | 0x1F80);
        const double alpha = 30.0 * dt;
        const auto correction = [alpha](double retention) {
          return std::isfinite(retention) && retention > 0 && retention < 1
              ? std::pow(retention, alpha) / retention : 1.0;
        };
        const float velocity_scale = float(correction(ctx.f9.f64));
        const float height_scale = float(correction(ctx.f13.f64));
        const uint32_t heights = Load32(base, tile + 312);
        const uint32_t velocities = Load32(base, tile + 308);
        for (uint32_t y = 1; y + 1 < height; ++y) {
          for (uint32_t x = 1; x + 1 < width; ++x) {
            const uint32_t offset = 4 * (y * width + x);
            if (height_scale != 1)
              StoreF(base, heights + offset, LoadF(base, heights + offset) * height_scale);
            if (velocity_scale != 1)
              StoreF(base, velocities + offset, LoadF(base, velocities + offset) * velocity_scale);
          }
        }
        _mm_setcsr(fp);
      }
    }
  }
}
