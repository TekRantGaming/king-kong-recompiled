// Guest frame-rate measurement.
//
// sub_821141D8 is the title's D3D swap routine (builds the swap packet and
// calls VdSwap), so it runs once per presented guest frame. Count calls to
// get the real game frame rate (the host presenter can run faster), log it
// once a second, and feed ReXGlue's F3 debug overlay. Also applies the
// kk_frame_rate cap.

#include "frame_stats.h"

#include <algorithm>
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

#include "crash_dump.h"
#include "presentation.h"
#include "settings.h"

namespace kk {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<uint64_t> g_frames{0};
std::atomic<int64_t> g_last_movie_video_read_ns{0};
std::atomic<int64_t> g_last_gameplay_camera_ns{0};
std::mutex g_mutex;
std::mutex g_presentation_mutex;
Clock::time_point g_window_start = Clock::now();
uint64_t g_window_frames = 0;
double g_window_swap_ms = 0;  // time spent inside the swap call this window
rex::ui::FrameStats g_stats;

// The user's display choice stays fixed while this temporarily letterboxes
// menus and (optionally) cutscenes.
bool g_presentation_initialized = false;
bool g_display_letterbox = false;
PresentationMode g_presentation_mode = PresentationMode::kUnknown;

uint32_t GuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return std::byteswap(value);
}

struct MoviePlayerState {
  uint32_t player;
  uint32_t state_268;
  uint32_t state_272;

  bool playing() const { return player && (state_268 || state_272); }
};

MoviePlayerState GetMoviePlayerState(const uint8_t* base) {
  const uint32_t player = GuestU32(base, 0x82CCAE80);
  if (!player) return {};
  return {player, GuestU32(base, player + 268), GuestU32(base, player + 272)};
}

int64_t MovieVideoReadAgeMs() {
  const int64_t last = g_last_movie_video_read_ns.load(std::memory_order_relaxed);
  if (!last) return -1;
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  return (now - last) / 1'000'000;
}

bool MovieVideoReadRecently(int64_t age_ms) {
  // XMV playback can stop issuing reads for several seconds while the decoder
  // catches up. Keep the cutscene framing through those stalls; the menu script
  // below provides an immediate, reliable exit when playback returns to menus.
  return age_ms >= 0 && age_ms < std::chrono::seconds(120).count() * 1000;
}

bool GameplayCameraActive() {
  const int64_t last = g_last_gameplay_camera_ns.load(std::memory_order_relaxed);
  if (!last) return false;
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  return now - last < std::chrono::milliseconds(500).count() * 1'000'000LL;
}

void UpdatePresentation(const uint8_t* base) {
  // The game's XMV player is at 0x82CCAE80. The menu script is detected by the
  // camera hook's recent-view counter, shared with the ultrawide FOV correction.
  const bool menu_script_active = kk::IsMenuActive();
  const auto movie_player = GetMoviePlayerState(base);
  const int64_t movie_read_age_ms = MovieVideoReadAgeMs();
  const bool recent_video_activity = !menu_script_active && MovieVideoReadRecently(movie_read_age_ms);
  const bool gameplay_camera = GameplayCameraActive();
  const bool movie = kk::KeepCutsceneFraming(movie_player.playing(), recent_video_activity, gameplay_camera);
  const bool menu = kk::KeepMenuFraming(menu_script_active);
  std::lock_guard lock(g_presentation_mutex);
  if (!g_presentation_initialized) {
    g_display_letterbox = REXCVAR_QUERY(bool, present_letterbox);
    g_presentation_initialized = true;
  }
  // Cutscenes and active menu scripts use 16:9; gameplay follows the selected
  // display aspect as soon as the menu script stops running.
  const auto choice = kk::ChoosePresentation(movie, menu, REXCVAR_QUERY(bool, kk_cutscenes_16_9), g_display_letterbox);
  const bool letterbox = choice.letterbox_16_9;
  if (REXCVAR_QUERY(bool, present_letterbox) != letterbox) {
    rex::cvar::SetFlagByName("present_letterbox", letterbox ? "true" : "false");
  }
  if (choice.mode == g_presentation_mode) return;
  g_presentation_mode = choice.mode;
  const char* mode_name = choice.mode == PresentationMode::kCutscene
                              ? "cutscene"
                              : choice.mode == PresentationMode::kMenu ? "menu" : "gameplay";
  REXLOG_INFO("KK: presentation mode is {} ({}; movie_player={:08X} state={:08X}/{:08X} video_read_age={}ms menu={} gameplay_camera={})",
              mode_name, letterbox ? "16:9" : "ultrawide", movie_player.player, movie_player.state_268,
              movie_player.state_272, movie_read_age_ms, menu_script_active, gameplay_camera);
}

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
    if (frame_ms >= 50.0) REXLOG_INFO("Hitch: {:.0f} ms frame", frame_ms);
    RecordFrameTime(frame_ms);
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

void NotifyMovieActivity() {
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  g_last_movie_video_read_ns.store(now, std::memory_order_relaxed);
}

bool IsMoviePlayerActive(const uint8_t* base) {
  return GetMoviePlayerState(base).playing();
}

void NotifyGameplayCamera() {
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  g_last_gameplay_camera_ns.store(now, std::memory_order_relaxed);
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
#else
  // Sleep for the bulk of the wait, then spin for precision.
  const auto coarse = next - now - std::chrono::microseconds(500);
  if (coarse > Clock::duration::zero()) std::this_thread::sleep_for(coarse);
#endif
  while (Clock::now() < next) std::this_thread::yield();
  next += period;
}

}  // namespace
}  // namespace kk

REX_EXTERN(__imp__sub_821141D8);
REX_HOOK_RAW(sub_821141D8) {
  kk::UpdatePresentation(base);
  kk::LimitFrameRate();
  kk::OnGuestSwap();
  const auto start = kk::Clock::now();
  __imp__sub_821141D8(ctx, base);
  kk::AddSwapTime(std::chrono::duration<double, std::milli>(kk::Clock::now() - start).count());
}

// Story movies are queued through sub_82723AF0. Sample immediately after it
// updates the XMV player so the presentation mode can switch on the first
// frame, even if the swap hook runs before the player state is updated.
REX_EXTERN(__imp__sub_82723AF0);
REX_HOOK_RAW(sub_82723AF0) {
  __imp__sub_82723AF0(ctx, base);
  kk::UpdatePresentation(base);
}
