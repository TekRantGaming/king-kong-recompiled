#pragma once

// Task 297: how long the renderer has spent compiling shaders on a thread the game waits on (a shader
// missing from the pack: DXC to DXIL or SPIR-V inside ShaderLibrary::Build, called from the game's
// draw hooks). King Kong reads it through nr_shader_compile_milliseconds (plugin_main.cpp,
// hooks/kk_native_ui.h) so its startup limits (the 60 s no-first-frame report, startup recovery's
// window) do not count that time. With a shader pack nothing is compiled and the value stays 0.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace nr {

inline uint64_t CompileClockNowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

struct CompileClock {
  // Finished compiles, summed.
  static inline std::atomic<uint64_t> finished_ms{0};
  // Start of the compile running now (the first of overlapping ones), 0 = none.
  static inline std::atomic<uint64_t> running_since{0};

  // Finished plus the running compile so far: grows while a compile runs (a long one counts as it goes).
  static uint64_t Milliseconds(uint64_t now = CompileClockNowMs()) {
    const uint64_t since = running_since.load(std::memory_order_relaxed);
    return finished_ms.load(std::memory_order_relaxed) + (since && now > since ? now - since : 0);
  }
};

// Around one compile the game waits for.
class CompileScope {
 public:
  CompileScope() : start_(CompileClockNowMs()) {
    uint64_t none = 0;
    owner_ = CompileClock::running_since.compare_exchange_strong(none, start_, std::memory_order_relaxed);
  }
  ~CompileScope() {
    // Only the scope that set running_since adds its time (overlapping compiles are counted once).
    if (!owner_) return;
    const uint64_t now = CompileClockNowMs();
    CompileClock::finished_ms.fetch_add(now > start_ ? now - start_ : 0, std::memory_order_relaxed);
    CompileClock::running_since.store(0, std::memory_order_relaxed);
  }
  CompileScope(const CompileScope&) = delete;
  CompileScope& operator=(const CompileScope&) = delete;

 private:
  uint64_t start_;
  bool owner_ = false;
};

}  // namespace nr
