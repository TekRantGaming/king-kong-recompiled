#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits>
#include <locale>
#include <string_view>

namespace kk::benchmark {
// Text format, guest byte order retained verbatim. No ABI-dependent structs on disk.
constexpr unsigned kRecordingVersion = 1;
struct RouteChapter { int entry; double duration; std::string name; };
struct PadSample {
  size_t chapter = 0;
  uint64_t frame = 0;
  double seconds = 0, raw_dt = 0, scaled_dt = 0;
  // Raw bits of the title's clock at 0x82CCAE1C, retained without reinterpretation.
  uint32_t game_ms = 0, user = 0, status = 0;
  std::array<uint8_t, 16> state{};
};
struct Checkpoint {
  size_t chapter = 0;
  double seconds = 0;
  bool valid = false;
  std::array<float, 3> position{};
};
struct Recording {
  std::string build, settings;
  bool complete = false;
  std::vector<RouteChapter> chapters;
  std::vector<PadSample> samples;
  std::vector<Checkpoint> checkpoints;
};
inline std::vector<RouteChapter> DefaultRoute(double duration = 45) {
  return {{2, duration, "Necropolis"}, {8, duration, "V-Rex"},
          {10, duration, "Kong"}, {1, duration, "Skull Island (coast)"}};
}
// A recording route other than the default (kk_benchmark_record_route, task 319): "<entry>:<seconds>"
// items, comma-separated, e.g. "9:180" (chapter-select entry 9, Ann, for 180 game seconds). At most 4
// chapters, entries 0-40, 1-600 seconds (what DecodeRecording accepts). Empty: the default route.
// Throws std::invalid_argument when the text is not such a list.
inline std::vector<RouteChapter> ParseRouteSpec(std::string_view spec, double default_duration = 45) {
  auto bad = []() -> void { throw std::invalid_argument("benchmark route: expected entry:seconds[,entry:seconds]"); };
  auto trim = [](std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return v;
  };
  spec = trim(spec);
  if (spec.empty()) return DefaultRoute(default_duration);
  std::vector<RouteChapter> out;
  while (true) {
    const size_t comma = spec.find(',');
    std::string_view item = trim(spec.substr(0, comma));
    const size_t colon = item.find(':');
    std::string_view entry_text = trim(item.substr(0, colon));
    if (entry_text.empty() || entry_text.size() > 2) bad();
    int entry = 0;
    for (char ch : entry_text) { if (ch < '0' || ch > '9') bad(); entry = entry * 10 + (ch - '0'); }
    double duration = default_duration;
    if (colon != std::string_view::npos) {
      const std::string seconds_text(trim(item.substr(colon + 1)));
      if (seconds_text.empty()) bad();
      std::istringstream in(seconds_text);
      in.imbue(std::locale::classic());
      if (!(in >> duration) || !in.eof()) bad();
    }
    if (entry > 40 || !std::isfinite(duration) || duration < 1 || duration > 600 || out.size() >= 4) bad();
    std::string name = "Chapter select entry " + std::to_string(entry);
    for (const auto& d : DefaultRoute()) if (d.entry == entry) name = d.name;
    if (entry == 9) name = "Ann";
    out.push_back({entry, duration, std::move(name)});
    if (comma == std::string_view::npos) break;
    spec = spec.substr(comma + 1);
  }
  return out;
}
inline std::string EncodeRecording(const Recording& r) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17) << "KKBR " << kRecordingVersion << '\n'
      << "build " << std::quoted(r.build) << '\n'
      << "settings " << std::quoted(r.settings) << '\n'
      << "complete " << r.complete << '\n';
  for (const auto& c : r.chapters)
    out << "chapter " << c.entry << ' ' << c.duration << ' ' << std::quoted(c.name) << '\n';
  for (const auto& s : r.samples) {
    out << "pad " << s.chapter << ' ' << s.frame << ' ' << s.seconds << ' ' << s.raw_dt
        << ' ' << s.scaled_dt << ' ' << s.game_ms << ' ' << s.user << ' ' << s.status;
    for (auto b : s.state) out << ' ' << unsigned(b);
    out << '\n';
  }
  for (const auto& c : r.checkpoints)
    out << "checkpoint " << c.chapter << ' ' << c.seconds << ' ' << c.valid << ' '
        << c.position[0] << ' ' << c.position[1] << ' ' << c.position[2] << '\n';
  out << "end\n";
  return out.str();
}

