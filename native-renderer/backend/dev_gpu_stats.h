#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace nr {
// Actual NVRHI command emissions, excluding SDK calls made directly outside
// NVRHI. Enabled only by the native_debug profiling bit; no per-draw logging.
enum class DevGpuCounter : uint32_t { Pipeline, Viewport, Scissor, Blend, Stencil, ConstantBinding, Count };
inline std::atomic<bool> g_dev_gpu_stats_enabled{false};
inline std::array<std::atomic<uint64_t>, uint32_t(DevGpuCounter::Count)> g_dev_gpu_stats{};
inline void CountGpuCommand(DevGpuCounter counter) {
  if (g_dev_gpu_stats_enabled.load(std::memory_order_relaxed))
    g_dev_gpu_stats[uint32_t(counter)].fetch_add(1, std::memory_order_relaxed);
}
inline std::array<uint64_t, uint32_t(DevGpuCounter::Count)> GpuCommandStats() {
  std::array<uint64_t, uint32_t(DevGpuCounter::Count)> values{};
  for (std::size_t i = 0; i < values.size(); ++i) values[i] = g_dev_gpu_stats[i].load(std::memory_order_relaxed);
  return values;
}
}  // namespace nr
