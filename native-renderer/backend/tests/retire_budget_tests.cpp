// Tasks 174/185: retired resources are released within a per-frame budget, never before their GPU
// lifetime, all at once under memory pressure, and a backlog that ages gets a larger (but still bounded)
// budget instead of one unbudgeted frame.
#include "backend/retire_budget.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <utility>

static int cases = 0;
static void Check(bool condition, int line) {
  ++cases;
  if (!condition) { std::fprintf(stderr, "case %d (line %d) failed\n", cases, line); std::exit(1); }
}
#define CHECK(c) Check((c), __LINE__)

// A clock that only moves when a resource is released (each release "costs" release_ns).
struct FakeClock {
  using rep = int64_t;
  using period = std::nano;
  using duration = std::chrono::nanoseconds;
  using time_point = std::chrono::time_point<FakeClock>;
  static constexpr bool is_steady = true;
  static inline int64_t now_ns = 0;
  static inline int64_t release_ns = 200000;  // 0.2 ms, a vkDestroyImage + vkFreeMemory
  static time_point now() { return time_point(duration(now_ns)); }
};
struct CostlyResource {
  ~CostlyResource() { FakeClock::now_ns += FakeClock::release_ns; }
};

int main() {
  using namespace nr;
  using Queue = std::deque<std::pair<uint64_t, std::shared_ptr<int>>>;
  auto fill = [](Queue& q, uint64_t frame, int n, std::shared_ptr<int>& probe) {
    for (int i = 0; i < n; ++i) q.emplace_back(frame, std::make_shared<int>(i));
    probe = q.back().second;
  };
  RetireBudget none;  // a zero time budget at every age: only min_per_frame each frame
  none.budget_ms = 0;
  none.max_budget_ms = 0;
  none.min_per_frame = 3;
  none.overdue_frames = 100;

  // Nothing is released before frame + keep_frames has passed.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 50, probe);
    CHECK(ReleaseRetired(q, 18, 8, false, none).released == 0);
    CHECK(ReleaseRetired(q, 18, 8, true, none).released == 0);
    CHECK(q.size() == 50);
  }
  // A burst is spread: min_per_frame a frame, the rest held (and counted) for later frames.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 50, probe);
    const RetireRelease r = ReleaseRetired(q, 19, 8, false, none);
    CHECK(r.released == 3 && r.aged_left == 47 && q.size() == 47 && r.oldest_age == 0);
    size_t frames = 1;
    for (uint64_t f = 20; !q.empty(); ++f, ++frames) ReleaseRetired(q, f, 8, false, none);
    CHECK(frames == 17);  // ceil(50 / 3)
    CHECK(probe.use_count() == 1);  // the queue no longer holds it
  }
  // Younger entries behind the aged ones are left alone and not counted as aged.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 5, probe);
    fill(q, 30, 5, probe);
    const RetireRelease r = ReleaseRetired(q, 20, 8, false, none);
    CHECK(r.released == 3 && r.aged_left == 2 && q.size() == 7);
    CHECK(ReleaseRetired(q, 20, 8, true, none).released == 2 && q.size() == 5);
  }
  // Memory pressure (all): every aged entry at once.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 50, probe);
    const RetireRelease r = ReleaseRetired(q, 19, 8, true, none);
    CHECK(r.released == 50 && r.aged_left == 0 && q.empty());
  }
  // Overdue entries are no longer released regardless of the budget (task 185): still budgeted.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 40, probe);
    fill(q, 200, 10, probe);
    const RetireRelease r = ReleaseRetired(q, 211, 8, false, none);  // first 40 are 192 frames past their lifetime
    CHECK(r.released == 3 && r.aged_left == 47 && r.oldest_age == 192);
  }
  // A generous budget releases a normal frame's handful in one go.
  {
    Queue q;
    std::shared_ptr<int> probe;
    fill(q, 10, 20, probe);
    RetireBudget wide = none;
    wide.budget_ms = 1000;
    CHECK(ReleaseRetired(q, 19, 8, false, wide).released == 20 && q.empty());
  }
  // The budget by age: flat, then a linear ramp to the maximum at overdue_frames, then flat.
  {
    RetireBudget b;  // 1 ms, 4 ms, ramp over ages 120..240
    CHECK(RetireBudgetFor(b, 0) == 1.0 && RetireBudgetFor(b, 120) == 1.0);
    CHECK(RetireBudgetFor(b, 180) == 2.5);
    CHECK(RetireBudgetFor(b, 240) == 4.0 && RetireBudgetFor(b, 100000) == 4.0);
    RetireBudget inverted = b;
    inverted.max_budget_ms = 0.5;  // never below budget_ms
    CHECK(RetireBudgetFor(inverted, 1000) == 1.0);
    RetireBudget no_ramp = b;
    no_ramp.ramp_frames = 0;
    CHECK(RetireBudgetFor(no_ramp, 240) == 1.0 && RetireBudgetFor(no_ramp, 241) == 4.0);
  }

  // The coast backlog (task 185): ~1180 resources at 0.2 ms each retired in one trim, with the default
  // budget. Before, the first ~1200 went at 1 ms a frame and whatever was left at 240 frames went in one
  // frame. Now: no frame above max_budget_ms + one release, and all gone well before overdue_frames.
  using CostQueue = std::deque<std::pair<uint64_t, std::shared_ptr<CostlyResource>>>;
  auto run = [](CostQueue& q, uint64_t from, uint64_t to, const RetireBudget& b, double* worst_ms,
                uint64_t* last_frame, size_t* biggest, auto&& retire_more) {
    for (uint64_t f = from; f < to; ++f) {
      retire_more(q, f);
      *biggest = std::max(*biggest, q.size());
      const int64_t before = FakeClock::now_ns;
      const RetireRelease r = ReleaseRetired<CostQueue, FakeClock>(q, f, 8, false, b);
      *worst_ms = std::max(*worst_ms, double(FakeClock::now_ns - before) / 1e6);
      if (r.released) *last_frame = f;
    }
  };
  auto trim = [](CostQueue& q, uint64_t f, int n) {
    for (int i = 0; i < n; ++i) q.emplace_back(f, std::make_shared<CostlyResource>());
  };
  {
    CostQueue q;
    trim(q, 10, 1180);
    double worst = 0;
    uint64_t last = 0;
    size_t biggest = 0;
    run(q, 11, 2000, RetireBudget{}, &worst, &last, &biggest, [](CostQueue&, uint64_t) {});
    std::printf("  1180-resource trim: drained by frame %llu (%llu frames past the lifetime), worst frame %.2f ms\n",
                static_cast<unsigned long long>(last), static_cast<unsigned long long>(last - 18), worst);
    CHECK(q.empty());
    CHECK(worst <= 4.0 + 0.2 + 1e-9);
    CHECK(last - 18 < 240);  // drained before the old overdue window ran out
  }
  // A far bigger backlog (10,000) still never makes a frame above the maximum, and drains.
  {
    CostQueue q;
    trim(q, 10, 10000);
    double worst = 0;
    uint64_t last = 0;
    size_t biggest = 0;
    run(q, 11, 5000, RetireBudget{}, &worst, &last, &biggest, [](CostQueue&, uint64_t) {});
    std::printf("  10000-resource backlog: drained by frame %llu, worst frame %.2f ms\n",
                static_cast<unsigned long long>(last), worst);
    CHECK(q.empty() && worst <= 4.2 + 1e-9);
  }
  // Memory stays bounded: a 1180 trim every 300 frames (the trim interval) for 30 trims, at an expensive
  // 0.5 ms a release; the queue never holds more than about one trim.
  {
    FakeClock::release_ns = 500000;
    CostQueue q;
    double worst = 0;
    uint64_t last = 0;
    size_t biggest = 0;
    run(q, 1, 9000, RetireBudget{}, &worst, &last, &biggest, [&](CostQueue& queue, uint64_t f) {
      if (f % 300 == 0) trim(queue, f, 1180);
    });
    std::printf("  a 1180 trim every 300 frames at 0.5 ms each: at most %zu waiting, worst frame %.2f ms\n", biggest,
                worst);
    CHECK(biggest <= 1180 + 20 && worst <= 4.5 + 1e-9);
    FakeClock::release_ns = 200000;
  }
  std::printf("retire budget tests: %d cases passed\n", cases);
  return 0;
}
