#pragma once
// Releasing retired GPU resources a few per frame (task 174).
//
// The caches drop everything unused for 1800 frames in one Trim, so the first trim after a chapter
// starts retires the previous chapter's (or the loading screen's) textures and buffers together:
// 300-600 resources. Destroying them is a driver call each (0.15-0.2 ms: vkDestroyImage/vkFreeMemory,
// ID3D12Resource::Release), and doing all of them in one frame made a 50-130 ms render-thread frame
// that the game waited for in Present, about 30 s into every chapter.
//
// Each frame now releases the entries that have passed their GPU lifetime (`keep_frames`) until the
// time budget is used, at least `min_per_frame` of them. Waiting longer is always safe (the GPU only
// needs them alive for `keep_frames`). Memory stays bounded: entries retired more than
// `overdue_frames` ago are released regardless of the budget, and `all` (memory pressure) releases
// every aged entry at once as before.
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace nr {

struct RetireBudget {
  double budget_ms = 1.0;       // per frame
  uint32_t min_per_frame = 2;   // released even when one release alone uses the budget
  uint64_t overdue_frames = 240;  // after retirement: released regardless of the budget
};

struct RetireRelease {
  size_t released = 0;
  size_t aged_left = 0;  // past keep_frames but held for a later frame (budget)
};

// Queue: a deque of (retired frame, handle) in retirement order. Popping an entry destroys it.
template <class Queue>
RetireRelease ReleaseRetired(Queue& queue, uint64_t frame, uint64_t keep_frames, bool all,
                             const RetireBudget& budget) {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  const auto limit = start + std::chrono::duration_cast<Clock::duration>(
                                 std::chrono::duration<double, std::milli>(budget.budget_ms));
  RetireRelease r;
  while (!queue.empty() && queue.front().first + keep_frames < frame) {
    const bool overdue = queue.front().first + budget.overdue_frames < frame;
    if (!all && !overdue && r.released >= budget.min_per_frame && Clock::now() >= limit) break;
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
