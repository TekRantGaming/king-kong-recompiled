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
  // Task 211 (the performance summary's worst-frame detail, kk/src/telemetry.cpp): running totals at the
  // frame's Present, so the game can take the difference over a slow frame.
  uint32_t resource_loading = 0;     // the game's level resources are loading
  uint64_t pipeline_waits = 0;       // draws that waited for their pipeline (and got it)
  uint64_t pipelines_made = 0;       // pipelines created
  uint64_t draws_skipped = 0;        // draws skipped, their pipeline not ready
  double pipeline_stutter_ms = 0;    // the draws' time blocked on pipelines (waits and draw-thread creation)
  double pipeline_compile_ms = 0;    // the background workers' pipeline creation time
  uint64_t texture_uploads = 0, upload_bytes = 0;
  double texture_ms = 0, prepare_ms = 0;
  uint64_t pack_textures = 0, pack_bytes = 0;  // texture pack replacements finished, bytes uploaded
  uint64_t gpu_wait_us = 0, submit_us = 0;     // the render thread waiting for the GPU; submitting
  double gpu_ms_total = 0;
  uint64_t gpu_frames = 0;           // frames whose GPU time is in gpu_ms_total
  uint32_t worker_jobs = 0;          // pipeline jobs queued or being made (every 8th frame)
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