constexpr size_t kMaxRecordingBytes = 256 * 1024 * 1024;
inline Recording DecodeRecording(std::string_view text) {
  auto fail = []() -> void { throw std::runtime_error("Invalid or incomplete benchmark route"); };
  if (text.size() > kMaxRecordingBytes) fail();
  std::istringstream in{std::string(text)};
  in.imbue(std::locale::classic());
  std::string token;
  unsigned version = 0;
  Recording r;
  if (!(in >> token >> version) || token != "KKBR" || version != kRecordingVersion) fail();
  if (!(in >> token >> std::quoted(r.build)) || token != "build" || r.build.size() > 512) fail();
  if (!(in >> token >> std::quoted(r.settings)) || token != "settings" || r.settings.size() > 65536) fail();
  if (!(in >> token >> r.complete) || token != "complete" || !r.complete) fail();
  bool ended = false, data_started = false;
  while (in >> token) {
    if (token == "end") { ended = true; break; }
    if (token == "chapter") {
      RouteChapter c;
      if (data_started || r.chapters.size() >= 4 || !(in >> c.entry >> c.duration >> std::quoted(c.name)) ||
          c.entry < 0 || c.entry > 40 || !std::isfinite(c.duration) || c.duration < 1 ||
          c.duration > 600 || c.name.size() > 128) fail();
      r.chapters.push_back(std::move(c));
    } else if (token == "pad") {
      data_started = true;
      PadSample s;
      if (r.samples.size() >= 1000000 || !(in >> s.chapter >> s.frame >> s.seconds >> s.raw_dt >>
          s.scaled_dt >> s.game_ms >> s.user >> s.status) || s.chapter >= r.chapters.size() || s.user > 3 ||
          !std::isfinite(s.seconds) || s.seconds < 0 || s.seconds > r.chapters[s.chapter].duration + 1 ||
          !std::isfinite(s.raw_dt) || s.raw_dt < 0 || s.raw_dt > 1 ||
          !std::isfinite(s.scaled_dt) || s.scaled_dt < 0 || s.scaled_dt > 100) fail();
      for (auto& b : s.state) { unsigned n; if (!(in >> n) || n > 255) fail(); b = uint8_t(n); }
      if (!r.samples.empty()) {
        const auto& last = r.samples.back();
        if (s.chapter < last.chapter || (s.chapter == last.chapter &&
            (s.seconds < last.seconds || s.frame < last.frame))) fail();
      }
      r.samples.push_back(s);
    } else if (token == "checkpoint") {
      data_started = true;
      Checkpoint c;
      if (r.checkpoints.size() >= 10000 || !(in >> c.chapter >> c.seconds >> c.valid >>
          c.position[0] >> c.position[1] >> c.position[2]) || c.chapter >= r.chapters.size() ||
          !std::isfinite(c.seconds) || c.seconds < 0 || c.seconds > r.chapters[c.chapter].duration + 1) fail();
      for (float v : c.position) if (!std::isfinite(v)) fail();
      if (!r.checkpoints.empty()) {
        const auto& last = r.checkpoints.back();
        if (c.chapter < last.chapter || (c.chapter == last.chapter && c.seconds < last.seconds)) fail();
      }
      r.checkpoints.push_back(c);
    } else fail();
  }
  if (!ended || (in >> token) || r.chapters.empty()) fail();
  for (size_t chapter = 0; chapter < r.chapters.size(); ++chapter) {
    bool connected = false, checkpoint = false;
    double first = std::numeric_limits<double>::infinity(), last = 0;
    for (const auto& s : r.samples) if (s.chapter == chapter && s.user == 0) {
      connected |= s.status == 0; first = std::min(first, s.seconds); last = s.seconds;
    }
    for (const auto& c : r.checkpoints) checkpoint |= c.chapter == chapter;
    if (!connected || first > .25 || last < r.chapters[chapter].duration - .25 || !checkpoint) fail();
  }
  return r;
}

