#include "benchmark_route.h"
#include "benchmark_data.h"

#include <fstream>
#include <iostream>
#include <iterator>

using namespace kk::benchmark;
int failures = 0;
void Check(bool ok, const char* label) {
  if (!ok) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}
template<class Change> void Reject(const Recording& source, Change change, const char* label) {
  auto r = source;
  change(r);
  try { DecodeRecording(EncodeRecording(r)); Check(false, label); }
  catch (const std::exception&) {}
}

Recording Fixture() {
  Recording r;
  r.complete = true;
  r.build = "synthetic build \"115\"";
  r.settings = "native_backend = vulkan\nkk_frame_rate = 60\n";
  r.chapters = DefaultRoute(4);
  for (size_t c = 0; c < r.chapters.size(); ++c) {
    for (uint64_t frame = 0; frame <= 240; ++frame) {
      for (uint32_t user = 0; user < 4; ++user) {
        PadSample s;
        s.chapter = c; s.frame = frame; s.seconds = frame / 60.0;
        s.raw_dt = 1.0 / 60; s.scaled_dt = s.raw_dt * .5;
        s.game_ms = uint32_t(frame * 1000 / 60); s.user = user;
        s.status = user == 0 ? 0 : 1167;
        if (!s.status) {
          for (size_t b = 0; b < s.state.size(); ++b) s.state[b] = uint8_t(frame + b);
        }
        r.samples.push_back(s);
      }
    }
    for (int second = 0; second <= 4; ++second)
      r.checkpoints.push_back({c, double(second), c != 2, {float(second), 2, -3}});
  }
  return r;
}

void FormatTests() {
  const auto defaults = DefaultRoute();
  Check(defaults.size() == 4 && defaults[0].entry == 2 && defaults[1].entry == 8 &&
      defaults[2].entry == 10 && defaults[3].entry == 1, "four varied chapter entries retained");
  double total = 0;
  for (const auto& c : defaults) { Check(c.duration == 45, "45 game seconds per chapter"); total += c.duration; }
  Check(total == 180, "three game minutes plus excluded loading");
  const auto r = Fixture();
  const auto text = EncodeRecording(r);
  const auto decoded = DecodeRecording(text);
  Check(EncodeRecording(decoded) == text, "lossless pads, status, clocks, settings, build and checkpoints");
  Check(decoded.samples[48].state == r.samples[48].state && decoded.samples[49].status == 1167,
      "all 16 guest bytes and disconnected users survive");
  Check(RecordingId(text) == RecordingId(EncodeRecording(decoded)) &&
      RecordingId(text + " ") != RecordingId(text), "route identity uses all file bytes");
  for (const auto& invalid : {std::string("KKBR 2\n"), text.substr(0, text.size() - 5), text + "extra\n"}) {
    try { DecodeRecording(invalid); Check(false, "bad version, truncated or trailing data rejected"); }
    catch (const std::exception&) {}
  }
  Reject(r, [](auto& v) { v.complete = false; }, "interrupted recording rejected");
  Reject(r, [](auto& v) { v.chapters[0].duration = 0; }, "zero duration rejected");
  Reject(r, [](auto& v) { v.chapters[0].duration = 601; }, "oversized duration rejected");
  Reject(r, [](auto& v) { v.chapters[0].entry = 999; }, "invalid chapter rejected");
  Reject(r, [](auto& v) { v.chapters.resize(2); }, "short chapter list rejected");
  Reject(r, [](auto& v) { v.samples[0].user = 4; }, "invalid controller rejected");
  Reject(r, [](auto& v) { v.samples[0].chapter = 4; }, "invalid sample chapter rejected");
  Reject(r, [](auto& v) { v.samples[8].seconds = -.1; }, "negative time rejected");
  Reject(r, [](auto& v) { v.samples[8].seconds = 0; }, "backward time rejected");
  Reject(r, [](auto& v) { v.samples[8].frame = 0; }, "backward frame rejected");
  Reject(r, [](auto& v) { v.samples[0].raw_dt = std::numeric_limits<double>::infinity(); }, "infinite delta rejected");
  Reject(r, [](auto& v) { v.samples[0].scaled_dt = -1; }, "negative scaled delta rejected");
  Reject(r, [](auto& v) { v.samples[0].raw_dt = 2; }, "implausible raw delta rejected");
  Reject(r, [](auto& v) { v.samples.erase(v.samples.begin(), v.samples.begin() + 80); }, "missing opening inputs rejected");
  Reject(r, [](auto& v) { v.samples.resize(v.samples.size() - 80); }, "missing ending inputs rejected");
  Reject(r, [](auto& v) { for (auto& s : v.samples) s.status = 1167; }, "no connected primary pad rejected");
  Reject(r, [](auto& v) { v.checkpoints.clear(); }, "missing checkpoints rejected");
  Reject(r, [](auto& v) { v.checkpoints[0].chapter = 4; }, "invalid checkpoint chapter rejected");
  Reject(r, [](auto& v) { v.checkpoints[2].seconds = 0; }, "backward checkpoint time rejected");
  Reject(r, [](auto& v) { v.checkpoints[0].position[0] = std::numeric_limits<float>::quiet_NaN(); }, "nonfinite position rejected");
  auto bad_byte = text;
  const auto end = bad_byte.find('\n', bad_byte.find("\npad ") + 1);
  bad_byte.replace(end - 1, 1, "999");
  try { DecodeRecording(bad_byte); Check(false, "out-of-range guest byte rejected"); }
  catch (const std::exception&) {}
}

