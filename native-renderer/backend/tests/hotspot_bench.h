// CPU-only micro-benchmarks. Call with --bench for five interleaved old/new measurements.
#pragma once
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(_MSC_VER)
#define NR_NOINLINE __declspec(noinline)
#else
#define NR_NOINLINE __attribute__((noinline))
#endif

namespace nr::test {
inline volatile uint64_t bench_sink = 0;
inline bool BenchmarkRequested(int argc, char** argv) {
  return argc > 1 && std::strcmp(argv[1], "--bench") == 0;
}
template<class Old, class New>
void Benchmark(const char* name, size_t calls, Old old, New next) {
#ifdef _WIN32
  DWORD_PTR process_mask = 0, system_mask = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
  const DWORD_PTR saved = process_mask ? SetThreadAffinityMask(GetCurrentThread(), process_mask & (~process_mask + 1)) : 0;
#endif
  bench_sink = old(); bench_sink = next();  // warm code and data, outside measurement
  for (unsigned round = 0; round < 5; ++round) {
    for (unsigned variant = 0; variant < 2; ++variant) {
      const auto start = std::chrono::steady_clock::now();
      const uint64_t sum = variant ? next() : old();
      const auto end = std::chrono::steady_clock::now();
      bench_sink = sum;
      const double ns = std::chrono::duration<double, std::nano>(end - start).count() / double(calls);
      std::printf("BENCH %s %s %u %.3f ns/call checksum=%llu\n", name, variant ? "new" : "old", round + 1,
                  ns, static_cast<unsigned long long>(sum));
      std::fflush(stdout);
    }
  }
#ifdef _WIN32
  if (saved) SetThreadAffinityMask(GetCurrentThread(), saved);
#endif
}
}  // namespace nr::test
