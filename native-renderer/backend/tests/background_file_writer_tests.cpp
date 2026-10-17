// Task 177: the background file writer. A queued write never blocks the caller (even on a slow disk),
// files are replaced atomically, a newer replacement of a waiting path wins, readers see pending
// content, Stop() writes everything, and a missing folder is never created again.
#include "backend/background_file_writer.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

static int cases = 0;
static void Check(bool condition, const char* what) {
  ++cases;
  if (!condition) { std::fprintf(stderr, "case %d failed: %s\n", cases, what); std::exit(1); }
}

static std::vector<uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }
static std::string ReadAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
static double MsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

int main() {
  using namespace nr;
  const auto dir = std::filesystem::temp_directory_path() / "nr-background-file-writer-tests";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir);
  const std::string a = (dir / "a.bin").string(), b = (dir / "b.bin").string();

  // Atomic replacement: content written, no temp file left, an existing file replaced.
  {
    BackgroundFileWriter w;
    std::atomic<int> done{0};
    w.Replace(a, Bytes("first"), [&](bool ok, double) { done += ok; });
    w.Flush();
    Check(ReadAll(a) == "first", "first content");
    w.Replace(a, Bytes("second"), [&](bool ok, double) { done += ok; });
    w.Flush();
    Check(ReadAll(a) == "second", "replaced content");
    Check(!std::filesystem::exists(a + ".tmp"), "no temp file left");
    Check(done == 2 && w.written() == 2 && w.failed() == 0, "two writes reported");
  }
  // A slow disk (developer delay) never holds the caller; the content is pending until written.
  {
    BackgroundFileWriter w;
    w.set_delay_ms(400);
    const auto t = std::chrono::steady_clock::now();
    w.Replace(b, Bytes("slow"));
    Check(MsSince(t) < 100, "a queued write returns at once");
    std::vector<uint8_t> pending;
    Check(w.Pending(b, &pending) && pending == Bytes("slow"), "pending content visible");
    Check(!std::filesystem::exists(b), "not written yet");
    w.Flush();
    Check(MsSince(t) >= 390, "flush waits for the slow write");
    Check(ReadAll(b) == "slow", "slow write landed");
    Check(!w.Pending(b, nullptr), "nothing pending after the write");
  }
  // A newer replacement of a path still waiting supersedes it; tasks run in order.
  {
    BackgroundFileWriter w;
    w.set_delay_ms(200);
    std::vector<int> order;
    w.Replace(a, Bytes("one"));  // starts (sleeping in the delay)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    w.Replace(a, Bytes("two"));
    w.Post([&] { order.push_back(1); });
    w.Replace(a, Bytes("three"));  // replaces "two" in the queue (before the task)
    w.Post([&] { order.push_back(2); });
    std::vector<uint8_t> pending;
    Check(w.Pending(a, &pending) && pending == Bytes("three"), "newest content pending");
    w.Flush();
    Check(ReadAll(a) == "three", "newest content written");
    Check(w.superseded() == 1 && w.written() == 2, "one waiting write superseded");
    Check(order.size() == 2 && order[0] == 1 && order[1] == 2, "tasks in order");
  }
  // Stop() writes everything still queued; later work runs on the caller.
  {
    BackgroundFileWriter w;
    w.set_delay_ms(150);
    w.Replace(b, Bytes("at exit"));
    w.Stop();
    Check(ReadAll(b) == "at exit", "stop flushes");
    w.set_delay_ms(0);
    w.Replace(b, Bytes("after stop"));
    Check(ReadAll(b) == "after stop", "after stop: written on the caller");
  }
  // The destructor also flushes.
  {
    {
      BackgroundFileWriter w;
      w.set_delay_ms(100);
      w.Replace(a, Bytes("destructor"));
    }
    Check(ReadAll(a) == "destructor", "destructor flushes");
  }
  // A folder removed meanwhile is not created again: the write fails and says so.
  {
    BackgroundFileWriter w;
    const auto gone = dir / "gone";
    bool reported = false, result = true;
    w.Replace((gone / "x.bin").string(), Bytes("x"), [&](bool ok, double) { reported = true; result = ok; });
    w.Flush();
    Check(reported && !result && w.failed() == 1, "failure reported");
    Check(!std::filesystem::exists(gone), "missing folder not created");
  }
  // The old path (developer A/B): the caller does the write and waits for the delay.
  {
    BackgroundFileWriter w;
    w.set_synchronous(true);
    w.set_delay_ms(150);
    const auto t = std::chrono::steady_clock::now();
    w.Replace(b, Bytes("sync"));
    Check(MsSince(t) >= 140 && ReadAll(b) == "sync", "synchronous write on the caller");
  }
  // Task 179: the exit task (the driver's pipeline cache save) runs inside FlushAll's bounded wait, every time.
  {
    BackgroundFileWriter w;
    int runs = 0;
    w.set_exit_task([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      ++runs;
    });
    Check(BackgroundFileWriter::FlushAll(std::chrono::milliseconds(3000)) && runs == 1, "exit task inside FlushAll");
    Check(BackgroundFileWriter::FlushAll(std::chrono::milliseconds(3000)) && runs == 2, "exit task again");
    w.set_exit_task({});
    Check(BackgroundFileWriter::FlushAll(std::chrono::milliseconds(3000)) && runs == 2, "cleared exit task");
    w.set_exit_task([] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); });
    Check(!BackgroundFileWriter::FlushAll(std::chrono::milliseconds(50)), "exit task bounded by the timeout");
    w.set_exit_task({});
  }
  std::filesystem::remove_all(dir, ec);
  std::printf("nr_background_file_writer_tests: %d cases passed\n", cases);
  return 0;
}
