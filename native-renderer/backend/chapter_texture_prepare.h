// CPU-only preparation of one immutable texture snapshot. The render thread
// never joins a pending job; it polls and publishes completed work while loading.
#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>
#include "kknr/resident_snapshot.h"

namespace nr {

inline kknr::TextureKey ChapterTextureLayout(kknr::TextureFetch fetch) {
  auto key = kknr::MakeTextureKey(fetch);
  key.words[1] &= 0xFFF;
  key.words[5] &= 0xFFF;
  return key;
}

// Creation notifications contain only the fetch header, never guest pointers.
// The bounded inbox is independent of the active renderer's lifetime.
class ChapterTextureInbox {
 public:
  void Push(const uint32_t* words) {
    auto fetch = kknr::TextureFetch::FromWords(words);
    fetch.words[1] = (fetch.words[1] & 0xFFF) | fetch.BaseAddress();
    fetch.words[5] = (fetch.words[5] & 0xFFF) | fetch.MipAddress();
    if (fetch.Type() != 2) return;
    std::lock_guard lock(mutex_);
    if (pending_.size() < 4096) pending_.push_back(fetch);
  }
  std::vector<kknr::TextureFetch> Take() {
    std::lock_guard lock(mutex_);
    std::vector<kknr::TextureFetch> result;
    result.swap(pending_);
    return result;
  }
 private:
  std::mutex mutex_;
  std::vector<kknr::TextureFetch> pending_;
};
inline ChapterTextureInbox& TextureCreationInbox() {
  static ChapterTextureInbox inbox;
  return inbox;
}

class ChapterTexturePrepare {
 public:
  static constexpr uint64_t kMaxBytes = 8ull << 20;
  struct Job {
    kknr::TextureFetch fetch;
    kknr::ResidentSnapshot snapshot;
    std::vector<uint8_t> source;
    std::vector<uint64_t> expected;
    kknr::TextureOptions options;
    kknr::HostTextureData data;
    uint64_t generation = 0, hash = 0;
    bool matched = false, converted = false;
  };
  ChapterTexturePrepare() : thread_([this] { Run(); }) {}
  ~ChapterTexturePrepare() {
    { std::lock_guard lock(mutex_); stop_ = true; }
    cv_.notify_one();
    thread_.join();
  }
  bool Busy() {
    std::unique_lock lock(mutex_, std::try_to_lock);
    return !lock || busy_ || pending_ || ready_;
  }
  void Start(std::unique_ptr<Job> job) {
    { std::lock_guard lock(mutex_); pending_ = std::move(job); }
    cv_.notify_one();
  }
  std::unique_ptr<Job> Take() {
    std::unique_lock lock(mutex_, std::try_to_lock);
    return lock ? std::move(ready_) : nullptr;
  }
 private:
  void Run() {
    std::unique_lock lock(mutex_);
    for (;;) {
      cv_.wait(lock, [&] { return stop_ || pending_; });
      if (stop_) return;
      auto job = std::move(pending_);
      busy_ = true;
      lock.unlock();
      try {
        job->hash = job->snapshot.Hash();
        job->matched = std::find(job->expected.begin(), job->expected.end(), job->hash) != job->expected.end();
        if (job->matched)
          job->converted = kknr::ConvertTexture(job->snapshot.Relocate(job->fetch), job->snapshot.memory,
                                               job->data, nullptr, job->options);
        if (job->data.bytes.size() > kMaxBytes) { job->data = {}; job->converted = false; }
      } catch (...) { job->converted = false; }
      lock.lock();
      ready_ = std::move(job);
      busy_ = false;
    }
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false, busy_ = false;
  std::unique_ptr<Job> pending_, ready_;
  std::thread thread_;
};
}  // namespace nr
