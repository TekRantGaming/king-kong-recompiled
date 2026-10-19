#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>
#include <thread>
#include "backend/pipeline_workers.h"
#include "backend/tests/check.h"
#ifdef _WIN32
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

using namespace nr;

TEST(HybridPlacementRespectsAllowedCoresAndSmt) {
  const std::vector<SchedulingCore> hybrid{{0x3, 1}, {0xC, 1}, {0x10, 0}, {0x20, 0}};
  CHECK_EQ(PerformanceCoreMask(0x3F, hybrid), uint64_t(0xF));
  CHECK_EQ(PerformanceCoreMask(0x35, hybrid), uint64_t(0x5));
  CHECK_EQ(PerformanceCoreMask(0x33, hybrid), uint64_t(0)); // only one P-core allowed
  CHECK_EQ(PerformanceCoreMask(0x30, hybrid), uint64_t(0)); // E-only restriction stays intact
  CHECK_EQ(PerformanceCoreMask(0, hybrid), uint64_t(0));
  CHECK_EQ(PerformanceCoreMask(0xF, {{0x3, 0}, {0xC, 0}}), uint64_t(0));
  CHECK_EQ(PerformanceCoreMask(0xF, {}), uint64_t(0));
}

// Task 276: background pipeline jobs while a level is played.
TEST(PlayWorkersFollowTheSettingAndTheCores) {
  // 0: no limit, as before task 276 (one worker kept for draws).
  CHECK_EQ(PlayBackgroundPipelineWorkers(6, 16, 8, 0), 5u);
  CHECK_EQ(PlayBackgroundPipelineWorkers(1, 4, 2, 0), 1u);
  CHECK_EQ(BackgroundPipelineWorkers(0), 1u);
  // N: at most N, at least 1, never more than the background workers.
  CHECK_EQ(PlayBackgroundPipelineWorkers(6, 16, 8, 2), 2u);
  CHECK_EQ(PlayBackgroundPipelineWorkers(4, 12, 6, 9), 3u);
  // Auto: 8 cores or fewer keep a third of the workers (at least 1); more cores, no limit.
  CHECK_EQ(PlayBackgroundPipelineWorkers(6, 16, 8, -1), 2u);   // 8 cores / 16 threads (handheld class)
  CHECK_EQ(PlayBackgroundPipelineWorkers(6, 8, 8, -1), 2u);    // 8 cores, no SMT
  CHECK_EQ(PlayBackgroundPipelineWorkers(4, 12, 6, -1), 1u);   // 6 cores / 12 threads
  CHECK_EQ(PlayBackgroundPipelineWorkers(2, 4, 4, -1), 1u);
  CHECK_EQ(PlayBackgroundPipelineWorkers(10, 24, 12, -1), 9u); // 12 cores: unchanged
  CHECK_EQ(PlayBackgroundPipelineWorkers(14, 32, 16, -1), 13u);
  CHECK_EQ(PlayBackgroundPipelineWorkers(6, 16, 0, -1), 2u);   // cores not known: half the logical CPUs
  CHECK_EQ(PlayBackgroundPipelineWorkers(10, 24, 0, -1), 9u);
}

TEST(LevelJobsGoAheadInTheirOrder) {
  struct Job {
    int id;
    bool urgent;
    uint32_t level;
  };
  std::deque<Job> jobs{{1, false, 0}, {2, false, 5}, {3, true, 0}, {4, false, 7}, {5, false, 5}, {6, false, 0}};
  const uint32_t tag = 5;
  const auto first = [tag](const Job& j) { return j.urgent || j.level == tag; };
  CHECK_EQ(MoveJobsAhead(jobs, first), size_t(3));
  std::vector<int> order;
  for (const Job& j : jobs) order.push_back(j.id);
  CHECK(order == (std::vector<int>{2, 3, 5, 1, 4, 6}));
  // A late job of the level joins its group; another goes to the back.
  QueueJobBehind(jobs, Job{7, false, 5}, first);
  QueueJobBehind(jobs, Job{8, false, 7}, first);
  order.clear();
  for (const Job& j : jobs) order.push_back(j.id);
  CHECK(order == (std::vector<int>{2, 3, 5, 7, 1, 4, 6, 8}));
  std::deque<Job> empty;
  CHECK_EQ(MoveJobsAhead(empty, first), size_t(0));
  QueueJobBehind(empty, Job{9, false, 5}, first);
  CHECK_EQ(empty.size(), size_t(1));
}

