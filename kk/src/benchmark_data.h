#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <span>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>

namespace kk::benchmark {
constexpr size_t kMaxSamples = 1000000;

struct Stats {
  size_t count = 0;
  double seconds = 0, average_fps = 0, low_fps = 0, worst_ms = 0, final_fps = 0;
  double low01_fps = 0, p50_ms = 0, p95_ms = 0, p99_ms = 0, stdev_ms = 0;
  size_t stutters = 0;
};

inline Stats Summarize(std::span<const double> samples) {
  std::vector<double> sorted;
  for (double ms : samples) if (std::isfinite(ms) && ms > 0) sorted.push_back(ms);
  Stats s;
  if (sorted.empty()) return s;
  s.count = sorted.size();
  s.final_fps = 1000 / sorted.back();
  const double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
  const double mean = sum / s.count;
  double square = 0;
  for (double ms : sorted) square += (ms - mean) * (ms - mean);
  s.stdev_ms = std::sqrt(square / s.count);
  s.seconds = sum / 1000;
  s.average_fps = 1000 * s.count / sum;
  std::sort(sorted.begin(), sorted.end());
  const size_t low_count = std::max<size_t>(1, (s.count + 99) / 100);
  const double low_sum = std::accumulate(sorted.end() - low_count, sorted.end(), 0.0);
  s.low_fps = 1000 * low_count / low_sum;
  const size_t low01_count = std::max<size_t>(1, (s.count + 999) / 1000);
  s.low01_fps = 1000 * low01_count /
      std::accumulate(sorted.end() - low01_count, sorted.end(), 0.0);
  s.stutters = size_t(sorted.end() - std::upper_bound(sorted.begin(), sorted.end(), 50.0));
  s.worst_ms = sorted.back();
  s.p50_ms = sorted[size_t(std::ceil(s.count * .50)) - 1];
  s.p95_ms = sorted[size_t(std::ceil(s.count * .95)) - 1];
  s.p99_ms = sorted[size_t(std::ceil(s.count * .99)) - 1];
  return s;
}

inline std::string Describe(const Stats& s) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(3)
      << "Samples: " << s.count << "\nMeasured seconds: " << s.seconds
      << "\nAverage FPS: " << s.average_fps << "\n1% low FPS: " << s.low_fps
      << "\nFinal FPS: " << s.final_fps
      << "\n0.1% low FPS: " << s.low01_fps << "\nStutters (>50 ms): " << s.stutters
      << "\nWorst frame ms: " << s.worst_ms << "\nStdev ms: " << s.stdev_ms
      << "\np50 ms: " << s.p50_ms << "\np95 ms: " << s.p95_ms << "\np99 ms: " << s.p99_ms
      << "\np99-p50 ms: " << s.p99_ms - s.p50_ms << '\n';
  return out.str();
}

inline std::string Csv(std::span<const double> samples, const char* series) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(6);
  double elapsed = 0;
  for (size_t i = 0; i < samples.size(); ++i) {
    elapsed += samples[i] / 1000;
    out << series << ',' << i + 1 << ',' << elapsed << ',' << samples[i] << '\n';
  }
  return out.str();
}

// Peak per time bucket: preserves stutters even in a small UI graph.
inline std::vector<float> Graph(std::span<const double> samples, size_t buckets = 300) {
  if (samples.empty() || !buckets) return {};
  const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
  if (total <= 0) return {};
  std::vector<float> points(std::min(buckets, samples.size()), 0);
  double elapsed = 0;
  for (double ms : samples) {
    const size_t i = std::min(points.size() - 1, size_t(elapsed / total * points.size()));
    points[i] = std::max(points[i], float(ms));
    elapsed += ms;
  }
  return points;
}
}  // namespace kk::benchmark
