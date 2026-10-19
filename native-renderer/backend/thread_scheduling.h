#pragma once

#include <algorithm>
#include <atomic>
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

struct SchedulingCore {
  uint64_t mask = 0;
  unsigned efficiency_class = 0;
};

// A zero mask means leave placement to Windows. Never escape process affinity,
// or force both critical threads onto a single performance core.
inline uint64_t PerformanceCoreMask(uint64_t allowed, const std::vector<SchedulingCore>& cores) {
  unsigned lowest = ~0u, highest = 0;
  for (const auto& core : cores) {
    if (!(core.mask & allowed)) continue;
    lowest = std::min(lowest, core.efficiency_class);
    highest = std::max(highest, core.efficiency_class);
  }
  if (lowest == ~0u || lowest == highest) return 0;
  uint64_t mask = 0;
  unsigned count = 0;
  for (const auto& core : cores) {
    if (core.efficiency_class == highest && (core.mask & allowed)) {
      mask |= core.mask & allowed;
      ++count;
    }
  }
  return count >= 2 ? mask : 0;
}

inline unsigned SetupWorkerCount(unsigned logical) { return logical > 1 ? logical - 1 : 1; }

inline unsigned ProcessLogicalProcessorCount() {
#ifdef _WIN32
  DWORD_PTR allowed = 0, system = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) && allowed)
    return std::popcount(uint64_t(allowed));
  return 1;  // ambiguous processor groups or failed query: do not oversubscribe
#else
  return std::max(1u, std::thread::hardware_concurrency());
#endif
}

inline unsigned ProcessSetupWorkerCount() { return SetupWorkerCount(ProcessLogicalProcessorCount()); }

#ifdef _WIN32
inline uint64_t ProcessPerformanceCoreMask() {
  USHORT groups = 1, group = 0;
  if (!GetProcessGroupAffinity(GetCurrentProcess(), &groups, &group) || groups != 1) return 0;
  DWORD_PTR allowed = 0, system = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) || !allowed) return 0;
  DWORD bytes = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
  std::vector<uint8_t> buffer(bytes);
  if (!bytes || !GetLogicalProcessorInformationEx(RelationProcessorCore,
      reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &bytes)) return 0;
  std::vector<SchedulingCore> cores;
  for (DWORD offset = 0; offset < bytes;) {
    const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
    if (info->Size < sizeof(DWORD) * 2 || info->Size > bytes - offset) return 0;
    if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount == 1 &&
        info->Processor.GroupMask[0].Group == group)
      cores.push_back({uint64_t(info->Processor.GroupMask[0].Mask), info->Processor.EfficiencyClass});
    offset += info->Size;
  }
  return PerformanceCoreMask(uint64_t(allowed), cores);
}

inline bool SetThreadEcoQos(bool background) {
  THREAD_POWER_THROTTLING_STATE power{};
  power.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  power.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  power.StateMask = background ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
  return SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &power, sizeof(power)) != 0;
}
#endif

// Task 295 (kk_cpu_priority): how much the game's critical threads (game, render, vsync, ring skimmer,
// audio) are favoured over other programs' and our own background threads under CPU load.
//   0: before task 295 (normal priority; audio above normal).
//   1: critical threads above normal, audio highest; a draw's pipeline compile above normal.
//   2: 1 + MMCSS "Games" ("Audio" for audio): Windows' multimedia scheduler, which keeps 20 % for the
//      rest of the system (no realtime class).
//   3: 2 + the process in the ABOVE_NORMAL class.
// One value per binary (the exe and the renderer DLL each set it before their threads start).
inline std::atomic<int>& CpuPriorityLevel() {
  static std::atomic<int> level{0};
  return level;
}
inline void SetCpuPriorityLevel(int level) {
  CpuPriorityLevel().store(std::clamp(level, 0, 3), std::memory_order_relaxed);
#ifdef _WIN32
  if (level >= 3) SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#endif
}

#ifdef _WIN32
// MMCSS for the calling thread for its lifetime; false when avrt.dll or the task is unavailable.
inline bool JoinMmcssTask(const wchar_t* task) {
  static HMODULE avrt = LoadLibraryW(L"avrt.dll");
  if (!avrt) return false;
  using Fn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
  static const auto join = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")));
  DWORD index = 0;
  return join && join(task, &index) != nullptr;
}
#endif

// Call once on the actual worker, after the SDK has started it. Homogeneous CPUs
// retain the OS's placement; hybrid CPUs use all allowed performance cores.
inline void ScheduleCriticalThread(bool audio = false) {
#ifdef _WIN32
  SetThreadEcoQos(false);
  if (const uint64_t mask = ProcessPerformanceCoreMask())
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(mask));
  const int level = CpuPriorityLevel().load(std::memory_order_relaxed);
  int priority = audio ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL;
  if (level >= 1) priority = audio ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL;
  if (GetThreadPriority(GetCurrentThread()) < priority)
    SetThreadPriority(GetCurrentThread(), priority);
  if (level >= 2) JoinMmcssTask(audio ? L"Audio" : L"Games");
#endif
}

// Demand work must not inherit background EcoQoS/priority when a draw waits for it.
inline void SchedulePipelineCompiler(bool urgent) {
#ifdef _WIN32
  const int demand = CpuPriorityLevel().load(std::memory_order_relaxed) >= 1 ? THREAD_PRIORITY_ABOVE_NORMAL
                                                                             : THREAD_PRIORITY_NORMAL;
  SetThreadPriority(GetCurrentThread(), urgent ? demand : THREAD_PRIORITY_BELOW_NORMAL);
  SetThreadEcoQos(!urgent);
#endif
}

}  // namespace nr
