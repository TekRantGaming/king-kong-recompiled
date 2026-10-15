#pragma once

#include <atomic>

namespace nr {

// Set before renderer workers start. Kept SDK-independent for the offline tests.
inline std::atomic<bool> g_diagnostics_enabled{false};
inline bool DiagnosticsEnabled() {
  return g_diagnostics_enabled.load(std::memory_order_relaxed);
}
inline void SetDiagnosticsEnabled(bool enabled) {
  g_diagnostics_enabled.store(enabled, std::memory_order_relaxed);
}

}  // namespace nr
