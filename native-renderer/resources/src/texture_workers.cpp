#include "kknr/texture_convert.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace kknr {

struct TextureConversionWorkers::Impl {
  std::mutex dispatch_mutex, mutex;
  std::condition_variable work, done;
  std::vector<std::thread> threads;
  bool stop = false;
  uint64_t generation = 0;
  unsigned jobs = 0, participants = 0, remaining = 0;
  void* context = nullptr;
  void (*job)(void*, unsigned) = nullptr;
  std::exception_ptr error;

  explicit Impl(unsigned count) {
    try {
      for (unsigned i = 1; i <= std::min(count, 3u); ++i)
        threads.emplace_back([this, i] { Worker(i); });
    } catch (...) {
      // Thread creation can fail under process limits. Join partial construction, then use the caller.
      Stop();
      threads.clear();
      stop = false;
    }
  }
  ~Impl() { Stop(); }

  void Stop() {
    {
      std::lock_guard lock(mutex);
      stop = true;
    }
    work.notify_all();
    for (auto& thread : threads) thread.join();
  }

  void Invoke(unsigned index) {
    for (; index < jobs; index += participants) {
      try {
        job(context, index);
      } catch (...) {
        std::lock_guard lock(mutex);
        if (!error) error = std::current_exception();
      }
    }
  }

  void Worker(unsigned index) {
    uint64_t seen = 0;
    std::unique_lock lock(mutex);
    for (;;) {
      work.wait(lock, [&] { return stop || generation != seen; });
      if (stop) return;
      seen = generation;
      if (index >= participants) continue;
      lock.unlock();
      Invoke(index);
      lock.lock();
      if (!--remaining) done.notify_one();
    }
  }

  void Run(unsigned count, void* arg, void (*fn)(void*, unsigned)) {
    // Sharing one pool between renderer instances cannot mix contexts or outlive a completed invocation.
    std::lock_guard dispatch(dispatch_mutex);
    if (!count) return;
    {
      std::lock_guard lock(mutex);
      jobs = count;
      participants = std::min(count, unsigned(threads.size()) + 1);
      remaining = participants - 1;
      context = arg;
      job = fn;
      error = nullptr;
      ++generation;
    }
    work.notify_all();
    Invoke(0);
    std::unique_lock lock(mutex);
    done.wait(lock, [&] { return !remaining; });
    job = nullptr;
    context = nullptr;
    if (error) std::rethrow_exception(error);
  }
};

TextureConversionWorkers::TextureConversionWorkers(unsigned max_workers) : impl_(new Impl(max_workers)) {}
TextureConversionWorkers::~TextureConversionWorkers() = default;
unsigned TextureConversionWorkers::WorkerCount() const { return unsigned(impl_->threads.size()); }
void TextureConversionWorkers::Run(unsigned jobs, void* context, void (*job)(void*, unsigned)) {
  impl_->Run(jobs, context, job);
}

}  // namespace kknr
