#pragma once
#include "benchmark_monitor.h"
#include <cstdlib>
#include <cstring>
#include <optional>

namespace kk::benchmark {
struct History {
  std::array<Stats, 5> frames{};
  std::array<Values, 5> means{MissingValues(), MissingValues(), MissingValues(), MissingValues(), MissingValues()};
  bool Valid() const { return frames[0].average_fps > 0; }
};
inline std::optional<double> ReadNumber(const std::string& text) {
  char* end = nullptr;
  const double n = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end || !std::isfinite(n)) return {};
  return n;
}
inline History ReadSummary(const std::string& csv) {
  History out;
  std::istringstream lines(csv); std::string line;
  std::getline(lines, line);
  if (!line.starts_with("scope,count,seconds,average_fps,")) return out;
  size_t rows = 0;
  for (size_t row = 0; row < 5 && std::getline(lines, line); ++row) {
    ++rows;
    std::istringstream fields(line); std::string field; std::vector<double> values;
    while (std::getline(fields, field, ',')) {
      if (field.ends_with('\r')) field.pop_back();
      const auto number = ReadNumber(field);
      if (!number) return {};
      values.push_back(*number);
    }
    if ((values.size() != 11 + kMetricCount * 2 && values.size() != 12 + kMetricCount * 2) ||
        values[0] != double(row) || values[1] < 0 || values[10] < 0) return {};
    auto& s = out.frames[row];
    s.count = size_t(std::min(values[1], double(kMaxSamples))); s.seconds = values[2];
    s.average_fps = values[3]; s.low_fps = values[4]; s.low01_fps = values[5];
    s.p50_ms = values[6]; s.p95_ms = values[7]; s.p99_ms = values[8]; s.worst_ms = values[9];
    s.stutters = size_t(std::min(values[10], double(kMaxSamples)));
    for (size_t i = 0; i < kMetricCount; ++i) out.means[row][i] = values[11 + i * 2];
    if (values.size() == 12 + kMetricCount * 2) s.final_fps = values.back();
  }
  return rows == 5 ? out : History{};
}
inline History ReadLegacySummary(const std::string& text) {
  History out;
  std::istringstream file(text); std::string line; size_t scope = 0; bool host = false;
  while (std::getline(file, line)) {
    if (line.starts_with("Recording settings:")) break;
    if (line.starts_with("Chapter ") && line.find(" entry ") != std::string::npos) { ++scope; host = false; }
    if (line.starts_with("Successful host") || line.starts_with("Host present returns:")) host = true;
    if (!host && scope < out.frames.size()) {
      auto& s = out.frames[scope];
      auto number = [&](const char* key, double& v) {
        if (line.starts_with(key)) if (auto n = ReadNumber(line.substr(std::strlen(key)))) v = *n;
      };
      number("Average FPS: ", s.average_fps); number("1% low FPS: ", s.low_fps);
      number("0.1% low FPS: ", s.low01_fps); number("p50 ms: ", s.p50_ms);
      number("p95 ms: ", s.p95_ms); number("p99 ms: ", s.p99_ms); number("Worst frame ms: ", s.worst_ms);
    }
  }
  return out;
}
}
