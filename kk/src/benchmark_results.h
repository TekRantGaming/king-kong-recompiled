// Benchmark passes and their summary: shared by the game (which measures and
// writes them) and the launcher (which reads and shows them). No game, window
// or file system access here, so the tests can check every rule offline.
#pragma once

#include "benchmark_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace kk::benchmark {

constexpr int kMaxPasses = 5;
constexpr size_t kRouteChapters = 4;
// Game seconds at the start of each chapter that are replayed but not measured:
// the fade in from loading, streaming and the first sight of the scene.
constexpr double kDefaultSettleSeconds = 5;
// Average CPU use outside the game (percent of all logical processors; this includes
// system and driver time, about 10-20% while the game runs on its own), or
// the busiest GPU engine's use by other processes, above which a pass is
// marked as disturbed by background activity.
constexpr double kBusyOtherCpu = 30;
constexpr double kBusyOtherGpu = 15;

// One chapter (or the whole route, chapter = 0) of one pass.
struct PassScope {
  int pass = 0;      // 1-based
  int chapter = 0;   // 0: the whole pass, 1..4 the route's chapters
  Stats frames;
  double other_cpu = -1, other_gpu = -1;  // -1: not available
  // Diagnostics (means over the pass's measured time; -1: not available).
  double gpu_clock = -1, gpu_temp = -1, gpu_ms = -1, game_ms = -1, render_ms = -1;
  bool Busy() const { return other_cpu > kBusyOtherCpu || other_gpu > kBusyOtherGpu; }
};

enum class Confidence { kUnknown, kHigh, kMedium, kLow };

inline double Median(std::vector<double> values) {
  values.erase(std::remove_if(values.begin(), values.end(), [](double v) { return !std::isfinite(v) || v <= 0; }),
               values.end());
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const size_t n = values.size();
  return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

// (max - min) / median, in percent; 0 with fewer than two values.
inline double SpreadPercent(const std::vector<double>& values) {
  std::vector<double> v;
  for (double x : values) if (std::isfinite(x) && x > 0) v.push_back(x);
  if (v.size() < 2) return 0;
  const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
  const double median = Median(v);
  return median > 0 ? (*hi - *lo) / median * 100 : 0;
}

inline Confidence ConfidenceFor(size_t passes, double spread_percent) {
  if (passes < 2) return Confidence::kUnknown;
  if (spread_percent <= 3) return Confidence::kHigh;
  if (spread_percent <= 6) return Confidence::kMedium;
  return Confidence::kLow;
}

inline const char* ConfidenceLabel(Confidence c) {
  switch (c) {
    case Confidence::kHigh: return "High";
    case Confidence::kMedium: return "Medium";
    case Confidence::kLow: return "Low";
    default: return "One pass only";
  }
}

// The result of one scope over every pass: the median of each figure, and how
// far the passes' average FPS spread.
struct ScopeSummary {
  Stats median;
  size_t passes = 0;
  double spread = 0;  // percent
  double min_fps = 0, max_fps = 0;
  size_t busy_passes = 0;
  Confidence confidence = Confidence::kUnknown;
};

inline ScopeSummary SummarizePasses(const std::vector<PassScope>& rows, int chapter) {
  ScopeSummary out;
  std::vector<double> avg, low, low01, p50, p95, p99, worst, seconds, count, stutters, final_fps;
  for (const auto& r : rows) {
    if (r.chapter != chapter || r.frames.count == 0) continue;
    ++out.passes;
    if (r.Busy()) ++out.busy_passes;
    avg.push_back(r.frames.average_fps); low.push_back(r.frames.low_fps); low01.push_back(r.frames.low01_fps);
    p50.push_back(r.frames.p50_ms); p95.push_back(r.frames.p95_ms); p99.push_back(r.frames.p99_ms);
    worst.push_back(r.frames.worst_ms); seconds.push_back(r.frames.seconds); count.push_back(double(r.frames.count));
    stutters.push_back(double(r.frames.stutters) + 1);  // +1: Median ignores zeros
    final_fps.push_back(r.frames.final_fps);
  }
  if (!out.passes) return out;
  auto& s = out.median;
  s.average_fps = Median(avg); s.low_fps = Median(low); s.low01_fps = Median(low01);
  s.p50_ms = Median(p50); s.p95_ms = Median(p95); s.p99_ms = Median(p99); s.worst_ms = Median(worst);
  s.seconds = Median(seconds); s.count = size_t(Median(count)); s.final_fps = Median(final_fps);
  s.stutters = size_t(std::max(0.0, Median(stutters) - 1));
  out.spread = SpreadPercent(avg);
  out.min_fps = *std::min_element(avg.begin(), avg.end());
  out.max_fps = *std::max_element(avg.begin(), avg.end());
  out.confidence = ConfidenceFor(out.passes, out.spread);
  return out;
}

// passes.csv: one row per pass and scope.
inline std::string PassesCsv(const std::vector<PassScope>& rows) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "pass,scope,count,seconds,average_fps,low1_fps,low01_fps,p50_ms,p95_ms,p99_ms,worst_ms,stutters,"
         "other_cpu_percent,other_gpu_percent,gpu_clock_mhz,gpu_temp_c,gpu_frame_ms,game_cpu_ms,render_cpu_ms\n";
  out << std::setprecision(10);
  for (const auto& r : rows) {
    const auto& s = r.frames;
    out << r.pass << ',' << r.chapter << ',' << s.count << ',' << s.seconds << ',' << s.average_fps << ','
        << s.low_fps << ',' << s.low01_fps << ',' << s.p50_ms << ',' << s.p95_ms << ',' << s.p99_ms << ','
        << s.worst_ms << ',' << s.stutters << ',' << r.other_cpu << ',' << r.other_gpu << ',' << r.gpu_clock << ','
        << r.gpu_temp << ',' << r.gpu_ms << ',' << r.game_ms << ',' << r.render_ms << '\n';
  }
  return out.str();
}

