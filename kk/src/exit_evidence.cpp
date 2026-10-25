// Task 229: evidence when the program ends without a word. See exit_evidence.h.
#include "exit_evidence.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winevt.h>
#pragma comment(lib, "wevtapi.lib")

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <typeinfo>

#include <rex/logging.h>

#include "backend/startup_steps.h"
#include "crash_dump.h"
#include "exit_evidence_logic.h"

namespace kk::exit_evidence {
namespace {

std::mutex g_mutex;           // g_marker and the file
SessionMarker g_marker;       // this process's
wchar_t g_path[MAX_PATH] = L"";
std::atomic<int64_t> g_frame{0};
std::atomic<bool> g_ended{false};  // clean exit, crash report or last words: the alive thread stops writing
std::atomic<bool> g_last_words{false};

int64_t Now() { return int64_t(std::time(nullptr)); }

// Mutex held (or try-locked).
void WriteMarkerLocked() {
  if (!g_path[0]) return;
  g_marker.frame = g_frame.load(std::memory_order_relaxed);
  const std::string text = FormatMarker(g_marker);
  HANDLE f = CreateFileW(g_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(f, text.data(), DWORD(text.size()), &written, nullptr);
  CloseHandle(f);
}

// From the crash and abort paths: never wait for the lock (the dying thread may hold it).
void SetStateNoWait(const std::string& state) {
  g_ended = true;
  std::unique_lock lock(g_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return;
  g_marker.state = state;
  g_marker.alive = Now();
  WriteMarkerLocked();
}

// Is process `pid` still running king_kong (a second launcher, or a game the launcher started)?
bool StillRunning(uint32_t pid) {
  HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return false;
  bool running = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
  if (running) {
    wchar_t image[MAX_PATH];
    DWORD size = MAX_PATH;
    wchar_t own[MAX_PATH];
    if (QueryFullProcessImageNameW(p, 0, image, &size) && GetModuleFileNameW(nullptr, own, MAX_PATH)) {
      const wchar_t* a = wcsrchr(image, L'\\');
      const wchar_t* b = wcsrchr(own, L'\\');
      running = _wcsicmp(a ? a + 1 : image, b ? b + 1 : own) == 0;  // the id reused by another program: gone
    }
  }
  CloseHandle(p);
  return running;
}

std::string Utf8(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

// What Windows' Application log says about process `pid` since `since` (Unix seconds). Own thread.
void LookUpWindowsRecord(uint32_t pid, int64_t since) {
  wchar_t own[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, own, MAX_PATH)) return;
  const wchar_t* slash = wcsrchr(own, L'\\');
  const std::string exe = Lower(Utf8(slash ? slash + 1 : own));
  int64_t window_ms = (Now() - since + 120) * 1000;
  if (window_ms < 0 || window_ms > 7LL * 24 * 3600 * 1000) window_ms = 7LL * 24 * 3600 * 1000;
  const std::wstring query =
      L"*[System[(Provider[@Name='Application Error'] or Provider[@Name='Application Hang']) and "
      L"TimeCreated[timediff(@SystemTime) <= " + std::to_wstring(window_ms) + L"]]]";
  EVT_HANDLE results = EvtQuery(nullptr, L"Application", query.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection);
  if (!results) {
    REXLOG_INFO("KK: could not read Windows' Application log (error {})", GetLastError());
    return;
  }
  int found = 0, seen = 0;
  EVT_HANDLE events[16];
  DWORD got = 0;
  while (seen < 400 && EvtNext(results, 16, events, 2000, 0, &got)) {
    for (DWORD i = 0; i < got; ++i, ++seen) {
      DWORD used = 0, props = 0;
      EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &used, &props);
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && used) {
        std::wstring xml(used / sizeof(wchar_t) + 1, L'\0');
        if (EvtRender(nullptr, events[i], EvtRenderEventXml, DWORD(xml.size() * sizeof(wchar_t)), xml.data(), &used,
                      &props)) {
          const std::string text = DescribeEndEvent(Utf8(xml.c_str()), pid, exe);
          if (!text.empty() && found < 3) {
            REXLOG_WARN("KK: about process {}: {}", pid, text);
            ++found;
          }
        }
      }
      EvtClose(events[i]);
    }
  }
  EvtClose(results);
  if (!found)
    REXLOG_INFO("KK: Windows recorded no crash or hang for process {} (closed from outside, the PC shut down or "
                "slept, or an end Windows did not record)", pid);
}

void LastWords(const char* why) {
  if (g_last_words.exchange(true)) return;
  nr::steps::Note("the program is ending: %s", why);  // task 353
  REXLOG_ERROR("KK: the program is ending: {}", why);
  rex::FlushLogging();
  kk::WriteCrashReportNow(why);
  SetStateNoWait(std::string("ending: ") + why);
}

void OnAbortSignal(int) {
  // rex::FatalError (a lost graphics device among others) and failed asserts print to stderr, which a
  // windowed program has nowhere, then call abort(); std::terminate ends here too. Returning lets abort()
  // end the program as before.
  LastWords("abort() was called (a fatal error, a failed check or an uncaught exception)");
}

std::terminate_handler g_previous_terminate = nullptr;
void OnTerminate() {
  std::string what = "std::terminate";
  if (const std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& x) {
      what = std::string("an uncaught C++ exception (") + typeid(x).name() + "): " + x.what();
    } catch (...) {
      what = "an uncaught C++ exception of an unknown type";
    }
  }
  LastWords(what.c_str());
  if (g_previous_terminate) g_previous_terminate();
  std::abort();
}

_invalid_parameter_handler g_previous_invalid = nullptr;
void __cdecl OnInvalidParameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                                unsigned int line, uintptr_t reserved) {
  if (g_previous_invalid) {  // someone else decides (it may let the call fail and go on)
    g_previous_invalid(expression, function, file, line, reserved);
    return;
  }
  // Without a handler the C runtime ends the program at once (fail-fast), past every crash handler.
  LastWords("an invalid argument to a C runtime function");
  __fastfail(FAST_FAIL_INVALID_ARG);
}

