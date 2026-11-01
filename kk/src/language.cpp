// Game language. The title asks XGetLanguage which language to use (and
// switches its text on the answer in sub_8272AE08), but the runtime's
// XGetLanguage always answers English. Answer with the launcher's Language
// setting (user_language) instead, limited to the languages on the disc.
//
// The disc has five more text languages that an Xbox 360 has no number for:
// Danish, Dutch, Finnish, Norwegian and Swedish. The game reaches its own
// language numbers through sub_827034A0 (0 French, 1 English, 5 German, and so
// on), so those are asked for there: user_language 100 + the game's number
// (launcher.cpp, kLanguages). Their speech stays English, which is what the
// game does for a text language with no voices (lang_set_text).

#include <cstdint>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

#include "hfr.h"

namespace {

constexpr uint32_t kFirstOwn = 100, kOwnCount = 29;
constexpr uint32_t kPresent = 0x828D1708;  // one word per game language: not 0 when the disc has its text

// The game's number for a language the console cannot name, -1 for the others or one the disc lacks.
int OwnLanguage(const uint8_t* base) {
  const uint32_t lang = REXCVAR_GET(user_language);
  if (lang < kFirstOwn || lang >= kFirstOwn + kOwnCount) return -1;
  const uint32_t own = lang - kFirstOwn;
  return kk::hfr::Load32(base, kPresent + own * 4) != 0 ? int(own) : -1;
}

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

REX_EXTERN(__imp__sub_827034A0);
REX_HOOK_RAW(sub_827034A0) {
  const int own = OwnLanguage(base);
  static bool logged = false;
  if (!logged) {
    logged = true;
    std::string present;
    for (uint32_t i = 0; i < kOwnCount; ++i)
      if (kk::hfr::Load32(base, kPresent + i * 4) != 0) present += " " + std::to_string(i);
    REXLOG_INFO("KK: game text languages on the disc:{}; this run uses {}", present, own);
  }
  if (own < 0) {
    __imp__sub_827034A0(ctx, base);
    return;
  }
  ctx.r3.u64 = uint64_t(own);
}

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
