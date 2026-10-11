// High frame rate fixes: the switch (see hfr.h and docs/high-frame-rate.md).

#include "hfr.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace kk::hfr {

bool On(const char* name) {
  if (!Active()) return false;
#if defined(KK_DEV_TOOLS)
  // KK_DEV_HFR_OFF="particles,step_decay" (or "all") leaves those as the game does them.
  static const std::string off = [] {
    const char* v = std::getenv("KK_DEV_HFR_OFF");
    return std::string(",") + (v ? v : "") + ",";
  }();
  if (off.find(",all,") != std::string::npos) return false;
  if (off.find("," + std::string(name) + ",") != std::string::npos) return false;
#else
  (void)name;
#endif
  return true;
}

void SetupDtFloor(rex::memory::Memory* memory);

void Setup(rex::memory::Memory* memory) { SetupDtFloor(memory); }

}  // namespace kk::hfr
