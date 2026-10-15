#include "frame_pacer.h"
#include "benchmark_data.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

using kk::pacing::Clock;
using namespace std::chrono_literals;
int failures = 0;
void Check(bool condition, const char* what) {
  if (!condition) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
bool Near(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; }

void TestSchedule() {
  kk::pacing::Schedule s;
  const auto t = Clock::time_point(1s);
  Check(s.Deadline(30, t) == t, "first capped frame does not wait");
  s.Released(t);
  Check(s.Deadline(30, t + 1ms) > t + 33ms, "30 Hz deadline");
  const auto late = t + 40ms;
  Check(s.Deadline(30, late) == late, "overrun does not add a delay");
  s.Released(late);
  Check(s.Deadline(30, late + 1ms) > late + 33ms, "late frame cannot create a short catch-up interval");
  Check(s.Deadline(240, late + 2ms) == late + 2ms, "cap change resets old deadline");
  s.Released(late + 2ms);
  Check(s.Deadline(240, late + 3ms) < late + 7ms, "240 Hz is not fixed to 30 or 60");
  Check(s.Deadline(0, late + 3ms) == late + 3ms, "uncapped never waits");
  s.Released(late + 3ms);
  Check(s.Deadline(60, late + 4ms) == late + 4ms, "re-enabling cap starts immediately");
  s.Released(late + 4ms);
  Check(s.Deadline(60, late + 10s) == late + 10s, "pause resumes without catch-up");
  kk::pacing::Schedule drift;
  drift.Deadline(60, t); drift.Released(t);
  auto tick = t;
  for (int i = 0; i < 600; ++i) {
    tick = drift.Deadline(60, tick + 1ms);
    drift.Released(tick + 50us);
  }
  Check(std::abs(std::chrono::duration<double>(tick - t).count() - 10) < 1e-5,
        "small timer wake errors do not accumulate into slower pacing");
}

void TestStats() {
  using namespace kk::benchmark;
  Check(Summarize({}).count == 0, "empty benchmark");
  std::vector<double> times(100, 10);
  times.back() = 100;
  const auto s = Summarize(times);
  Check(s.count == 100 && Near(s.average_fps, 100000.0 / 1090), "average uses total elapsed time");
  Check(Near(s.low_fps, 10) && Near(s.worst_ms, 100), "1% low uses slowest interval");
  Check(Near(s.p50_ms, 10) && Near(s.p99_ms, 10), "nearest-rank quantiles");
  times.push_back(200);
  const auto odd = Summarize(times);
  Check(Near(odd.low_fps, 1000.0 / 150), "1% ceil includes two samples at N=101");
  times = {0, -1, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 20};
  const auto filtered = Summarize(times);
  Check(filtered.count == 1 && Near(filtered.average_fps, 50) && Near(filtered.stdev_ms, 0), "invalid samples excluded");
  times.assign(1000, 5);
  times[501] = 500;
  const auto graph = Graph(times, 20);
  Check(graph.size() == 20 && *std::max_element(graph.begin(), graph.end()) == 500, "graph preserves a single hitch");
  Check(Graph({}, 0).empty(), "empty graph");
  Check(Csv(std::vector<double>{10, 20}, "game") == "game,1,0.010000,10.000000\ngame,2,0.030000,20.000000\n", "CSV elapsed times and locale");
  Check(CameraX(0) == 32767 && CameraX(3) == 0 && CameraX(6) == -32767 &&
        CameraX(12) == 0 && CameraX(14) == 32767 && CameraX(17) == 0 &&
        CameraX(25) == 32767 && CameraX(75) == 32767, "replay boundaries");
  for (int fps : {30, 60, 120, 144, 240, 1000}) {
    // Integrate the replay's stick command at each supported simulation rate.
    double area = 0;
    for (int frame = 0; frame < fps * 75; ++frame) area += CameraX((frame + .5) / fps) / double(fps);
    Check(std::abs(area) < 1, "camera sweep has the same total motion at every frame rate");
  }
}

// The Beta 5 limiter, retained only in this offline comparison tool.
class Legacy {
 public:
  void Wait(int fps) {
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
    auto now = Clock::now();
    if (next_ == Clock::time_point{} || now - next_ > period) { next_ = now + period; return; }
    const auto coarse = next_ - now - 500us;
#if defined(_WIN32)
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer_ && coarse > Clock::duration::zero()) {
      LARGE_INTEGER due;
      due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(coarse).count() / 100;
      SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer_, INFINITE);
    }
#else
    if (coarse > Clock::duration::zero()) std::this_thread::sleep_for(coarse);
#endif
    while (Clock::now() < next_) std::this_thread::yield();
    next_ += period;
  }
  ~Legacy() {
#if defined(_WIN32)
    if (timer_) CloseHandle(timer_);
#endif
  }
 private:
  Clock::time_point next_{};
#if defined(_WIN32)
  HANDLE timer_ = nullptr;
#endif
};

template<class Limiter>
void Measure(std::ofstream& out, const char* name, int fps, bool stalled, double seconds) {
  Limiter limiter;
  for (int warm = 0; warm < 4; ++warm) limiter.Wait(fps);
  auto previous = Clock::now();
  const auto start = previous;
#if defined(_WIN32)
  FILETIME created, exited, kernel_before, user_before, kernel_after, user_after;
  GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel_before, &user_before);
#endif
  std::vector<double> samples;
  kk::pacing::Waiter work_stall;
  for (size_t i = 0; std::chrono::duration<double>(Clock::now() - start).count() < seconds; ++i) {
    // A renderer that occasionally misses the cap by 1.5 ms. The long frame
    // is unavoidable; the next frame need not be shortened to compensate.
    if (stalled && i % 17 == 16) work_stall.Until(previous +
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps)) + 1500us);
    limiter.Wait(fps);
    const auto now = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - previous).count();
    out << name << ',' << fps << ',' << stalled << ',' << samples.size() + 1 << ',' << ms << '\n';
    samples.push_back(ms);
    previous = now;
  }
  const auto stats = kk::benchmark::Summarize(samples);
  double cpu_ms = 0;
#if defined(_WIN32)
  GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel_after, &user_after);
  auto ticks = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  cpu_ms = (ticks(kernel_after) + ticks(user_after) - ticks(kernel_before) - ticks(user_before)) / 10000.0;
#endif
  std::cout << name << ',' << fps << ',' << stalled << ',' << stats.average_fps << ',' << stats.stdev_ms << ','
            << stats.p99_ms - stats.p50_ms << ',' << cpu_ms / (stats.seconds * 10) << '\n';
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "--measure") {
    const double seconds = argc >= 4 ? std::atof(argv[3]) : 3;
    std::ofstream csv(argv[2]);
    if (!csv || seconds <= 0) return 2;
    csv << "limiter,fps,stalled,frame,frame_ms\n";
    std::cout << "limiter,cap,stalled,avg_fps,stdev_ms,p99-p50_ms,thread_cpu_percent\n";
    for (bool stalled : {false, true}) for (int fps : {30, 60, 120, 144, 240}) {
      Measure<Legacy>(csv, "before", fps, stalled, seconds);
      Measure<kk::pacing::Limiter>(csv, "after", fps, stalled, seconds);
    }
    return csv.good() ? 0 : 2;
  }
  TestSchedule();
  TestStats();
  if (!failures) std::cout << "Pacing and benchmark tests passed\n";
  return failures ? 1 : 0;
}
