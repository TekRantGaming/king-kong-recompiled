// CPU-only preparation of one immutable texture snapshot. The render thread
// never joins a pending job; it polls and publishes completed work while loading.
#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include "backend/thread_scheduling.h"
#include "backend/guest_memory.h"
#include "kknr/resident_snapshot.h"

namespace nr {

inline kknr::TextureFetch ChapterTextureFetch(kknr::TextureFetch fetch) {
  // SetTexture (82118F78) converts the CPU-view addresses in the object to
  // physical GPU addresses. The E view has an extra 4 KiB host offset; simply
  // clearing the high three bits addresses the preceding page instead.
  for (const unsigned word : {1u, 5u}) {
    const uint32_t value = fetch.words[word];
    const uint32_t offset = ((value >> 20) + 0x200) & 0x1000;
    fetch.words[word] = (value + offset) & 0x1FFFFFFF;
  }
  return fetch;
}

inline kknr::TextureKey ChapterTextureLayout(kknr::TextureFetch fetch) {
  auto key = kknr::MakeTextureKey(fetch);
  key.words[1] &= 0xFFF;
  key.words[5] &= 0xFFF;
  return key;
}

// Creation supplies guest object addresses; the loader may rewrite the header
// after CreateTexture (for example replacing its temporary RGBA layout with BC).
// Reading a candidate later must use the optional, fault-safe live-memory copy.
class ChapterTextureInbox {
 public:
  void Push(uint32_t object) {
    if (!object) return;
    std::lock_guard lock(mutex_);
    if (pending_.size() < 4096) pending_.push_back(object);
  }
  std::vector<uint32_t> Take() {
    std::lock_guard lock(mutex_);
    std::vector<uint32_t> result;
    result.swap(pending_);
    return result;
  }
 private:
  std::mutex mutex_;
  std::vector<uint32_t> pending_;
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
    uint64_t allocation_bytes = 0;
    bool matched = false, converted = false;
    bool scan = false;
    uint32_t scan_begin = 0, scan_end = 0;
    std::function<bool(uint32_t, void*, uint32_t)> copy;
    std::function<bool()> cancelled;
    std::vector<kknr::TextureKey> layouts;
    std::vector<uint32_t> discovered;
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
    SchedulePipelineCompiler(false);  // the same below-normal/EcoQoS policy as other background preparation
    std::unique_lock lock(mutex_);
    for (;;) {
      cv_.wait(lock, [&] { return stop_ || pending_; });
      if (stop_) return;
      auto job = std::move(pending_);
      busy_ = true;
      lock.unlock();
      try {
        if (job->scan) {
          Discover(*job);
        } else {
        job->hash = job->snapshot.Hash();
        job->matched = std::find(job->expected.begin(), job->expected.end(), job->hash) != job->expected.end();
        if (job->matched)
          job->converted = kknr::ConvertTexture(job->snapshot.Relocate(job->fetch), job->snapshot.memory,
                                               job->data, nullptr, job->options);
        if (job->data.bytes.size() > kMaxBytes) { job->data = {}; job->converted = false; }
        }
      } catch (...) { job->converted = false; }
      lock.lock();
      ready_ = std::move(job);
      busy_ = false;
    }
  }
  static void Discover(Job& job) {
    std::unordered_set<uint32_t> first_words;
    for (const auto& key : job.layouts) first_words.insert(key.words[0]);
    std::vector<uint8_t> bytes(65536);
    auto inspect = [&](uint32_t address, uint32_t size) {
      for (uint32_t at = 0; at + 40 <= size; at += 4) {
        const auto* p = bytes.data() + at;
        const uint32_t common = LoadBE32(p);
        if (((common >> 16) & 7) != 3) continue;
        const uint32_t word0 = LoadBE32(p + 16) & ~(0x1FFu << 10);
        if (!first_words.contains(word0)) continue;
        const auto fetch = kknr::TextureFetch::FromGuest(p + 16);
        if (fetch.Type() != 2) continue;
        const auto layout = ChapterTextureLayout(fetch);
        if (std::find(job.layouts.begin(), job.layouts.end(), layout) == job.layouts.end()) continue;
        if (job.discovered.size() < 1024) job.discovered.push_back(address + at);
      }
    };
    for (uint32_t address = job.scan_begin; address < job.scan_end;) {
      if (job.cancelled && job.cancelled()) return;
      const uint32_t size = std::min(65536u, job.scan_end - address);
      if (job.copy(address, bytes.data(), size)) inspect(address, size);
      else for (uint32_t page = 0; page < size; page += 4096) {
        if (job.cancelled && job.cancelled()) return;
        const uint32_t n = std::min(4096u, size - page);
        if (job.copy(address + page, bytes.data(), n)) inspect(address + page, n);
      }
      address += size;
    }
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false, busy_ = false;
  std::unique_ptr<Job> pending_, ready_;
  std::thread thread_;
};
}  // namespace nr
