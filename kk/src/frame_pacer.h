#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace kk::pacing {
using Clock = std::chrono::steady_clock;

// There is no catch-up frame after work overruns the deadline: that would turn
// one late frame into a long/short pair. Keep the phase across small timer wake
// errors, so those errors do not accumulate into a lower average frame rate.
class Schedule {
 public:
  Clock::time_point Deadline(int fps, Clock::time_point now) {
    if (fps <= 0 || fps != fps_ || !started_) {
      fps_ = fps;
      started_ = fps > 0;
      return pending_ = now;
    }
    period_ = std::max(Clock::duration(1),
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps)));
    return pending_ = std::max(now, last_ + period_);
  }
  void Released(Clock::time_point now) {
    last_ = now - pending_ > period_ ? now : pending_;
  }

 private:
  int fps_ = 0;
  bool started_ = false;
  Clock::time_point last_{};
  Clock::time_point pending_{};
  Clock::duration period_{};
};

class Waiter {
 public:
  Waiter() = default;
  Waiter(const Waiter&) = delete;
  Waiter& operator=(const Waiter&) = delete;
  ~Waiter() {
#if defined(_WIN32)
    if (timer_) CloseHandle(timer_);
#endif
  }

  void Until(Clock::time_point deadline) {
    // Learn the timer's wake error. Usually the tail is 0.1 ms; its hard limit
    // is 0.5 ms, and only the part left after the timer wakes is active waiting.
    // Failed/unsupported timers always fall back to sleeping, never a full-frame spin.
    const auto tail = guard_;
    auto now = Clock::now();
    if (deadline - now > tail) {
      const auto coarse = deadline - now - tail;
      bool slept = false;
#if defined(_WIN32)
      if (!attempted_) {
        attempted_ = true;
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
      }
      if (timer_) {
        LARGE_INTEGER due;
        due.QuadPart = -std::max<int64_t>(1,
            std::chrono::duration_cast<std::chrono::nanoseconds>(coarse).count() / 100);
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
          slept = WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0;
      }
#endif
      if (!slept) std::this_thread::sleep_for(coarse);
      const auto overshoot = std::max(Clock::duration::zero(), Clock::now() - (deadline - tail));
      guard_ = std::clamp(std::max(overshoot + std::chrono::microseconds(50), guard_ * 9 / 10),
          std::chrono::duration_cast<Clock::duration>(std::chrono::microseconds(100)),
          std::chrono::duration_cast<Clock::duration>(std::chrono::microseconds(500)));
    }
    // Recheck the wall clock after every yield. No timer-resolution change,
    // additional frame queue, or wait when the renderer already missed the cap.
    // Task 295 (tail_spin, kk_cpu_priority >= 1): a yield under CPU load can hand the rest of the time
    // slice to another program's thread and overshoot by a scheduler quantum; the <= 0.5 ms tail
    // pauses the core instead.
    while (Clock::now() < deadline) {
#if defined(_M_X64) || defined(__x86_64__)
      if (tail_spin) { _mm_pause(); continue; }
#endif
      std::this_thread::yield();
    }
  }

  bool tail_spin = false;

 private:
  Clock::duration guard_ = std::chrono::microseconds(100);
#if defined(_WIN32)
  HANDLE timer_ = nullptr;
  bool attempted_ = false;
#endif
};

class Limiter {
 public:
  void Wait(int fps, bool tail_spin = false) {
    waiter_.tail_spin = tail_spin;
    const auto deadline = schedule_.Deadline(fps, Clock::now());
    if (fps > 0) waiter_.Until(deadline);
    schedule_.Released(Clock::now());
  }

 private:
  Schedule schedule_;
  Waiter waiter_;
};
}  // namespace kk::pacing
