#include "backend/file_lock.h"

#include <filesystem>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace nr {

bool FileLock::Lock(const std::string& path, std::chrono::milliseconds timeout) {
  Unlock();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
#ifdef _WIN32
  const std::filesystem::path p(path);
  for (;;) {
    if (!handle_) {
      HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) handle_ = h;
    }
    if (handle_) {
      OVERLAPPED overlapped = {};
      if (LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
        locked_ = true;
        return true;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
#else
  for (;;) {
    if (fd_ < 0) fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) == 0) {
      locked_ = true;
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
#endif
  Unlock();
  return false;
}

void FileLock::Unlock() {
#ifdef _WIN32
  if (handle_) {
    if (locked_) {
      OVERLAPPED overlapped = {};
      UnlockFileEx(handle_, 0, 1, 0, &overlapped);
    }
    CloseHandle(handle_);
    handle_ = nullptr;
  }
#else
  if (fd_ >= 0) {
    if (locked_) flock(fd_, LOCK_UN);
    close(fd_);
    fd_ = -1;
  }
#endif
  locked_ = false;
}

}  // namespace nr
