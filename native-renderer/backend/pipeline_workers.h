#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <thread>
#include <vector>
#include "backend/thread_scheduling.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nr {

// Called with the queue mutex held. Active work is absent from the queue and is never duplicated.
template <class Queue>
bool PromotePipelineJob(Queue& jobs, uint64_t key) {
  auto queued = std::find_if(jobs.begin(), jobs.end(),
                            [key](const auto& job) { return job.key == key; });
  if (queued == jobs.end()) return false;
  queued->urgent = true;
  std::rotate(jobs.begin(), queued, std::next(queued));
  return true;
}

// Task 276: moves the jobs `first` picks ahead of the others, both groups in their order (the urgent ones a
// draw asked for, then a level's prewarm, then the rest). Called with the queue mutex held. Returns the
// jobs now ahead.
template <class Queue, class Pred>
size_t MoveJobsAhead(Queue& jobs, Pred first) {
  const auto split = std::stable_partition(jobs.begin(), jobs.end(), first);
  return size_t(std::distance(jobs.begin(), split));
}

// Task 276: queues `job` behind the leading jobs `first` picks (a level's job joins its level's group, behind
// the urgent ones and ahead of the rest); a job `first` does not pick goes to the back.
template <class Queue, class Job, class Pred>
void QueueJobBehind(Queue& jobs, Job&& job, Pred first) {
  if (!first(job)) {
    jobs.push_back(std::forward<Job>(job));
    return;
  }
  auto end = std::find_if_not(jobs.begin(), jobs.end(), first);
  jobs.insert(end, std::forward<Job>(job));
}

// The prewarm keeps one compiler free for draws' jobs while it has a backlog: the jobs it may run at once.
inline unsigned BackgroundPipelineWorkers(unsigned workers) { return workers > 1 ? workers - 1 : 1; }

// Task 276: the background (prewarm) jobs that may run at once while a chapter is being played, so the game
// thread keeps its CPU on a PC with few cores. Draws' jobs always take any worker. `setting`
// (--native_pipeline_play_workers): 0 = as in menus and loading (no limit, the behaviour before task 276);
// N > 0 = at most N; -1 = auto: PCs with kFewPipelineCores physical cores or fewer (0 = not known: half the
// logical CPUs) keep a third of the workers, at least 1; more cores, no limit. Never chosen by GPU vendor.
constexpr unsigned kFewPipelineCores = 8;
inline unsigned PlayBackgroundPipelineWorkers(unsigned workers, unsigned logical, unsigned cores, int setting) {
  const unsigned background = BackgroundPipelineWorkers(workers);
  if (setting == 0) return background;
  if (setting > 0) return std::clamp(unsigned(setting), 1u, background);
  const unsigned physical = cores ? cores : logical / 2;
  if (physical > kFewPipelineCores) return background;
  return std::clamp(workers / 3, 1u, background);
}

inline unsigned PipelineWorkerCount(unsigned logical, unsigned cores) {
  // Leave two physical cores for game/render work. Respect restricted affinity,
  // including processes allowed only one SMT sibling of each core.
  const unsigned available = cores ? std::min(logical, cores) : logical / 2;
  return available > 2 ? available - 2 : 1;
}

struct ProcessCpus {
  unsigned logical = 0;  // logical CPUs the process may run on
  unsigned cores = 0;    // physical cores among them (0 = not known)
};

inline ProcessCpus ProcessCpuCounts() {
  ProcessCpus cpus;
#ifdef _WIN32
  DWORD_PTR process_mask = 0, system_mask = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask) {
    cpus.logical = std::popcount(uint64_t(process_mask));
    DWORD bytes = 0;
    GetLogicalProcessorInformation(nullptr, &bytes);
    if (bytes) {
      std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> topology(
          (bytes + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) - 1) /
          sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
      if (GetLogicalProcessorInformation(topology.data(), &bytes)) {
        for (size_t i = 0; i < bytes / sizeof(topology[0]); ++i)
          if (topology[i].Relationship == RelationProcessorCore &&
              (topology[i].ProcessorMask & process_mask)) ++cpus.cores;
      }
    }
  }
#else
  cpus.logical = std::thread::hardware_concurrency();
#endif
  return cpus;
}

inline unsigned ProcessPipelineWorkerCount() {
#ifdef _WIN32
  const ProcessCpus cpus = ProcessCpuCounts();
  // A failed/ambiguous affinity query must not over-subscribe a restricted process.
  return cpus.logical && cpus.cores ? PipelineWorkerCount(cpus.logical, cpus.cores) : 1;
#else
  return PipelineWorkerCount(std::thread::hardware_concurrency(), 0);
#endif
}

}  // namespace nr
