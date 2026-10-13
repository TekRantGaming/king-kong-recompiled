#pragma once
#include <cstdint>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nr {
// CPU execution time, excluding sleeps and GPU/queue waits. Zero on unsupported hosts.
inline uint64_t CurrentThreadCpuMicroseconds() {
#if defined(_WIN32)
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return (ticks(kernel) + ticks(user)) / 10;
#else
  return 0;
#endif
}
}  // namespace nr
