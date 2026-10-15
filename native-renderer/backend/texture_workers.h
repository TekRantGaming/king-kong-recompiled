#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace nr {

inline unsigned TextureWorkerCount() {
  unsigned logical = 0;
#ifdef _WIN32
  DWORD_PTR process_mask = 0, system_mask = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
    logical = std::popcount(uint64_t(process_mask));
#elif defined(__linux__)
  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  if (!sched_getaffinity(0, sizeof(affinity), &affinity)) logical = unsigned(CPU_COUNT(&affinity));
#else
  logical = std::thread::hardware_concurrency();
#endif
  // This caller already occupies a CPU. Leave two logical CPUs for game/pipeline work, and never start
  // workers when a restricted/unknown affinity would make the additional parallelism unhelpful.
  return logical > 3 ? std::min(3u, logical - 3) : 0;
}

}  // namespace nr
