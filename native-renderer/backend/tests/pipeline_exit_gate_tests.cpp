// Task 233: the pipeline exit gate. Before a hard exit the gate closes: a worker's compile is refused from then on,
// a render-thread creation is only counted, and the exit waits for the creations inside the driver, never longer
// than its limit. A device-owning thread holds (no teardown) while the exit runs, unless its owner stops it.
#include "backend/pipeline_exit_gate.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

static int cases = 0;
static void Check(bool condition, const char* what) {
  ++cases;
  if (!condition) {
    std::fprintf(stderr, "case %d failed: %s\n", cases, what);
    std::exit(1);
  }
}
static double MsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

int main() {
  using namespace nr;
  using namespace std::chrono_literals;

  // Open: both kinds enter and are counted; Leave uncounts.
  {
    PipelineExitGate gate;
    Check(gate.Enter(true) && gate.Enter(false), "an open gate lets both kinds in");
    Check(gate.inside() == 2, "two inside");
    gate.Leave();
    gate.Leave();
    Check(gate.inside() == 0 && !gate.closed() && gate.refused() == 0, "nothing inside, open, nothing refused");
  }

  // Nothing running: the close returns at once; a second close is not the first.
  {
    PipelineExitGate gate;
    const auto start = std::chrono::steady_clock::now();
    const PipelineExitGate::Result r = gate.CloseAndWait(1000ms);
    Check(r.first && r.running == 0 && r.left == 0, "first close, none running");
    Check(MsSince(start) < 100, "no wait when nothing runs");
    const PipelineExitGate::Result again = gate.CloseAndWait(1000ms);
    Check(!again.first && again.running == 0 && again.left == 0, "a later close is not the first");
  }

  // Closed: a worker's compile is refused (nothing counted), a render-thread creation is counted.
  {
    PipelineExitGate gate;
    gate.CloseAndWait(0ms);
    {
      PipelineExitGate::Pass worker(gate, true);
      Check(!worker, "a background compile is refused after the close");
      Check(gate.inside() == 0 && gate.refused() == 1, "refused, not counted");
    }
    Check(gate.inside() == 0, "a refused pass leaves nothing behind");
    {
      PipelineExitGate::Pass render(gate, false);
      Check(bool(render) && gate.inside() == 1, "a render-thread creation is never refused, only counted");
    }
    Check(gate.inside() == 0, "its pass left");
  }

  // The close waits for a creation inside the driver (100 ms here), not for its limit.
  {
    PipelineExitGate gate;
    std::atomic<bool> entered{false};
    std::thread compile([&] {
      PipelineExitGate::Pass pass(gate, true);
      entered = true;
      std::this_thread::sleep_for(100ms);
    });
    while (!entered) std::this_thread::yield();
    const PipelineExitGate::Result r = gate.CloseAndWait(1000ms);
    compile.join();
    Check(r.first && r.running == 1 && r.left == 0, "one running, finished within the limit");
    Check(r.waited_ms >= 60 && r.waited_ms < 900, "waited about as long as the compile");
  }

  // Bounded: a compile that does not finish never holds the exit past the limit.
  {
    PipelineExitGate gate;
    std::atomic<bool> entered{false}, release{false};
    std::thread compile([&] {
      PipelineExitGate::Pass pass(gate, true);
      entered = true;
      while (!release) std::this_thread::sleep_for(5ms);
    });
    while (!entered) std::this_thread::yield();
    const PipelineExitGate::Result r = gate.CloseAndWait(150ms);
    Check(r.running == 1 && r.left == 1, "still running when the limit ended the wait");
    Check(r.waited_ms >= 140 && r.waited_ms < 1000, "the wait ended at its limit");
    release = true;
    compile.join();
    Check(gate.inside() == 0, "it left when it finished");
  }

  // A worker pool: after the close no job starts inside the driver; the queue drains with refusals at once.
  {
    PipelineExitGate gate;
    std::mutex m;
    std::deque<int> jobs;
    for (int i = 0; i < 400; ++i) jobs.push_back(i);
    std::atomic<int> made{0}, refused{0}, started_after_close{0};
    std::atomic<bool> closed{false};
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w) {
      workers.emplace_back([&] {
        for (;;) {
          {
            std::lock_guard lock(m);
            if (jobs.empty()) return;
            jobs.pop_front();
          }
          PipelineExitGate::Pass pass(gate, true);
          if (!pass) {
            ++refused;
            continue;
          }
          if (closed) ++started_after_close;
          std::this_thread::sleep_for(10ms);
          ++made;
        }
      });
    }
    std::this_thread::sleep_for(35ms);
    const PipelineExitGate::Result r = gate.CloseAndWait(1000ms);
    closed = true;
    const auto drained = std::chrono::steady_clock::now();
    for (std::thread& t : workers) t.join();
    Check(r.left == 0 && r.running <= 4, "the running compiles finished within the limit");
    Check(started_after_close == 0, "no compile entered after the close returned");
    Check(made + refused == 400 && refused > 300, "the rest of the queue was refused");
    Check(MsSince(drained) < 200, "the refused queue drains at once");
    Check(gate.refused() == uint64_t(refused.load()), "the gate counted every refusal");
  }

  // A device-owning thread: no hold while the gate is open; held while closed until its owner stops it.
  {
    PipelineExitGate gate;
    const auto start = std::chrono::steady_clock::now();
    HoldWhileExiting(gate, [] { return false; });
    Check(MsSince(start) < 50, "an open gate never holds");
    gate.CloseAndWait(0ms);
    std::atomic<bool> stop{false};
    std::atomic<bool> released{false};
    std::thread owner([&] {
      HoldWhileExiting(gate, [&] { return stop.load(); }, std::chrono::milliseconds(5));
      released = true;
    });
    std::this_thread::sleep_for(60ms);
    Check(!released, "held while the program exits");
    stop = true;
    owner.join();
    Check(released, "an orderly stop ends the hold (its owner joins it)");
  }

  // The process gate is one object.
  Check(&ExitGate() == &ExitGate(), "one gate per module");

  std::printf("pipeline exit gate tests: %d cases passed\n", cases);
  return 0;
}
