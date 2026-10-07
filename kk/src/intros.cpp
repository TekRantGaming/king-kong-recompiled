// Startup logo movies. Three flags at 0x828E3860 (+4 Ubisoft, +8 Universal,
// +12 WingNut) start set in the game's data; while the game loads at boot,
// sub_8272EED8 runs every frame and plays each flagged movie in turn, clearing
// its flag. With kk_skip_intros on, the flags are cleared first so none of
// them plays. Story movies (Intro.wmv and the chapter cutscenes) are played
// elsewhere and are not affected.

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

#include "settings.h"

REX_EXTERN(__imp__sub_8272EED8);

namespace {
constexpr uint32_t kLogoFlags = 0x828E3860;
}  // namespace

REX_HOOK_RAW(sub_8272EED8) {
  if (REXCVAR_GET(kk_skip_intros)) {
    bool any = false;
    for (uint32_t off : {4u, 8u, 12u}) {
      uint32_t v;
      std::memcpy(&v, base + kLogoFlags + off, 4);
      if (v) {
        any = true;
        v = 0;  // the same in either byte order
        std::memcpy(base + kLogoFlags + off, &v, 4);
      }
    }
    if (any) REXLOG_INFO("KK: skipping the startup logo movies");
  }
  __imp__sub_8272EED8(ctx, base);
}