inline std::string RecordingId(std::string_view bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : bytes) hash = (hash ^ c) * 1099511628211ull;
  std::ostringstream out; out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

inline uint32_t DigitalBits(const PadSample& s) {
  if (s.status) return 0;
  return (uint32_t(s.state[4]) << 8 | s.state[5]) |
      (s.state[6] > 30 ? 1u << 16 : 0) | (s.state[7] > 30 ? 1u << 17 : 0);
}

// Frame() timestamps reads/checkpoints after adding dt, but runs before that
// frame's simulation. A frame-zero read has not begun an interval yet.
inline double SampleStartSeconds(const PadSample& s) {
  return s.frame ? std::max(0.0, s.seconds - s.raw_dt) : s.seconds;
}

inline std::vector<double> CheckpointStartSeconds(const Recording& r) {
  std::vector<double> times(r.checkpoints.size(), std::numeric_limits<double>::quiet_NaN());
  size_t sample = 0;
  for (size_t i = 0; i < r.checkpoints.size(); ++i) {
    const auto& c = r.checkpoints[i];
    while (sample < r.samples.size() && (r.samples[sample].chapter < c.chapter ||
        (r.samples[sample].chapter == c.chapter && r.samples[sample].seconds < c.seconds - 1e-9))) ++sample;
    for (size_t j = sample; j < r.samples.size() && r.samples[j].chapter == c.chapter &&
        std::abs(r.samples[j].seconds - c.seconds) < 1e-9; ++j) {
      if (r.samples[j].user == 0) { times[i] = SampleStartSeconds(r.samples[j]); break; }
    }
  }
  return times;
}

inline std::array<float, 3> PositionAt(const std::array<float, 3>& previous,
    double previous_seconds, const std::array<float, 3>& current, double current_seconds, double target) {
  if (current_seconds <= previous_seconds) return current;
  const double fraction = std::clamp((target - previous_seconds) / (current_seconds - previous_seconds), 0.0, 1.0);
  std::array<float, 3> position;
  for (size_t i = 0; i < 3; ++i) position[i] = float(previous[i] + fraction * (double(current[i]) - previous[i]));
  return position;
}

// Zero-order hold on the game's elapsed seconds, never wall frames or a fixed
// simulation step. Keep every recorded read; report digital transitions that
// cannot be represented by one state at a replay poll.
class RouteReplay {
 public:
  void Attach(const Recording& r) {
    recording_ = &r;
    indices_.clear(); indices_.resize(r.chapters.size());
    for (size_t i = 0; i < r.samples.size(); ++i)
      indices_[r.samples[i].chapter][r.samples[i].user].push_back(i);
    Reset(0);
  }
  void Reset(size_t chapter) { chapter_ = chapter; next_.fill(0); selected_.fill(nullptr); }
  const PadSample* Read(uint32_t user, double seconds, uint32_t& missed, bool before_first_frame = false) {
    missed = 0;
    if (!recording_ || user > 3 || chapter_ >= indices_.size()) return nullptr;
    const auto& rows = indices_[chapter_][user];
    uint32_t previous = selected_[user] ? DigitalBits(*selected_[user]) : 0, changed = 0;
    while (next_[user] < rows.size()) {
      const auto& s = recording_->samples[rows[next_[user]]];
      if ((before_first_frame && s.frame) || SampleStartSeconds(s) > seconds + 1e-9) break;
      const uint32_t bits = DigitalBits(s);
      const uint32_t edges = bits ^ previous;
      missed |= changed & edges;
      changed |= edges;
      previous = bits; selected_[user] = &s; ++next_[user];
    }
    return selected_[user];
  }
 private:
  const Recording* recording_ = nullptr;
  std::vector<std::array<std::vector<size_t>, 4>> indices_;
  size_t chapter_ = 0;
  std::array<size_t, 4> next_{};
  std::array<const PadSample*, 4> selected_{};
};

inline double PositionDistance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  double squared = 0;
  for (size_t i = 0; i < 3; ++i) {
    if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return std::numeric_limits<double>::infinity();
    const double delta = double(a[i]) - b[i]; squared += delta * delta;
  }
  return std::sqrt(squared);
}
}  // namespace kk::benchmark
