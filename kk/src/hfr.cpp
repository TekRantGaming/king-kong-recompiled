// High frame rate fixes: the switch (see hfr.h and docs/high-frame-rate.md).

#include "hfr.h"

#include <rex/logging.h>

#include <chrono>
#include <cstdlib>
#include <map>
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

void Count(const char* name) {
#if defined(KK_DEV_TOOLS)
  static const bool trace = std::getenv("KK_DEV_HFR_TRACE") != nullptr;
  if (!trace) return;
  static std::map<std::string, int> counts;
  static auto start = std::chrono::steady_clock::now();
  ++counts[name];
  if (std::chrono::steady_clock::now() - start < std::chrono::seconds(1)) return;
  start = std::chrono::steady_clock::now();
  for (auto& [n, c] : counts) {
    if (c) REXLOG_INFO("KK dev hfr {}: {}", n, c);
    c = 0;
  }
#else
  (void)name;
#endif
}

void SetupDtFloor(rex::memory::Memory* memory);

void Setup(rex::memory::Memory* memory) { SetupDtFloor(memory); }

}  // namespace kk::hfr
