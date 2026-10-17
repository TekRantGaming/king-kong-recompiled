#pragma once
// Releasing retired GPU resources a few per frame (tasks 174, 185).
//
// The caches drop everything unused for 1800 frames in one Trim, so the first trim after a chapter
// starts retires the previous chapter's (or the loading screen's) textures and buffers together:
// 300-1200 resources. Destroying them is a driver call each (0.15-0.2 ms: vkDestroyImage/vkFreeMemory,
// ID3D12Resource::Release), and doing all of them in one frame made a 50-130 ms render-thread frame
// that the game waited for in Present, about 30 s into every chapter.
//
// Each frame now releases the entries that have passed their GPU lifetime (`keep_frames`) until the
// time budget is used, at least `min_per_frame` of them. Waiting longer is always safe (the GPU only
// needs them alive for `keep_frames`).
//
// Memory stays bounded without ever releasing a backlog in one unbudgeted frame (task 185: a ~1180
// resource trim on the Skull Island coast outlived the old 240-frame "release regardless" window and
// the rest went in one 49-63 ms frame). The budget grows with the age of the oldest waiting entry:
// `budget_ms` until `overdue_frames - ramp_frames` past its lifetime, then rising linearly to
// `max_budget_ms` at `overdue_frames`, and `max_budget_ms` after that. At the maximum a frame
// releases ~20 resources (0.2 ms each), far more than the caches ever retire per frame on average
// (one trim every 300 frames), so any backlog drains. `all` (memory pressure) still releases every
// aged entry at once, as before.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace nr {

struct RetireBudget {
  double budget_ms = 1.0;        // per frame while the backlog is young
  double max_budget_ms = 4.0;    // per frame once the oldest entry is overdue
  uint32_t min_per_frame = 2;    // released even when one release alone uses the budget
  uint64_t overdue_frames = 240;  // past the lifetime: the budget has reached max_budget_ms
  uint64_t ramp_frames = 120;     // the budget rises over the last ramp_frames before overdue_frames
};

struct RetireRelease {
  size_t released = 0;
  size_t aged_left = 0;    // past keep_frames but held for a later frame (budget)
  double budget_ms = 0;    // the budget this frame had
  uint64_t oldest_age = 0;  // frames the oldest aged entry had waited past its lifetime (at the start)
};

// The time budget for a frame whose oldest aged entry has waited `age` frames past its lifetime.
inline double RetireBudgetFor(const RetireBudget& budget, uint64_t age) {
  const double low = budget.budget_ms;
  const double high = std::max(budget.max_budget_ms, low);
  const uint64_t ramp = std::min(budget.ramp_frames, budget.overdue_frames);
  const uint64_t ramp_start = budget.overdue_frames - ramp;
  if (age <= ramp_start) return low;
  if (age >= budget.overdue_frames || ramp == 0) return high;
  return low + (high - low) * double(age - ramp_start) / double(ramp);
}

// Queue: a deque of (retired frame, handle) in retirement order. Popping an entry destroys it.
// Clock: steady_clock, or a test clock with the same now()/duration/time_point.
template <class Queue, class Clock = std::chrono::steady_clock>
RetireRelease ReleaseRetired(Queue& queue, uint64_t frame, uint64_t keep_frames, bool all,
                             const RetireBudget& budget) {
  RetireRelease r;
  if (!queue.empty() && queue.front().first + keep_frames < frame)
    r.oldest_age = frame - (queue.front().first + keep_frames) - 1;
  r.budget_ms = RetireBudgetFor(budget, r.oldest_age);
  const auto start = Clock::now();
  const auto limit = start + std::chrono::duration_cast<typename Clock::duration>(
                                 std::chrono::duration<double, std::milli>(r.budget_ms));
  while (!queue.empty() && queue.front().first + keep_frames < frame) {
    if (!all && r.released >= budget.min_per_frame && Clock::now() >= limit) break;
    queue.pop_front();
    ++r.released;
  }
  for (const auto& entry : queue) {
    if (entry.first + keep_frames >= frame) break;
    ++r.aged_left;
  }
  return r;
}

}  // namespace nr
