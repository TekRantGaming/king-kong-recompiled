// Task 353: the start-up step log. One line per risky start-up step (device, swapchain, shader pack, each
// upscaler library, first frame), written straight to the file (no buffer in this process) before and after
// the step, so the last line names the step even when the process is killed outright (a fail-fast or a
// driver ending the program, where no crash handler runs).
//
// The file is the environment variable KK_STEP_LOG (king_kong.exe sets it once it knows its logs folder:
// logs\startup-steps.txt); child processes inherit it (the upscaler check, the restarted game). Without it
// nothing is written. KK_DIAGNOSTICS=1 (the "Start normally (diagnostics)" file in the game folder) also
// flushes every line to the disk and writes the extra steps marked verbose.
//
// Header only, shared by king_kong.exe and the renderer plugin: plain Windows calls, nothing allocated on
// the heap, safe from a crash handler. Elsewhere it does nothing.
#pragma once

#include <cstdarg>
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nr::steps {

#if defined(_WIN32)

namespace detail {
struct State {
  SRWLOCK lock = SRWLOCK_INIT;
  HANDLE file = nullptr;  // not open
  wchar_t path[MAX_PATH] = L"";
  int verbose = -1;  // -1 not read yet
};
inline State& Get() {
  static State s;  // constant-initialised: usable from any thread and from crash handlers
  return s;
}
}  // namespace detail

// KK_DIAGNOSTICS=1: every line flushed to the disk, and the verbose steps.
inline bool Verbose() {
  detail::State& s = detail::Get();
  if (s.verbose < 0) {
    wchar_t v[8] = L"";
    s.verbose = GetEnvironmentVariableW(L"KK_DIAGNOSTICS", v, 8) && v[0] == L'1' ? 1 : 0;
  }
  return s.verbose == 1;
}

// Appends "<local time> pid <pid> tid <tid> <text>" to KK_STEP_LOG.
inline void Note(const char* format, ...) {
  detail::State& s = detail::Get();
  wchar_t path[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"KK_STEP_LOG", path, MAX_PATH);
  if (!n || n >= MAX_PATH) return;
  char line[1024];
  SYSTEMTIME t;
  GetLocalTime(&t);
  int used = std::snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u pid %lu tid %lu ", t.wYear, t.wMonth,
                           t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(),
                           GetCurrentThreadId());
  if (used < 0) return;
  va_list args;
  va_start(args, format);
  const int text = std::vsnprintf(line + used, sizeof(line) - size_t(used) - 2, format, args);
  va_end(args);
  if (text > 0) used += text;
  if (used > int(sizeof(line)) - 3) used = int(sizeof(line)) - 3;
  line[used++] = '\r';
  line[used++] = '\n';
  const bool flush = Verbose();
  AcquireSRWLockExclusive(&s.lock);
  if (!s.file || lstrcmpiW(s.path, path) != 0) {
    if (s.file) CloseHandle(s.file);
    // Appends are whole lines even with several processes writing (FILE_APPEND_DATA).
    s.file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) s.file = nullptr;
    lstrcpynW(s.path, path, MAX_PATH);
  }
  if (s.file) {
    DWORD written = 0;
    WriteFile(s.file, line, DWORD(used), &written, nullptr);
    if (flush) FlushFileBuffers(s.file);
  }
  ReleaseSRWLockExclusive(&s.lock);
}

#else

inline bool Verbose() { return false; }
inline void Note(const char*, ...) {}

#endif

}  // namespace nr::steps