inline std::vector<PassScope> ReadPassesCsv(const std::string& csv) {
  std::vector<PassScope> rows;
  std::istringstream lines(csv);
  std::string line;
  if (!std::getline(lines, line) || !line.starts_with("pass,scope,count,seconds,average_fps,")) return {};
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::istringstream fields(line);
    std::vector<double> v;
    for (std::string f; std::getline(fields, f, ',');) {
      char* end = nullptr;
      const double n = std::strtod(f.c_str(), &end);
      if (end == f.c_str() || *end || !std::isfinite(n)) return {};
      v.push_back(n);
    }
    if ((v.size() != 14 && v.size() != 19) || v[0] < 1 || v[0] > kMaxPasses || v[1] < 0 || v[1] > double(kRouteChapters) || v[2] < 0 ||
        rows.size() >= size_t(kMaxPasses) * (kRouteChapters + 1)) return {};
    PassScope r;
    r.pass = int(v[0]); r.chapter = int(v[1]);
    auto& s = r.frames;
    s.count = size_t(std::min(v[2], double(kMaxSamples))); s.seconds = v[3]; s.average_fps = v[4];
    s.low_fps = v[5]; s.low01_fps = v[6]; s.p50_ms = v[7]; s.p95_ms = v[8]; s.p99_ms = v[9]; s.worst_ms = v[10];
    s.stutters = size_t(std::clamp(v[11], 0.0, double(kMaxSamples)));
    r.other_cpu = v[12]; r.other_gpu = v[13];
    if (v.size() == 19) { r.gpu_clock = v[14]; r.gpu_temp = v[15]; r.gpu_ms = v[16]; r.game_ms = v[17]; r.render_ms = v[18]; }
    rows.push_back(r);
  }
  return rows;
}

// A pad press that cancels the run (B, Back or Start on the player's own
// controller). A button held when the run starts must be released first.
struct CancelPress {
  static constexpr uint16_t kButtons = 0x2000 | 0x0020 | 0x0010;  // B, Back, Start (XInput)
  bool armed = false;
  bool Update(uint16_t buttons) {
    if (!(buttons & kButtons)) { armed = true; return false; }
    return armed;
  }
};

}  // namespace kk::benchmark
