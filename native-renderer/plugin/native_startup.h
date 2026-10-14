#pragma once
#include <atomic>

namespace nr {
using StartupFailureHandler = int (*)(const char*);
inline std::atomic<StartupFailureHandler> startup_failure_handler{nullptr};
inline bool ReportStartupFailure(const char* why) {
  const auto handler = startup_failure_handler.load(std::memory_order_acquire);
  return handler && handler(why);
}
// The game's first frame reached the screen: startup recovery stops watching (kk/src/startup_recovery.cpp).
using StartupPresentedHandler = void (*)();
inline std::atomic<StartupPresentedHandler> startup_presented_handler{nullptr};
inline void ReportStartupPresented() {
  if (const auto handler = startup_presented_handler.load(std::memory_order_acquire)) handler();
}
}  // namespace nr
