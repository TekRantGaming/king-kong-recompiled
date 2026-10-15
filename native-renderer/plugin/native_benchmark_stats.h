#pragma once
#include <cstdint>
#include <mutex>
#include <atomic>

// Independent, size-checked interface; does not change the F8 stats ABI.
struct NrBenchmarkStats {
  uint32_t size = sizeof(NrBenchmarkStats);
  uint64_t frame = 0, render_cpu_us = 0;
  double gpu_ms = -1, pipeline_total_ms = 0;
  uint64_t vram_used = 0, vram_budget = 0;
  uint32_t memory_valid = 0;
};
namespace nr {
inline std::atomic<bool> benchmark_stats_enabled{false};
inline std::mutex benchmark_stats_mutex;
inline NrBenchmarkStats benchmark_stats;
inline bool ReadBenchmarkStats(NrBenchmarkStats* out) {
  if (!out || out->size != sizeof(NrBenchmarkStats)) return false;
  std::lock_guard lock(benchmark_stats_mutex);
  *out = benchmark_stats;
  return out->frame != 0;
}
}
