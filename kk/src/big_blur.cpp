// Screen blur (launcher: Graphics > Screen blur, kk_big_blur).
//
// Some levels (Necropolis, Brontosaurus) have the game's "BigBlur" after
// effect on: a full-screen pass that averages four slightly offset samples of
// the frame (the AfterEffects pixel shader's BigBlur variant). The offsets are
// sized for the console's 1280x720, so at any higher render resolution the
// picture is blurred back to about 720p sharpness, which players reported as
// those levels not using the higher resolution (#32). sub_82762A38 runs it
// every frame (slot 10 of its class's vtable, like motion blur's sub_82768DE8
// and the fog's sub_827648A8); with the setting off it isn't run, and the pass
// isn't drawn. Found by skipping each after effect in turn (dev_ae.cpp) and
// identified from the shader key of the draw it adds.

#include <rex/hook.h>
#include <rex/system/flags.h>

#include "settings.h"

REX_EXTERN(__imp__sub_82762A38);
REX_HOOK_RAW(sub_82762A38) {
  if (!REXCVAR_GET(kk_big_blur)) return;
  __imp__sub_82762A38(ctx, base);
}
