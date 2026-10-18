// Guest frame-rate measurement.
//
// sub_821141D8 is the title's D3D swap routine (builds the swap packet and
// calls VdSwap), so it runs once per presented guest frame. Count calls to
// get the real game frame rate (the host presenter can run faster), log it
// once a second, and feed ReXGlue's F3 debug overlay. Also applies the
// kk_frame_rate cap.

#include "ui_paint.h"
#include "handoff.h"

#include "frame_stats.h"
#include "frame_pacer.h"
#include "benchmark.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <cstdlib>
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

#include "crash_dump.h"
#include "settings.h"
#include "telemetry.h"
#include "ps2_movies.h"

namespace kk {
#if defined(KK_DEV_TOOLS) && defined(_WIN32)
namespace dev {
void GameWatchFrame();  // dev_game_watch.cpp
}
#endif
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<uint64_t> g_frames{0};
std::mutex g_mutex;
Clock::time_point g_window_start = Clock::now();
uint64_t g_window_frames = 0;
double g_window_swap_ms = 0;  // time spent inside the swap call this window
double g_window_worst_ms = 0, g_worst_ms = 0;  // the longest frame of this window, of the last one
rex::ui::FrameStats g_stats;
// The last guest frame times (ms), for the Performance metrics overlay's graph.
constexpr size_t kRecentFrames = 256;
std::array<float, kRecentFrames> g_recent{};
size_t g_recent_count = 0;  // all frames ever recorded; the newest is at (count - 1) % size

struct Deferred {
  double after;
  std::function<void()> fn;
};
std::vector<Deferred> g_deferred;
Clock::time_point g_first_frame{};
// Task 211 (telemetry's worst-frame detail): the last swap call's time and this frame's frame-cap wait (ms).
std::atomic<double> g_last_swap_call_ms{-1}, g_cap_wait_ms{-1};

void RunDeferredIfDue(Clock::time_point now) {
  std::vector<std::function<void()>> due;
  {
    std::lock_guard lock(g_mutex);
    if (g_first_frame == Clock::time_point{}) {
      g_first_frame = now;
      handoff::ContentDrawn();  // the game's first frame
    }
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

// Every 10 s: average and 1% low frame rate, the worst frame and how many
// frames took over 50 and 100 ms, so stutter shows up in players' logs.
void RecordFrameTime(double frame_ms) {
  static std::vector<double> times;
  static Clock::time_point start = Clock::now();
  times.push_back(frame_ms);
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  if (elapsed < 10.0) return;
  std::sort(times.begin(), times.end());
  const size_t n = times.size();
  double sum = 0, low_sum = 0;
  for (double t : times) sum += t;
  const size_t low_n = std::max<size_t>(1, n / 100);
  for (size_t i = n - low_n; i < n; ++i) low_sum += times[i];
  const auto over = [&](double ms) { return size_t(times.end() - std::upper_bound(times.begin(), times.end(), ms)); };
  REXLOG_INFO("Frame times: {:.1f} FPS average, {:.1f} FPS 1% low, worst {:.0f} ms, {} over 50 ms, {} over 100 ms",
              1000.0 * n / sum, 1000.0 * low_n / low_sum, times.back(), over(50.0), over(100.0));
  times.clear();
  start = Clock::now();
}

void OnGuestSwap() {
#if defined(KK_DEV_TOOLS) && defined(_WIN32)
  dev::GameWatchFrame();  // task 217 (dev_game_watch.cpp)
#endif
  benchmark::GuestSwap();
  NoteGuestFrame();
  RunDeferredIfDue(Clock::now());
  const uint64_t total = g_frames.fetch_add(1) + 1;
  std::lock_guard lock(g_mutex);
  ++g_window_frames;
  const auto now = Clock::now();
  // Log single long frames (stutter); the per-second average hides them.
  static Clock::time_point last_swap{};
  if (last_swap != Clock::time_point{}) {
    const double frame_ms = std::chrono::duration<double, std::milli>(now - last_swap).count();
    if (frame_ms >= 50.0) {
      // Task 217: the level loader waited for a PS2 cutscene (the player watched it): not a stutter.
      static uint32_t cutscenes_seen = 0;
      const uint32_t cutscenes = movies::LoaderWaitsEnded();
      if (cutscenes != cutscenes_seen) REXLOG_INFO("Hitch: {:.0f} ms frame (a PS2 cutscene played)", frame_ms);
      else REXLOG_INFO("Hitch: {:.0f} ms frame", frame_ms);
      cutscenes_seen = cutscenes;
    }
    g_window_worst_ms = std::max(g_window_worst_ms, frame_ms);
    g_recent[g_recent_count++ % kRecentFrames] = float(frame_ms);
    RecordFrameTime(frame_ms);
    // The per-chapter summary (Beta 7 uploads); the swap call and cap wait inside this frame (task 211).
    telemetry::NoteFrame(frame_ms, g_last_swap_call_ms.load(std::memory_order_relaxed),
                         g_cap_wait_ms.load(std::memory_order_relaxed));
  }
  last_swap = now;
  // Full performance metrics: the graph and readings repaint ten times a second (the frame rate line
  // above once a second is enough for "FPS only").
  static Clock::time_point last_paint{};
  if (now - last_paint >= std::chrono::milliseconds(100) && REXCVAR_GET(kk_performance_metrics) == "full") {
    last_paint = now;
    RequestUiPaint();
  }
  const double elapsed = std::chrono::duration<double>(now - g_window_start).count();
  if (elapsed >= 1.0) {
    g_stats.fps = g_window_frames / elapsed;
    g_stats.frame_time_ms = elapsed * 1000.0 / g_window_frames;
    g_stats.frame_count = total;
    if (REXCVAR_GET(kk_performance_metrics) != "off") RequestUiPaint();
    g_worst_ms = g_window_worst_ms;
    g_window_worst_ms = 0;
    REXLOG_INFO("Guest FPS: {:.1f} ({:.2f} ms/frame, {:.2f} ms in swap)",
                g_stats.fps, g_stats.frame_time_ms, g_window_swap_ms / g_window_frames);
#if defined(KK_DEV_TOOLS)
    if (std::getenv("KK_DEV_UIPAINT_TEST"))
      REXLOG_INFO("UI repaint guest trace: {} guest swaps in {:.3f} s", g_window_frames, elapsed);
#endif
    g_window_start = now;
    g_window_frames = 0;
    g_window_swap_ms = 0;
  }
}


void AddSwapTime(double ms) {
  g_last_swap_call_ms.store(ms, std::memory_order_relaxed);
  std::lock_guard lock(g_mutex);
  g_window_swap_ms += ms;
}

}  // namespace

rex::ui::FrameStats GetGuestFrameStats() {
  std::lock_guard lock(g_mutex);
  return g_stats;
}

size_t GuestFrameTimes(float* out, size_t max) {
  std::lock_guard lock(g_mutex);
  const size_t n = std::min({max, g_recent_count, kRecentFrames});
  for (size_t i = 0; i < n; ++i) out[i] = g_recent[(g_recent_count - n + i) % kRecentFrames];
  return n;
}

double GuestWorstFrameMs() {
  std::lock_guard lock(g_mutex);
  return g_worst_ms;
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
  static pacing::Limiter limiter;
  limiter.Wait(REXCVAR_GET(kk_frame_rate));
}

}  // namespace
}  // namespace kk

REX_EXTERN(__imp__sub_821141D8);
REX_HOOK_RAW(sub_821141D8) {
  const auto cap_start = kk::Clock::now();
  kk::LimitFrameRate();
  kk::g_cap_wait_ms.store(std::chrono::duration<double, std::milli>(kk::Clock::now() - cap_start).count(),
                          std::memory_order_relaxed);
  kk::OnGuestSwap();
  const auto start = kk::Clock::now();
  __imp__sub_821141D8(ctx, base);
  kk::AddSwapTime(std::chrono::duration<double, std::milli>(kk::Clock::now() - start).count());
}
