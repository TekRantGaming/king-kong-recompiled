// Developer-only: KK_DEV_VENTURE_TRACE=1 logs the Venture opening's camera
// tutorial (Chaloupe_CineD1, sub_82432CD8: "Right stick to aim", then the
// vertical sighting choice) twice a second: its gates (+12, +592, +596,
// +600, +660 of the script's variables) and the right stick's y, to see why
// the choice is or is not taken.
#if defined(KK_DEV_TOOLS)

#include <chrono>
#include <cstdlib>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"

REX_EXTERN(__imp__sub_82432CD8);
REX_HOOK_RAW(sub_82432CD8) {
  using namespace kk::hfr;
  static const bool on = std::getenv("KK_DEV_VENTURE_TRACE") != nullptr;
  if (!on) {
    __imp__sub_82432CD8(ctx, base);
    return;
  }
  const uint32_t ai = Load32(base, 0x82CCBF64);
  const uint32_t v = ai ? Load32(base, ai + 64) : 0;
  auto gates = [&] {
    return v ? fmt::format("12={} 592={} 596={} 600={} 660={}", Load32(base, v + 12), Load32(base, v + 592),
                           Load32(base, v + 596), Load32(base, v + 600), Load32(base, v + 660))
             : std::string("no vars");
  };
  const std::string before = gates();
  __imp__sub_82432CD8(ctx, base);
  static auto last = std::chrono::steady_clock::now();
  static std::string last_after;
  const std::string after = gates();
  // Stick block (sub_8272C610's source): 0x82C638D8, right stick at +16/+20.
  const float sy = LoadF(base, 0x82C638D8 + 20);
  if (after != last_after || std::chrono::steady_clock::now() - last > std::chrono::milliseconds(500)) {
    last = std::chrono::steady_clock::now();
    last_after = after;
    REXLOG_INFO("KK dev venture: before [{}] after [{}] stick y {:.2f} dt {:.4f} ret {}", before, after, sy,
                FrameTime(base), int32_t(ctx.r3.u32));
  }
}

#endif
