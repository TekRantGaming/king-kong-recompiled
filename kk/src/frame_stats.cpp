// Guest frame-rate measurement.
//
// sub_821141D8 is the title's D3D swap routine (builds the swap packet and
// calls VdSwap), so it runs once per presented guest frame. Count calls to
// get the real game frame rate (the host presenter can run faster), log it
// once a second, and feed ReXGlue's F3 debug overlay. Also applies the
// kk_frame_rate cap.

#include "frame_stats.h"

#include <atomic>
#include <bit>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/hook.h>
#include <rex/logging.h>

#include "settings.h"

namespace kk {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<uint64_t> g_frames{0};
std::mutex g_mutex;
Clock::time_point g_window_start = Clock::now();
uint64_t g_window_frames = 0;
double g_window_swap_ms = 0;  // time spent inside the swap call this window
rex::ui::FrameStats g_stats;

struct Deferred {
  double after;
  std::function<void()> fn;
};
std::vector<Deferred> g_deferred;
Clock::time_point g_first_frame{};

void RunDeferredIfDue(Clock::time_point now) {
  std::vector<std::function<void()>> due;
  {
    std::lock_guard lock(g_mutex);
    if (g_first_frame == Clock::time_point{}) g_first_frame = now;
    if (g_deferred.empty()) return;
    const double elapsed = std::chrono::duration<double>(now - g_first_frame).count();
    for (auto it = g_deferred.begin(); it != g_deferred.end();) {
      if (elapsed >= it->after) {
        due.push_back(std::move(it->fn));
        it = g_deferred.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& fn : due) fn();
}

void OnGuestSwap() {
  RunDeferredIfDue(Clock::now());
  const uint64_t total = g_frames.fetch_add(1) + 1;
  std::lock_guard lock(g_mutex);
  ++g_window_frames;
  const auto now = Clock::now();
  // Log single long frames (stutter); the per-second average hides them.
  static Clock::time_point last_swap{};
  if (last_swap != Clock::time_point{}) {
    const double frame_ms = std::chrono::duration<double, std::milli>(now - last_swap).count();
    if (frame_ms >= 50.0) REXLOG_INFO("Hitch: {:.0f} ms frame", frame_ms);
  }
  last_swap = now;
  const double elapsed = std::chrono::duration<double>(now - g_window_start).count();
  if (elapsed >= 1.0) {
    g_stats.fps = g_window_frames / elapsed;
    g_stats.frame_time_ms = elapsed * 1000.0 / g_window_frames;
    g_stats.frame_count = total;
    REXLOG_INFO("Guest FPS: {:.1f} ({:.2f} ms/frame, {:.2f} ms in swap)",
                g_stats.fps, g_stats.frame_time_ms, g_window_swap_ms / g_window_frames);
    g_window_start = now;
    g_window_frames = 0;
    g_window_swap_ms = 0;
  }
}


void AddSwapTime(double ms) {
  std::lock_guard lock(g_mutex);
  g_window_swap_ms += ms;
}

}  // namespace

rex::ui::FrameStats GetGuestFrameStats() {
  std::lock_guard lock(g_mutex);
  return g_stats;
}

void RunAfterFirstFrame(double seconds, std::function<void()> fn) {
  std::lock_guard lock(g_mutex);
  g_deferred.push_back({seconds, std::move(fn)});
}

void RunAfterDelay(double seconds, std::function<void()> fn) {
  std::lock_guard lock(g_mutex);
  const double now = g_first_frame == Clock::time_point{}
                         ? 0.0
                         : std::chrono::duration<double>(Clock::now() - g_first_frame).count();
  g_deferred.push_back({now + seconds, std::move(fn)});
}

}  // namespace kk

namespace kk {
namespace {

// Frame limiter for kk_frame_rate: high-resolution waitable timer for the bulk
// of the wait, then a short spin for precision.
void LimitFrameRate() {
  const int32_t fps = REXCVAR_GET(kk_frame_rate);
  static Clock::time_point next{};
  if (fps <= 0) {
    next = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
  auto now = Clock::now();
  if (next == Clock::time_point{} || now - next > period) {
    next = now + period;  // first frame, or fell behind: resync
    return;
  }
#if defined(_WIN32)
  static HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const auto coarse = next - now - std::chrono::microseconds(500);
  if (timer && coarse > Clock::duration::zero()) {
    LARGE_INTEGER due;
    due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(coarse).count() / 100;
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
  }
#endif
  while (Clock::now() < next) std::this_thread::yield();
  next += period;
}

}  // namespace
}  // namespace kk

REX_EXTERN(__imp__sub_821141D8);
REX_HOOK_RAW(sub_821141D8) {
  kk::LimitFrameRate();
  kk::OnGuestSwap();
  const auto start = kk::Clock::now();
  __imp__sub_821141D8(ctx, base);
  kk::AddSwapTime(std::chrono::duration<double, std::milli>(kk::Clock::now() - start).count());
}
