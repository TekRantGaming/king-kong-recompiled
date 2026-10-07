// Motion blur (launcher: Graphics > Motion blur, kk_motion_blur).
//
// The game's motion blur is one of its after effects (sub_82768DE8, a virtual
// method of the effect's class): it blends the previous frame over the new one
// with a strength from the effect's parameters (this + (index + 5) * 4, scaled
// by frame time into g_fMotionBlurAlpha), then keeps the new frame for next
// time. Kong's sequences and some transitions turn it up. When the strength is
// 0 the game skips the blend and drops the kept frame, so with the setting off
// the effect runs with its strength read as 0 and the game's value is put back
// afterwards (it fades the strength itself).

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/system/flags.h>

#include "settings.h"

REX_EXTERN(__imp__sub_82768DE8);
REX_HOOK_RAW(sub_82768DE8) {
  if (REXCVAR_GET(kk_motion_blur)) {
    __imp__sub_82768DE8(ctx, base);
    return;
  }
  const uint32_t strength = ctx.r3.u32 + (ctx.r4.u32 + 5) * 4;
  uint32_t saved;
  std::memcpy(&saved, base + strength, 4);
  const uint32_t zero = 0;
  std::memcpy(base + strength, &zero, 4);
  __imp__sub_82768DE8(ctx, base);
  std::memcpy(base + strength, &saved, 4);
}
