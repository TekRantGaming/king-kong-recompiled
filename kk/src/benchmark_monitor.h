#pragma once
#include "benchmark_data.h"
#include "benchmark_results.h"
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace kk::benchmark {
enum Metric { kGpuUse, kCpuUse, kVram, kBudget, kRam, kSystemRam, kGpuMs, kGameMs,
  kRenderMs, kPipelineMs, kLatency, kOtherCpu, kOtherGpu, kClock, kTemperature, kPower, kMetricCount };
inline constexpr const char* kMetricNames[] = {
  "GPU busiest engine (%)", "Process CPU (%)", "VRAM used (MiB)", "VRAM budget (MiB)",
  "Process RAM (MiB)", "System RAM used (MiB)", "GPU frame (ms)", "Game thread CPU (ms)",
  "Render thread CPU (ms)", "Pipeline wait (ms)", "Game tick age at swap (ms)",
  "Other programs CPU (%)", "Other programs GPU (%)",
  "GPU clock (MHz)", "GPU temperature (C)", "GPU power (W)"};
using Values = std::array<double, kMetricCount>;
inline constexpr Metric kPassMetrics[] = {kOtherCpu, kOtherGpu, kClock, kTemperature, kGpuMs, kGameMs, kRenderMs};
inline constexpr size_t kPassMetricCount = std::size(kPassMetrics);
inline Values MissingValues() { Values v; v.fill(-1); return v; }
struct MetricMean {
  double sum = 0, weight = 0, peak = -1;
  void Add(double value, double seconds) {
    if (!std::isfinite(value) || value < 0 || seconds <= 0) return;
    sum += value * seconds; weight += seconds; peak = std::max(peak, value);
  }
  double Mean() const { return weight > 0 ? sum / weight : -1; }
};
using Metrics = std::array<MetricMean, kMetricCount>;
inline const char* Bound(const Metrics& m, double interval_ms = -1) {
  const double gpu = m[kGpuMs].Mean(), game = m[kGameMs].Mean(), render = m[kRenderMs].Mean();
  if (gpu < 0 || game < 0 || render < 0) return "Timing unavailable";
  const double cpu = std::max(game, render);
  if (interval_ms > 0 && std::max(cpu, gpu) < interval_ms * .75) return "Mixed or cap-limited";
  if (gpu > cpu * 1.15) return "GPU-bound";
  if (cpu > gpu * 1.15) return "CPU-bound";
  return "Mixed or cap-limited";
}
struct MonitorView {
  Stats frames;
  double current_fps = 0;
  Values current = MissingValues();
  std::array<float, 240> recent{};
  size_t recent_count = 0;
  std::array<Metrics, 5> metrics{}; // total, then four chapters (every pass)
  // Per pass and chapter (pass * 4 + chapter): other programs' CPU and GPU use,
  // GPU clock and temperature, GPU frame time and the game/render threads' CPU time.
  std::array<std::array<MetricMean, kPassMetricCount>, kMaxPasses * kRouteChapters> background{};
  uint32_t width = 0, height = 0, output_width = 0, output_height = 0, api = 0;
  std::string upscaler;
  uint64_t dropped = 0, samples = 0;
  double worker_ms = 0;
};
class Monitor {
 public:
  ~Monitor() { Stop(); }
  void Start();
  void Stop();
  // -1: not measuring (loading, settling). Pass is 0-based.
  void Chapter(int chapter, int pass = 0) { pass_.store(pass); chapter_.store(chapter); }
  void GameThread();
  void Record(double ms, double latency);
  MonitorView View() const { std::lock_guard lock(view_mutex_); return view_; }
 private:
  struct Frame { double ms, latency; int chapter; };
  static constexpr size_t kQueue = 8192;
  std::array<Frame, kQueue> queue_{};
  std::atomic<size_t> write_{0}, read_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<int> chapter_{-1}, pass_{0};
  std::atomic<void*> game_thread_{nullptr};
  mutable std::mutex view_mutex_;
  MonitorView view_;
  std::mutex stop_mutex_;
  std::condition_variable stop_cv_;
  bool stop_ = false;
  std::thread worker_;
  void Run();
};
}