TEST(SetupKeepsOneAllowedLogicalCpuFree) {
  CHECK_EQ(SetupWorkerCount(0), 1u);
  CHECK_EQ(SetupWorkerCount(1), 1u);
  CHECK_EQ(SetupWorkerCount(4), 3u);
  CHECK_EQ(SetupWorkerCount(6), 5u);
  CHECK_EQ(SetupWorkerCount(16), 15u);
}

#ifdef _WIN32
TEST(RestrictedSetupBudgetsAndDemandPriority) {
  DWORD_PTR original = 0, system = 0;
  CHECK(GetProcessAffinityMask(GetCurrentProcess(), &original, &system));
  if (!original) return;
  struct Restore {
    DWORD_PTR mask;
    ~Restore() { SetProcessAffinityMask(GetCurrentProcess(), mask); }
  } restore{original};
  DWORD_PTR mask = 0;
  unsigned count = 0;
  for (unsigned bit = 0; bit < 64; ++bit) {
    if (!(original & (uint64_t(1) << bit))) continue;
    mask |= uint64_t(1) << bit;
    ++count;
    if (count != 1 && count != 4 && count != 6) continue;
    CHECK(SetProcessAffinityMask(GetCurrentProcess(), mask));
    CHECK_EQ(ProcessLogicalProcessorCount(), count);
    CHECK_EQ(ProcessSetupWorkerCount(), SetupWorkerCount(count));
    CHECK((ProcessPerformanceCoreMask() & ~uint64_t(mask)) == 0);
  }
  std::thread([] {
    SchedulePipelineCompiler(false);
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_BELOW_NORMAL);
    SchedulePipelineCompiler(true);
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_NORMAL);
    SchedulePipelineCompiler(false);
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_BELOW_NORMAL);
    ScheduleCriticalThread(true);
    CHECK_EQ(GetThreadPriority(GetCurrentThread()), THREAD_PRIORITY_ABOVE_NORMAL);
  }).join();
}

// Offline setup responsiveness measurement. Synthetic CPU jobs, no game/assets/GPU.
int Benchmark(unsigned logical, bool after) {
  DWORD_PTR allowed = 0, system = 0, mask = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) return 2;
  unsigned found = 0;
  for (unsigned bit = 0; bit < 64 && found < logical; ++bit)
    if (allowed & (uint64_t(1) << bit)) { mask |= uint64_t(1) << bit; ++found; }
  if (found != logical || !SetProcessAffinityMask(GetCurrentProcess(), mask)) return 2;
  timeBeginPeriod(1);
  const unsigned workers = after ? ProcessSetupWorkerCount() :
      SetupWorkerCount(std::max(1u, std::thread::hardware_concurrency()));
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> jobs{0};
  std::vector<std::thread> pool;
  for (unsigned i = 0; i < workers; ++i) pool.emplace_back([&] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // old and new shader workers
    uint64_t state = 123;
    while (!stop.load(std::memory_order_relaxed)) {
      for (int j = 0; j < 100000; ++j) state = state * 6364136223846793005ull + 1;
      jobs.fetch_add(state ? 1 : 0, std::memory_order_relaxed);
    }
  });
  std::vector<double> latency;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  auto next = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < until) {
    next += std::chrono::milliseconds(5);
    std::this_thread::sleep_until(next);
    latency.push_back(std::max(0.0, std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - next).count()));
  }
  stop = true;
  for (auto& worker : pool) worker.join();
  timeEndPeriod(1);
  std::sort(latency.begin(), latency.end());
  auto percentile = [&](double q) { return latency[size_t((latency.size() - 1) * q)]; };
  std::printf("setup-synthetic %s logical=%u mask=%llX workers=%u pipeline_workers=%u "
              "samples=%zu wake_lateness_p50/p95/p99_ms=%.3f/%.3f/%.3f jobs=%llu\n",
      after ? "after" : "before", logical, uint64_t(mask), workers, ProcessPipelineWorkerCount(),
      latency.size(), percentile(.5), percentile(.95), percentile(.99), jobs.load());
  return 0;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  if (argc == 4 && std::strcmp(argv[1], "--bench") == 0)
    return Benchmark(unsigned(std::atoi(argv[2])), std::strcmp(argv[3], "after") == 0);
#endif
  return nr::test::RunAll(argc, argv);
}
