#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace kk::benchmark {
// The close-request hook calls Shutdown before the SDK's hard exit. The same
// barrier covers normal teardown and prevents a late game-thread save starting.
class ResultWriter {
 public:
  ~ResultWriter() { Shutdown(); }
  template<class Work> bool Start(Work work) {
    return Start(std::move(work), [] {});
  }
  template<class Work, class Starting> bool Start(Work work, Starting starting) {
    std::lock_guard lock(mutex_);
    if (closed_ || thread_.joinable()) return false;
    // Publish Saving under the same lock as worker registration. An accepted
    // close must wait even if it races this handoff before the thread exists.
    starting();
    thread_ = std::thread(std::move(work));
    return true;
  }
  void Shutdown() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    if (thread_.joinable()) thread_.join();
  }
 private:
  std::mutex mutex_;
  std::thread thread_;
  bool closed_ = false;
};

// Results directories are unique. Publish each complete, closed file with one
// rename; an IO failure or abrupt termination can leave only a private .tmp.
inline bool WriteResult(const std::filesystem::path& path, const std::string& text) {
  auto temporary = path;
  temporary += ".tmp";
  std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
  file.write(text.data(), std::streamsize(text.size()));
  file.flush();
  file.close();
  if (file.fail()) return false;
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  return !error;
}
}  // namespace kk::benchmark
