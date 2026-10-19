// Task 297: the startup limits leave out shader compile time (startup_clock.h), and the renderer's compile
// clock (native-renderer/backend/compile_clock.h) counts overlapping compiles once and a running one as it goes.
// Usage: kk-startup-clock-tests
#include "startup_clock.h"
#include "backend/compile_clock.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace kk::startup_clock;

namespace {
int failures = 0;
void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
constexpr uint64_t s = 1000;
}  // namespace

// Compile-time checks of the same rules.
static_assert(CountedMs(Window{1000, 0}, 61000, 0) == 60000);
static_assert(!Reached(Window{}, 1000000, 0, 60000));

int main() {
  const uint64_t t0 = 5000000;
  // A shader pack: nothing compiled, the limits as before (60 s report, 120 s window).
  {
    const Window w{t0, 7};
    Check(!Reached(w, t0 + 59 * s, 7, 60 * s), "pack: 59 s is not 60");
    Check(Reached(w, t0 + 60 * s, 7, 60 * s), "pack: 60 s reports");
    Check(!ExtendedByCompile(w, t0 + 60 * s, 7, 60 * s), "pack: never extended");
    Check(Reached(w, t0 + 120 * s, 7, 120 * s), "pack: recovery window ends at 120 s");
    Check(CountedMs(w, t0 + 30 * s, 7) == 30 * s, "pack: counted = wall");
  }
  // No pack: 4 min 11 s of DXC on the game thread (REPORT-286 run l1dx1), then the first frame 1 s later.
  {
    const uint64_t compile0 = 250;  // the clock before the game started (launcher prewarm, an earlier start)
    const Window w{t0, compile0};
    const uint64_t compiled = 251 * s;
    Check(!Reached(w, t0 + 60 * s, compile0 + 59 * s, 60 * s), "no pack: 60 s wall, 59 s compiling: no report");
    Check(ExtendedByCompile(w, t0 + 60 * s, compile0 + 59 * s, 60 * s), "no pack: extension noted");
    Check(!Reached(w, t0 + 252 * s, compile0 + compiled, 60 * s), "no pack: 252 s wall, 251 s compiling: no report");
    Check(!Reached(w, t0 + 252 * s, compile0 + compiled, 120 * s), "no pack: recovery still watching");
    Check(CountedMs(w, t0 + 252 * s, compile0 + compiled) == 1 * s, "no pack: counted 1 s");
    // Stuck after the compile (a real hang): the report comes 60 s after the compiling stopped.
    Check(!Reached(w, t0 + 310 * s, compile0 + compiled, 60 * s), "stuck after compile: 59 s counted");
    Check(Reached(w, t0 + 311 * s, compile0 + compiled, 60 * s), "stuck after compile: 60 s counted reports");
    Check(!ExtendedByCompile(w, t0 + 311 * s, compile0 + compiled, 60 * s), "stuck after compile: no extension line");
  }
  // A compile that never returns: the ten-minute cap still ends the wait.
  {
    const Window w{t0, 0};
    Check(!Reached(w, t0 + kCapMs - 1, kCapMs - 1, 60 * s), "endless compile: before the cap");
    Check(Reached(w, t0 + kCapMs, kCapMs, 60 * s), "endless compile: the cap reports");
    Check(Reached(w, t0 + kCapMs, kCapMs, 120 * s), "endless compile: the cap ends the recovery window");
  }
  // Clock oddities: a compile clock lower than its start (plugin reloaded) or wall before start count as 0.
  {
    const Window w{t0, 9000};
    Check(CountedMs(w, t0 + 10 * s, 100) == 10 * s, "compile clock went back: wall counted");
    Check(CountedMs(w, t0 - 5, 9000) == 0, "now before start: 0");
    Check(CompiledMs(w, 100) == 0, "compile clock went back: 0 compiled");
    const Window r = Restarted(t0 + 100 * s, 40 * s);
    Check(r.started == t0 + 100 * s && r.compile_at_start == 40 * s, "sleep restarts the window");
  }
  // The renderer's compile clock.
  {
    using nr::CompileClock;
    using nr::CompileScope;
    const uint64_t before = CompileClock::Milliseconds();
    {
      CompileScope a;
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      Check(CompileClock::Milliseconds() >= before + 50, "running compile counts as it goes");
      {
        CompileScope overlapping;  // e.g. another thread's compile at the same time: counted once
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }
    }
    const uint64_t after = CompileClock::Milliseconds();
    Check(after >= before + 85 && after < before + 400, "finished compiles summed once: " + std::to_string(after - before));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Check(CompileClock::Milliseconds() == after, "nothing running: the clock stands still");
  }
  if (failures) return 1;
  std::printf("startup clock: all checks passed\n");
  return 0;
}
