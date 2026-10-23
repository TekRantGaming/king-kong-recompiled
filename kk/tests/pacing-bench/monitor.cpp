#include "benchmark_monitor.h"
#include "benchmark_history.h"
#include "hooks/kk_native_ui.h"
#include "plugin/native_benchmark_stats.h"
#include <iostream>
#include <limits>
#include <cstdlib>

namespace nr::kk { NrEffectStats EffectTimes() { return {}; } }
using namespace kk::benchmark;
using namespace std::chrono_literals;
int failures = 0;
void Check(bool ok, const char* why) { if (!ok) { ++failures; std::cerr << why << '\n'; } }
int main() {
  std::vector<double> frames(1001, 10);
  frames[0] = 50; frames[1] = 100; frames[2] = 200;
  const auto stats = Summarize(frames);
  Check(stats.stutters == 2, "stutters strictly >50 ms");
  Check(std::abs(stats.low01_fps - 1000.0 / 150) < 1e-9, "0.1% ceil includes two slowest frames");
  Check(stats.p50_ms == 10 && stats.p95_ms == 10 && stats.p99_ms == 10, "nearest-rank quantiles");
  MetricMean m; m.Add(10, 1); m.Add(20, 3); m.Add(-1, 100); m.Add(NAN, 1); m.Add(10, 0);
  Check(m.Mean() == 17.5 && m.peak == 20, "time-weighted means ignore unavailable/invalid values");
  Metrics bound;
  Check(std::string(Bound(bound)) == "Timing unavailable", "missing times cannot invent a verdict");
  bound[kGpuMs].Add(8, 1); bound[kGameMs].Add(3, 1); bound[kRenderMs].Add(4, 1);
  Check(std::string(Bound(bound)) == "GPU bound", "GPU indication");
  bound[kGameMs] = {}; bound[kGameMs].Add(12, 1);
  Check(std::string(Bound(bound)) == "CPU bound", "CPU indication uses slower thread");
  bound[kGameMs] = {}; bound[kGameMs].Add(8, 1);
  Check(std::string(Bound(bound)) == "Mixed or capped", "close timings are inconclusive");
  auto old = ReadLegacySummary("Average FPS: 80\n1% low FPS: 40\nSuccessful host present return intervals:\nAverage FPS: 999\nChapter 1 entry 2\nAverage FPS: 60\nHost present returns:\nAverage FPS: 999\nChapter 2 entry 8\nAverage FPS: 55\nRecording settings:\nAverage FPS: 999\n");
  Check(old.Valid() && old.frames[0].average_fps == 80 && old.frames[1].average_fps == 60 && old.frames[2].average_fps == 55, "legacy excludes host and recording data");
  Check(!ReadNumber("nan") && !ReadNumber("inf") && !ReadNumber("10x"), "invalid history numeric fields rejected");
  std::string csv = "scope,count,seconds,average_fps,rest\n";
  for (int c = 0; c < 5; ++c) {
    csv += std::to_string(c) + ",100,1,100,50,25,10,15,20,25,2";
    for (int i = 0; i < kMetricCount; ++i) csv += ",12,15";
    csv += '\n';
  }
  auto history = ReadSummary(csv);
  Check(history.Valid() && history.frames[4].stutters == 2 && history.means[4][kLatency] == 12, "full history metrics and chapters");
  Check(!ReadSummary("scope,count,seconds,average_fps,rest\n0,nan\n").Valid(), "malformed history rejected");
  NrBenchmarkStats native;
  native.size = 1; Check(!nr::ReadBenchmarkStats(&native), "stats size mismatch rejected");
  Monitor monitor;
  monitor.Chapter(0); monitor.GameThread(); monitor.Start();
  for (int i = 0; i < 300; ++i) monitor.Record(10, 5);
  std::this_thread::sleep_for(350ms);
  monitor.Chapter(-1);
  std::this_thread::sleep_for(300ms);
  monitor.Chapter(1);
  for (int i = 0; i < 100; ++i) monitor.Record(20, 7);
  monitor.Stop();
  auto view = monitor.View();
  Check(view.frames.count == 400 && view.dropped == 0, "stop drains frame queue");
  Check(std::abs(view.metrics[1][kLatency].Mean() - 5) < 1e-9 && view.metrics[2][kLatency].Mean() > 6.999, "latency attributed to correct chapters");
  Check(view.metrics[3][kLatency].Mean() < 0 && view.metrics[0][kGpuMs].Mean() < 0, "unavailable chapters/GPU remain absent");
  Check(view.current[kLatency] == 7 && view.current_fps == 50, "current readout uses latest tick and latest sample window");
  Check(view.recent_count == 240 && view.recent[239] == 20 && view.current[kRam] > 0, "scrolling history and real process RAM");
  // Cost of the actual nonblocking producer, with a full queue (worst steady
  // contention is separately observed in game). No sleeps/IO on producer.
  Monitor cost;
  auto began = std::chrono::steady_clock::now();
  for (int i = 0; i < 8192; ++i) cost.Record(10, 5);
  const double accepted_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - began).count() / 8192;
  began = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000000; ++i) cost.Record(10, 5);
  auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - began).count();
  std::cout << "Producer accepted ns/call: " << accepted_ns << "; full queue ns/call: " << elapsed / 1000000 << "; worker measured ms: " << view.worker_ms << '\n';
  std::cout << "Monitor/history checks: " << (failures ? "FAIL" : "PASS") << '\n';
  return failures ? 1 : 0;
}
