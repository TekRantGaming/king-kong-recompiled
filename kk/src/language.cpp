// Game language. The title asks XGetLanguage which language to use (and
// switches its text on the answer in sub_8272AE08), but the runtime's
// XGetLanguage always answers English. Answer with the launcher's Language
// setting (user_language) instead, limited to the languages on the disc.

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

namespace {

// XLanguage values the disc includes: English, German, French, Spanish, Italian.
uint32_t GameLanguage() {
  const uint32_t lang = REXCVAR_GET(user_language);
  switch (lang) {
    case 1:
    case 3:
    case 4:
    case 5:
    case 6:
      return lang;
    default:
      return 1;
  }
}

}  // namespace

REX_HOOK_RAW(__imp__XGetLanguage) {
  (void)base;
  const uint32_t lang = GameLanguage();
  static bool logged = false;
  if (!logged) {
    REXLOG_INFO("KK: game language {}", lang);
    logged = true;
  }
  ctx.r3.u64 = lang;
}
