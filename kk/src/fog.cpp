// Distance fog (launcher: Graphics > Distance fog, kk_fog).
//
// The haze over far scenery is one of the game's after effects: a full-screen
// pass that blends the fog color over the frame by the scene depth (the
// AfterEffects pixel shader's fog variants). sub_827648A8 runs it every frame
// (slot 10 of its class's vtable at 0x820A3E80, like motion blur's
// sub_82768DE8): it binds the depth texture, picks the fog variant and returns
// a value through r6 that the caller draws the pass with. With the setting off
// it isn't run, and the pass isn't drawn. Found by skipping each after effect
// in turn (dev_ae.cpp).

#include <rex/hook.h>
#include <rex/system/flags.h>

#include "settings.h"

REX_EXTERN(__imp__sub_827648A8);
REX_HOOK_RAW(sub_827648A8) {
  if (!REXCVAR_GET(kk_fog)) return;
  __imp__sub_827648A8(ctx, base);
}
