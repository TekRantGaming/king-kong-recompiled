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

inline unsigned PipelineWorkerCount(unsigned logical, unsigned cores) {
  // Keep the existing modest parallelism, and reserve a physical core for
  // game work whenever the topology is available. One worker is the minimum.
  const unsigned budget = cores == 0 ? 4 : (cores > 1 ? cores - 1 : 1);
  return std::min(std::clamp(logical / 4, 1u, 4u), budget);
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
