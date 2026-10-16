// Task 177: file writes off the render thread. The render thread snapshots what a file should hold
// (a cheap copy) and queues it here; one background thread does the disk work, so a busy or slow disk
// never holds a frame. Whole-file replacements are atomic (temp file + rename) and a replacement of a
// path that is still waiting is superseded by the newer one. Flush() waits for everything queued;
// Stop() (also the destructor) flushes and joins, after which work runs on the caller (nothing is lost
// at shutdown). The thread starts with the first job.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nr {

// Writes `bytes` to path + ".tmp" and renames it over `path` (a reader sees the old file or the new one,
// never a torn one). Directories are not created: a cache folder removed meanwhile makes this fail
// instead of bringing it back. A rename refused for a moment (another program reading the old file)
// is retried a few times.
inline bool ReplaceFileAtomically(const std::string& path, const void* bytes, size_t size, std::string* error = nullptr) {
  if (path.empty()) {
    if (error) *error = "no path";
    return false;
  }
  const std::string temp = path + ".tmp";
  std::FILE* out = std::fopen(temp.c_str(), "wb");
  if (!out) {
    if (error) *error = "cannot create " + temp;
    return false;
  }
  bool ok = size == 0 || std::fwrite(bytes, 1, size, out) == size;
  ok = std::fflush(out) == 0 && ok;
  ok = std::fclose(out) == 0 && ok;
  std::error_code ec;
  if (ok) {
    for (int attempt = 0; attempt < 5; ++attempt) {
      std::filesystem::rename(temp, path, ec);
      if (!ec) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ok = !ec;
  }
  if (!ok) {
    if (error) *error = "cannot write " + path + (ec ? " (" + ec.message() + ")" : std::string());
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
  }
  return ok;
}

class BackgroundFileWriter {
 public:
  using Bytes = std::vector<uint8_t>;
  // Called on the writer thread (or the caller after Stop) with the result and the milliseconds the
  // write took, including the developer delay.
  using Done = std::function<void(bool ok, double ms)>;

  BackgroundFileWriter() {
    std::lock_guard lock(Registry().mutex);
    Registry().writers.insert(this);
  }
  BackgroundFileWriter(const BackgroundFileWriter&) = delete;
  BackgroundFileWriter& operator=(const BackgroundFileWriter&) = delete;
  ~BackgroundFileWriter() {
    {
      std::lock_guard lock(Registry().mutex);
      Registry().writers.erase(this);
    }
    Stop();
  }

  // Every live writer in this module, flushed within `timeout` in all: the process is about to exit
  // without running destructors (the runtime hard-exits when the title ends). False on a timeout.
  static bool FlushAll(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::lock_guard lock(Registry().mutex);
    bool all = true;
    for (BackgroundFileWriter* w : Registry().writers) all = w->FlushUntil(deadline) && all;
    return all;
  }

  // Developer aid (KK_DEV_SLOW_PRELOAD_SAVE_MS): every replacement waits this long first, as a slow disk would.
  void set_delay_ms(uint32_t ms) { delay_ms_ = ms; }
  // Developer A/B (KK_DEV_SYNC_FILE_WRITES=1): every job runs on the caller, as before task 177.
  void set_synchronous(bool on) { synchronous_ = on; }

  // Queues an atomic replacement of `path` with `bytes`.
  void Replace(std::string path, Bytes bytes, Done done = {}) {
    auto data = std::make_shared<const Bytes>(std::move(bytes));
    std::unique_lock lock(mutex_);
    if (stopped_ || synchronous_) {
      lock.unlock();
      RunReplace(path, *data, done);
      return;
    }
    pending_[path] = data;
    for (Job& queued : queue_) {
      if (queued.replace && queued.path == path) {  // still waiting: the newer content replaces it
        queued.bytes = std::move(data);
        queued.done = std::move(done);
        ++superseded_;
        return;
      }
    }
    queue_.push_back(Job{true, std::move(path), std::move(data), std::move(done), {}});
    StartLocked();
    lock.unlock();
    cv_.notify_one();
  }

  // Queues a task (e.g. appending to an open file), run in order with the replacements.
  void Post(std::function<void()> task) {
    std::unique_lock lock(mutex_);
    if (stopped_ || synchronous_) {
      lock.unlock();
      task();
      return;
    }
    queue_.push_back(Job{false, {}, {}, {}, std::move(task)});
    StartLocked();
    lock.unlock();
    cv_.notify_one();
  }

  // What `path` will hold once the queued or running replacement of it is done (false: none waiting),
  // so a reader never sees a file older than what was already decided.
  bool Pending(const std::string& path, Bytes* bytes) const {
    std::lock_guard lock(mutex_);
    auto it = pending_.find(path);
    if (it == pending_.end()) return false;
    if (bytes) *bytes = *it->second;
    return true;
  }

  // Waits until everything queued so far is written.
  void Flush() {
    std::unique_lock lock(mutex_);
    idle_cv_.wait(lock, [&] { return queue_.empty() && !busy_; });
  }
  bool FlushUntil(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    return idle_cv_.wait_until(lock, deadline, [&] { return queue_.empty() && !busy_; });
  }

  // Flushes and joins the thread; later jobs run on the caller.
  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stopped_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  uint64_t written() const { std::lock_guard lock(mutex_); return written_; }
  uint64_t failed() const { std::lock_guard lock(mutex_); return failed_; }
  uint64_t superseded() const { std::lock_guard lock(mutex_); return superseded_; }

 private:
  struct Job {
    bool replace = false;
    std::string path;
    std::shared_ptr<const Bytes> bytes;
    Done done;
    std::function<void()> task;
  };

  struct WriterRegistry {
    std::mutex mutex;
    std::unordered_set<BackgroundFileWriter*> writers;
  };
  static WriterRegistry& Registry() {
    static WriterRegistry registry;
    return registry;
  }

  void StartLocked() {
    if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
  }

  void Delay() const {
    if (delay_ms_) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
  }

  void RunReplace(const std::string& path, const Bytes& bytes, const Done& done) {
    const auto start = std::chrono::steady_clock::now();
    Delay();
    const bool ok = ReplaceFileAtomically(path, bytes.data(), bytes.size());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    {
      std::lock_guard lock(mutex_);
      ++(ok ? written_ : failed_);
    }
    if (done) done(ok, ms);
  }

  void Run() {
    std::unique_lock lock(mutex_);
    for (;;) {
      cv_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
      if (queue_.empty()) break;  // stopped and everything written
      Job job = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
      lock.unlock();
      if (job.replace) {
        RunReplace(job.path, *job.bytes, job.done);
      } else {
        job.task();
      }
      lock.lock();
      if (job.replace) {
        auto it = pending_.find(job.path);
        if (it != pending_.end() && it->second == job.bytes) pending_.erase(it);
      }
      busy_ = false;
      if (queue_.empty()) idle_cv_.notify_all();
    }
    busy_ = false;
    idle_cv_.notify_all();
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_, idle_cv_;
  std::deque<Job> queue_;
  std::unordered_map<std::string, std::shared_ptr<const Bytes>> pending_;
  std::thread thread_;
  bool busy_ = false, stopped_ = false, synchronous_ = false;
  uint32_t delay_ms_ = 0;
  uint64_t written_ = 0, failed_ = 0, superseded_ = 0;
};

}  // namespace nr
