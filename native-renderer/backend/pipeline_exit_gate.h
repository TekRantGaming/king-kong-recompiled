// Task 233: the program hard-exits (std::_Exit, the SDK's OnClosing) at the end of the game and of the launcher.
// A pipeline compile on a worker thread was killed inside the driver, possibly while the driver wrote its own
// shader cache (AMD reports, task 228: black screen until %LOCALAPPDATA%\AMD's caches were emptied).
//
// Every pipeline creation of the renderer passes this gate (renderer.cpp CreateGraphicsPipeline, the workers'
// library parts). Before an exit (or a hand-over to a new process) the gate closes: background compiles (the
// pipeline workers' jobs) are refused from then on, and the caller waits, bounded, for the creations already
// inside the driver. A render-thread creation is never refused (its draw would fail): it is only counted.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace nr {

class PipelineExitGate {
 public:
  struct Result {
    unsigned running = 0;  // creations inside the driver when the gate closed
    unsigned left = 0;     // still inside when the wait ended (the limit was reached)
    double waited_ms = 0;
    bool first = false;  // this call closed the gate (later calls only wait again)
  };

  // false: the gate is closed and this is a background compile (skip it; nothing was counted).
  bool Enter(bool background) {
    std::lock_guard lock(mutex_);
    if (closed_ && background) {
      ++refused_;
      return false;
    }
    ++inside_;
    return true;
  }
  void Leave() {
    {
      std::lock_guard lock(mutex_);
      if (inside_) --inside_;
    }
    cv_.notify_all();
  }

  // Closes the gate and waits until no creation is inside the driver, at most `limit`. Never hangs.
  Result CloseAndWait(std::chrono::milliseconds limit) {
    const auto start = std::chrono::steady_clock::now();
    Result r;
    std::unique_lock lock(mutex_);
    r.first = !closed_;
    closed_ = true;
    r.running = inside_;
    if (limit.count() > 0) cv_.wait_until(lock, start + limit, [this] { return inside_ == 0; });
    r.left = inside_;
    r.waited_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return r;
  }

  bool closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
  }
  unsigned inside() const {
    std::lock_guard lock(mutex_);
    return inside_;
  }
  uint64_t refused() const {
    std::lock_guard lock(mutex_);
    return refused_;
  }

  // Holds the gate for one creation (Enter in the constructor, Leave in the destructor when entered).
  class Pass {
   public:
    Pass(PipelineExitGate& gate, bool background) : gate_(gate), entered_(gate.Enter(background)) {}
    ~Pass() {
      if (entered_) gate_.Leave();
    }
    Pass(const Pass&) = delete;
    Pass& operator=(const Pass&) = delete;
    explicit operator bool() const { return entered_; }

   private:
    PipelineExitGate& gate_;
    bool entered_;
  };

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  unsigned inside_ = 0;
  uint64_t refused_ = 0;
  bool closed_ = false;
};

// The process's gate (one per module: the plugin DLL, or a test program).
inline PipelineExitGate& ExitGate() {
  static PipelineExitGate gate;
  return gate;
}

// A thread that owns a headless device (the launcher's prewarm, the setup's pipeline step): once the exit started
// it starts no new driver work (a device, its teardown and the cache save are driver calls too) and waits here,
// until the process ends or its owner stops it for an orderly end (`stopping` true: the owner then joins it).
template <class Stopping>
void HoldWhileExiting(PipelineExitGate& gate, Stopping stopping,
                      std::chrono::milliseconds poll = std::chrono::milliseconds(20)) {
  while (gate.closed() && !stopping()) std::this_thread::sleep_for(poll);
}

}  // namespace nr
