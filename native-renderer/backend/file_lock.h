// An exclusive lock on a file, shared by processes and by devices in one process (task 185).
//
// The driver pipeline cache (vk_pipeline_cache.h) is saved by every device that uses it: the setup
// job's, the launcher prewarm's (--native_launcher_prewarm) and the game's, which can be in one
// process or in two (the launcher and the game child). Two saves at once wrote the same temporary
// file and the next start found the cache "damaged". Saves now hold this lock on "<file>.lock".
//
// The lock belongs to the open handle (Windows LockFileEx, POSIX flock), so a second FileLock in the
// same process waits like another process does, and the system drops it when a process ends (a game
// cut off in the exit flush leaves no stale lock). The lock file itself stays (empty).
#pragma once

#include <chrono>
#include <string>

namespace nr {

class FileLock {
 public:
  FileLock() = default;
  ~FileLock() { Unlock(); }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  // Opens (creating) `path` and takes its exclusive lock, waiting up to `timeout`. False: not taken
  // (another holder kept it, or the file cannot be made).
  bool Lock(const std::string& path, std::chrono::milliseconds timeout);
  void Unlock();
  bool locked() const { return locked_; }

 private:
  bool locked_ = false;
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace nr
