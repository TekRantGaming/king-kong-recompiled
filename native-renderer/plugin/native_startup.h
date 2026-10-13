#pragma once
#include <atomic>

namespace nr {
using StartupFailureHandler = int (*)(const char*);
inline std::atomic<StartupFailureHandler> startup_failure_handler{nullptr};
inline bool ReportStartupFailure(const char* why) {
  const auto handler = startup_failure_handler.load(std::memory_order_acquire);
  return handler && handler(why);
}
}  // namespace nr
