#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <thread>
#include <vector>
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

inline unsigned PipelineWorkerCount(unsigned logical, unsigned cores) {
  // Leave two physical cores for game/render work. Respect restricted affinity,
  // including processes allowed only one SMT sibling of each core.
  const unsigned available = cores ? std::min(logical, cores) : logical / 2;
  return available > 2 ? available - 2 : 1;
}

inline unsigned ProcessPipelineWorkerCount() {
#ifdef _WIN32
  DWORD_PTR process_mask = 0, system_mask = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) && process_mask) {
    const unsigned logical = std::popcount(uint64_t(process_mask));
    unsigned cores = 0;
    DWORD bytes = 0;
    GetLogicalProcessorInformation(nullptr, &bytes);
    if (bytes) {
      std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> topology(
          (bytes + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) - 1) /
          sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
      if (GetLogicalProcessorInformation(topology.data(), &bytes)) {
        for (size_t i = 0; i < bytes / sizeof(topology[0]); ++i)
          if (topology[i].Relationship == RelationProcessorCore &&
              (topology[i].ProcessorMask & process_mask)) ++cores;
      }
    }
    return cores ? PipelineWorkerCount(logical, cores) : 1;
  }
  // A failed/ambiguous affinity query must not over-subscribe a restricted process.
  return 1;
#else
  return PipelineWorkerCount(std::thread::hardware_concurrency(), 0);
#endif
}

}  // namespace nr