_purecall_handler g_previous_purecall = nullptr;
void __cdecl OnPureCall() {
  LastWords("a pure virtual function call");
  if (g_previous_purecall) g_previous_purecall();
  std::abort();
}

void AliveThread() {
  for (;;) {
    for (int i = 0; i < 30 && !g_ended; ++i) Sleep(1000);
    if (g_ended) return;
    std::lock_guard lock(g_mutex);
    if (g_marker.state != "running") return;
    g_marker.alive = Now();
    WriteMarkerLocked();
  }
}

}  // namespace

void Install(const std::filesystem::path& logs_dir, const char* role) {
  static bool installed = false;
  if (installed) return;
  installed = true;
  std::error_code ec;
  std::filesystem::create_directories(logs_dir, ec);
  const std::wstring path = (logs_dir / (std::string("session-") + role + ".txt")).wstring();
  if (path.size() >= MAX_PATH) return;

  // The previous run of this kind.
  {
    std::string text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      char buf[2048];
      DWORD got = 0;
      if (ReadFile(f, buf, sizeof buf, &got, nullptr)) text.assign(buf, got);
      CloseHandle(f);
    }
    SessionMarker previous;
    if (ParseMarker(text, previous) && EndedWithoutWord(previous) && previous.pid != GetCurrentProcessId() &&
        !StillRunning(previous.pid)) {
      REXLOG_WARN("{}", DescribeUncleanEnd(previous, Now()));
      std::thread([pid = previous.pid, since = previous.started] { LookUpWindowsRecord(pid, since); }).detach();
    }
  }

  {
    std::lock_guard lock(g_mutex);
    wcsncpy_s(g_path, path.c_str(), _TRUNCATE);
    g_marker.pid = GetCurrentProcessId();
#if defined(KK_VERSION)
    g_marker.version = KK_VERSION;
#endif
    g_marker.role = role;
    g_marker.state = "running";
    g_marker.started = g_marker.alive = Now();
    WriteMarkerLocked();
  }

  InstallHandlers();
  std::thread(AliveThread).detach();
}

void InstallHandlers() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  std::signal(SIGABRT, OnAbortSignal);
  g_previous_terminate = std::set_terminate(OnTerminate);
  g_previous_invalid = _set_invalid_parameter_handler(OnInvalidParameter);
  g_previous_purecall = _set_purecall_handler(OnPureCall);
}

void SetRole(const char* role) {
  std::lock_guard lock(g_mutex);
  g_marker.role = role;
  if (g_marker.state == "running") WriteMarkerLocked();
}

void NoteFrame() { g_frame.store(Now(), std::memory_order_relaxed); }

void NoteCleanExit(const char* how) {
  g_ended = true;
  std::lock_guard lock(g_mutex);
  if (g_marker.state != "running") return;
  g_marker.state = std::string("clean: ") + how;
  g_marker.alive = Now();
  WriteMarkerLocked();
}

void NoteCrashReport() { SetStateNoWait("crash report written"); }

}  // namespace kk::exit_evidence

#else

namespace kk::exit_evidence {
void Install(const std::filesystem::path&, const char*) {}
void InstallHandlers() {}
void SetRole(const char*) {}
void NoteFrame() {}
void NoteCleanExit(const char*) {}
void NoteCrashReport() {}
}  // namespace kk::exit_evidence

#endif
