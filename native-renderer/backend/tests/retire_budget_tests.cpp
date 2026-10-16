// Task 174: retired resources are released within a per-frame budget, never before their GPU lifetime,
// all at once under memory pressure, and never later than overdue_frames.
#include "backend/retire_budget.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <utility>

static int cases = 0;
static void Check(bool condition) {
  ++cases;
  if (!condition) { std::fprintf(stderr, "case %d failed\n", cases); std::exit(1); }
}

int main() {
  using namespace nr;
  using Queue = std::deque<std::pair<uint64_t, std::shared_ptr<int>>>;
  auto fill = [](Queue& q, uint64_t frame, int n, std::shared_ptr<int>& probe) {
    for (int i = 0; i < n; ++i) q.emplace_back(frame, std::make_shared<int>(i));
    probe = q.back().second;
  };
  RetireBudget none;  // a zero time budget: only min_per_frame (plus overdue) each frame
  none.budget_ms = 0;
  none.min_per_frame = 3;
  none.overdue_frames = 100;

  // Nothing is released before frame + keep_frames has passed.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 50, probe);
    Check(ReleaseRetired(q, 18, 8, false, none).released == 0);
    Check(ReleaseRetired(q, 18, 8, true, none).released == 0);
    Check(q.size() == 50);
  }
  // A burst is spread: min_per_frame a frame, the rest held (and counted) for later frames.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 50, probe);
    const RetireRelease r = ReleaseRetired(q, 19, 8, false, none);
    Check(r.released == 3 && r.aged_left == 47 && q.size() == 47);
    size_t frames = 1;
    for (uint64_t f = 20; !q.empty(); ++f, ++frames) ReleaseRetired(q, f, 8, false, none);
    Check(frames == 17);  // ceil(50 / 3)
    Check(probe.use_count() == 1);  // the queue no longer holds it
  }
  // Younger entries behind the aged ones are left alone and not counted as aged.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 5, probe);
    fill(q, 30, 5, probe);
    const RetireRelease r = ReleaseRetired(q, 20, 8, false, none);
    Check(r.released == 3 && r.aged_left == 2 && q.size() == 7);
    Check(ReleaseRetired(q, 20, 8, true, none).released == 2 && q.size() == 5);
  }
  // Memory pressure (all): every aged entry at once.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 50, probe);
    const RetireRelease r = ReleaseRetired(q, 19, 8, true, none);
    Check(r.released == 50 && r.aged_left == 0 && q.empty());
  }
  // Overdue entries are released regardless of the budget (memory stays bounded).
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 40, probe);
    fill(q, 200, 10, probe);
    const RetireRelease r = ReleaseRetired(q, 211, 8, false, none);  // 10 + 100 < 211: first 40 overdue
    Check(r.released == 40 && r.aged_left == 10 && q.size() == 10);  // the overdue count toward the minimum
  }
  // A generous budget releases a normal frame's handful in one go.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 20, probe);
    RetireBudget wide = none;
    wide.budget_ms = 1000;
    Check(ReleaseRetired(q, 19, 8, false, wide).released == 20 && q.empty());
  }
  std::printf("retire budget tests: %d cases passed\n", cases);
  return 0;
}