void ReplayTests() {
  const auto r = DecodeRecording(EncodeRecording(Fixture()));
  for (int fps : {30, 60, 120, 144, 240, 1000}) {
    RouteReplay replay;
    replay.Attach(r);
    for (size_t c = 0; c < r.chapters.size(); ++c) {
      replay.Reset(c);
      for (int frame = 0; frame <= fps * 4; ++frame) {
        // Deliberately unrelated replay/record frame counts. Selection uses seconds only.
        const double seconds = frame / double(fps);
        uint32_t missed = 0;
        const auto* actual = replay.Read(0, seconds, missed);
        const size_t recorded_frame = std::min<size_t>(240, size_t(std::floor(seconds * 60 + 1e-7)));
        const auto& expected = r.samples[c * 964 + recorded_frame * 4];
        Check(actual && actual->seconds <= seconds + 1e-9 && actual->state == expected.state,
            "game-time selection at arbitrary frame rates without future inputs");
      }
    }
  }
  RouteReplay replay;
  replay.Attach(r);
  uint32_t missed = 0;
  for (double t : {0., .003, .033, .049, .113, .877, 1.001, 3.01, 4.}) {
    const auto* s = replay.Read(0, t, missed);
    Check(s && s->seconds <= t + 1e-9 && t - s->seconds < 1.0 / 60 + 1e-8,
        "variable game deltas retain bounded input age");
  }
  replay.Reset(1);
  Check(replay.Read(0, 0, missed)->chapter == 1, "chapter load resets replay cursor");
  Check(replay.Read(1, 0, missed)->status == 1167, "secondary pad status replayed independently");
  Check(!replay.Read(4, 0, missed), "invalid user cannot index replay storage");
  Recording pulses;
  pulses.chapters = DefaultRoute(4);
  auto add = [&](double seconds, bool pressed) {
    PadSample s; s.seconds = seconds; s.state[4] = pressed ? 0x10 : 0;
    pulses.samples.push_back(s);
  };
  add(.01, false); add(.02, true); add(.03, false); add(.04, true); add(.05, false);
  replay.Attach(pulses);
  Check(!replay.Read(0, 0, missed), "neutral until first recorded input");
  replay.Read(0, .04, missed);
  Check(missed == 0x1000, "hidden press/release detected even when final state is pressed");
  replay.Read(0, .05, missed);
  Check(!missed, "normally observed release is not a lost transition");
  replay.Reset(0);
  replay.Read(0, .05, missed);
  Check(missed == 0x1000, "multiple complete pulses between polls reported");
  replay.Reset(0);
  for (double t : {.01, .02, .03, .04, .05}) {
    replay.Read(0, t, missed);
    Check(!missed, "fully observed digital transitions do not report drift");
  }
  Check(PositionDistance({0, 0, 0}, {3, 4, 0}) == 5, "checkpoint distance in guest units");
  Check(std::isinf(PositionDistance({NAN, 0, 0}, {0, 0, 0})), "invalid live position reports drift");
  std::vector<double> total;
  double seconds = 0; size_t count = 0;
  for (const auto& part : {std::vector<double>{10, 20, 30}, std::vector<double>{40, 50}}) {
    const auto stats = Summarize(part); seconds += stats.seconds; count += stats.count;
    total.insert(total.end(), part.begin(), part.end());
  }
  const auto stats = Summarize(total);
  Check(stats.count == count && std::abs(stats.seconds - seconds) < 1e-12 &&
      std::abs(stats.average_fps - count / seconds) < 1e-9, "total FPS weights chapter intervals, excludes load gaps");
}

int main(int argc, char** argv) {
  try {
    FormatTests(); ReplayTests();
    if (argc == 3 && std::string(argv[1]) == "--inspect") {
      std::ifstream file(argv[2], std::ios::binary);
      if (!file) throw std::runtime_error("Cannot open recording");
      const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
      const auto r = DecodeRecording(bytes);
      std::cout << "Recording: " << r.chapters.size() << " chapters, " << r.samples.size()
          << " pad reads, " << r.checkpoints.size() << " checkpoints, ID " << RecordingId(bytes) << '\n';
    }
  } catch (const std::exception& e) { Check(false, e.what()); }
  if (!failures) std::cout << "Benchmark route format, arbitrary-rate replay and drift tests passed\n";
  return failures ? 1 : 0;
}
