#pragma once

// Task 297: the startup limits (crash_dump's 60 s no-first-frame report, startup recovery's 120 s window)
// leave out the time the renderer spent compiling shaders the game waited for (no shader pack: DXC on the
// game thread, minutes on a first start; nr::kk::ShaderCompileMilliseconds). With a pack nothing is
// compiled and the limits are as before. A hard cap still ends the wait if compiling never stops.
// Pure logic (kk/tests/startup_clock_tests.cpp).

#include <cstdint>

namespace kk::startup_clock {

// Ten minutes of wall time end the wait whatever was compiled (a compile that never returns).
constexpr uint64_t kCapMs = 600000;

struct Window {
  uint64_t started = 0;          // wall clock (ms) when the window began; 0 = not running
  uint64_t compile_at_start = 0; // the compile clock then
};

// Wall time since the start, less the compile time within it.
constexpr uint64_t CountedMs(const Window& w, uint64_t now, uint64_t compile_now) {
  const uint64_t wall = now > w.started ? now - w.started : 0;
  const uint64_t compiled = compile_now > w.compile_at_start ? compile_now - w.compile_at_start : 0;
  return wall > compiled ? wall - compiled : 0;
}

constexpr uint64_t CompiledMs(const Window& w, uint64_t compile_now) {
  return compile_now > w.compile_at_start ? compile_now - w.compile_at_start : 0;
}

// The limit is reached: counted time, or the cap of wall time.
constexpr bool Reached(const Window& w, uint64_t now, uint64_t compile_now, uint64_t limit_ms,
                       uint64_t cap_ms = kCapMs) {
  if (!w.started) return false;
  const uint64_t wall = now > w.started ? now - w.started : 0;
  return CountedMs(w, now, compile_now) >= limit_ms || wall >= cap_ms;
}

// Wall time alone would have reached the limit, but compiling held it off: the moment to say so (once).
constexpr bool ExtendedByCompile(const Window& w, uint64_t now, uint64_t compile_now, uint64_t limit_ms) {
  if (!w.started) return false;
  const uint64_t wall = now > w.started ? now - w.started : 0;
  return wall >= limit_ms && CountedMs(w, now, compile_now) < limit_ms;
}

// A paused PC (sleep): the window starts again from now, with the compile clock of now.
constexpr Window Restarted(uint64_t now, uint64_t compile_now) { return Window{now, compile_now}; }

}  // namespace kk::startup_clock
