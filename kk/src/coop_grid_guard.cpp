// C133: the saved 28f hang is a raptor grid walk with NaN coordinates, not
// an established driver stall. Its two loop limits became INT_MAX. Refuse
// an invalid line before conversion; the native return value 0 means blocked.
#include "coop_grid_guard.h"
#include "hfr.h"
#include <chrono>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, kk_coop_enabled);
REX_EXTERN(__imp__sub_82556890);

REX_HOOK_RAW(sub_82556890) {
  const kk::coop::grid_guard::Line line{ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.f1.f64};
  if (!kk::coop::grid_guard::Refuse(REXCVAR_GET(kk_coop_enabled), line)) {
    __imp__sub_82556890(ctx, base);
    return;
  }

  // Preserve the native helper's initial scratch flag clear. Do not read an
  // actor, AI or world pointer to reject an invalid query.
  kk::hfr::Store32(base, 0x82CCBFC0, 0);
  static kk::coop::grid_guard::Reports reports;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (reports.Note(now)) {
    const auto p = line.Points();
    // These two globals hold identity only. No diagnostic follows the pointers.
    REXLOG_WARN("KK co op grid query: invalid line refused actor={:08X} ai={:08X} caller={:08X} "
        "from=({},{},{}) to=({},{},{}) packed={:016X}/{:016X}/{:016X}/{:016X} "
        "shorten={} mask={:08X} width={} refused={}",
        kk::hfr::Load32(base, 0x82CCBF6C), kk::hfr::Load32(base, 0x82CCBF64), uint32_t(ctx.lr),
        p[0], p[1], p[2], p[3], p[4], p[5], line.from_xy, line.from_z, line.to_xy, line.to_z,
        line.shorten, ctx.r9.u32, ctx.r10.s32, reports.refused);
  }
  ctx.r3.u64 = 0;
}
