// Reproducible spatial-AA captures in one developer run. Never built into releases.
#if defined(KK_DEV_TOOLS)
#include <cstdlib>
#include <rex/logging.h>
#include <rex/system/flags.h>
namespace kk {
void DevAaTick(double seconds) {
  static const bool enabled = std::getenv("KK_DEV_AA_SEQUENCE") != nullptr;
  static int last = -1;
  if (!enabled || seconds < 0) return;
  const int stage = seconds < 24 ? 0 : seconds < 32 ? 1 : 2;
  if (stage == last) return;
  const char* value = stage == 0 ? "none" : stage == 1 ? "fxaa" : "smaa";
  rex::cvar::SetFlagByName("swap_post_effect", value);
  REXLOG_INFO("KK dev AA: {} at {:.3f} s", value, seconds);
  last = stage;
}
}
#endif
