// Crash reports: write user_root/crashes/crash-<time>.dmp (a
// minidump for debugging) and crash-<time>.txt (redacted diagnostics)
// so players have something to attach to a bug report. Windows only.
//
// Hang reports: a watchdog thread notices when the game has drawn no frame for
// 20 seconds and writes logs/hang-<time>.dmp and .txt (where every thread was),
// so a freeze leaves something to debug too. The game keeps running; if it
// recovers, the log says how long it stalled.

#include "crash_dump.h"
#include "diagnostics.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <thread>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "settings.h"

namespace kk {
namespace {

wchar_t g_dir[MAX_PATH];  // hang/hitch folder, prepared up front
wchar_t g_crash_dir[MAX_PATH];
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
volatile LONG g_crashing = 0;

// logs\<kind>-<time> (no extension).
void ReportBase(const wchar_t* kind, wchar_t (&base)[MAX_PATH + 64]) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  if (wcscmp(kind, L"crash") == 0)
    swprintf(base, MAX_PATH + 64, L"%ls\\%ls-%04u%02u%02u-%02u%02u%02u-%03u-%lu", g_crash_dir, kind, t.wYear, t.wMonth,
             t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
  else
    swprintf(base, MAX_PATH + 64, L"%ls\\%ls-%04u%02u%02u-%02u%02u%02u", g_dir, kind, t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond);
}

void WriteMiniDump(const wchar_t* base, EXCEPTION_POINTERS* info) {
  wchar_t path[MAX_PATH + 80];
  swprintf(path, MAX_PATH + 80, L"%ls.dmp", base);
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
  MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                    MINIDUMP_TYPE(MiniDumpWithThreadInfo | (info ? MiniDumpNormal : MiniDumpWithIndirectlyReferencedMemory)),
                    info ? &mei : nullptr, nullptr, nullptr);
  CloseHandle(file);
}

// "king_kong.exe + 0x1234" for a code address.
void DescribeAddress(const void* address, wchar_t* out, size_t out_size) {
  HMODULE module = nullptr;
  wchar_t name[MAX_PATH] = L"?";
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(address), &module) &&
      GetModuleFileNameW(module, name, MAX_PATH)) {
    const wchar_t* slash = wcsrchr(name, L'\\');
    swprintf(out, out_size, L"%ls + 0x%llX", slash ? slash + 1 : name,
             static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
  } else {
    swprintf(out, out_size, L"%p", address);
  }
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
  if (InterlockedExchange(&g_crashing, 1) == 0 && g_crash_dir[0]) {
    wchar_t base[MAX_PATH + 64];
    ReportBase(L"crash", base);
    WriteMiniDump(base, info);
    wchar_t path[MAX_PATH + 80];
    swprintf(path, MAX_PATH + 80, L"%ls.txt", base);
    if (FILE* f = _wfopen(path, L"wb")) {
      const auto* rec = info->ExceptionRecord;
      // Addresses suffice for symbolication; never include a module's full path.
      std::fprintf(f, "King Kong crashed.\nException 0x%08X at %p\nThread %lu\n\n",
                   rec->ExceptionCode, rec->ExceptionAddress, GetCurrentThreadId());
      WriteCrashDiagnostics(f);
      std::fclose(f);
    }
  }
  return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------ hangs ---

constexpr ULONGLONG kHangMs = 20000;
std::atomic<ULONGLONG> g_last_frame{0};  // GetTickCount64 at the last guest frame; 0 before the first

// Where each thread is right now: suspend it just long enough to read its
// instruction pointer, and (with `stacks`) copy the top of its stack (nothing
// is resolved or allocated while it is stopped). Afterwards, code addresses
// found in that copy are listed as likely callers, newest first.
void WriteThreadList(FILE* f, bool stacks = false) {
  constexpr size_t kStackBytes = 16 * 1024;
  struct Seen {
    DWORD id;
    DWORD64 rip;
    wchar_t name[64];
    std::vector<uint64_t> stack;
  };
  std::vector<Seen> threads;
  std::vector<uint64_t> copy(kStackBytes / 8);
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                          te.th32ThreadID);
    if (!h) continue;
    Seen s{te.th32ThreadID, 0, L"", {}};
    size_t copied = 0;
    if (SuspendThread(h) != DWORD(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(h, &ctx)) {
        s.rip = ctx.Rip;
        if (stacks) {
          SIZE_T got = 0;
          ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ctx.Rsp), copy.data(), kStackBytes,
                            &got);
          copied = got / 8;
        }
      }
      ResumeThread(h);
    }
    s.stack.assign(copy.begin(), copy.begin() + copied);
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
      wcsncpy_s(s.name, desc, _TRUNCATE);
      LocalFree(desc);
    }
    CloseHandle(h);
    threads.push_back(s);
  }
  CloseHandle(snap);
  for (const Seen& s : threads) {
    wchar_t where[MAX_PATH + 64];
    DescribeAddress(reinterpret_cast<const void*>(s.rip), where, std::size(where));
    std::fwprintf(f, L"  thread %5lu %-32ls %ls\n", s.id, s.name[0] ? s.name : L"", where);
    int shown = 0;
    for (uint64_t v : s.stack) {
      if (shown >= 12) break;
      HMODULE module = nullptr;
      if (v < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                             reinterpret_cast<LPCWSTR>(v), &module))
        continue;
      DescribeAddress(reinterpret_cast<const void*>(v), where, std::size(where));
      std::fwprintf(f, L"        from %ls\n", where);
      ++shown;
    }
  }
}

// kk_hitch_report_ms: when a frame takes longer than this, write
// logs/hitch-<time>.txt with where every thread was (diagnostics; 0 = off).
void WriteHitchReport(ULONGLONG stalled_ms) {
  static int written = 0;
  if (++written > 40) return;
  wchar_t base[MAX_PATH + 64];
  ReportBase(L"hitch", base);
  wchar_t path[MAX_PATH + 80];
  swprintf(path, MAX_PATH + 80, L"%ls-%llu.txt", base, stalled_ms);
  if (FILE* f = _wfopen(path, L"w")) {
    std::fwprintf(f, L"No frame for %llu ms. Threads (with likely callers):\n", stalled_ms);
    WriteThreadList(f, true);
    std::fclose(f);
  }
  REXLOG_WARN("KK: no frame for {} ms, wrote {}", stalled_ms, std::filesystem::path(path).filename().string());
}

void WriteHangReport(ULONGLONG stalled_ms) {
  wchar_t base[MAX_PATH + 64];
  ReportBase(L"hang", base);
  wchar_t path[MAX_PATH + 80];
  swprintf(path, MAX_PATH + 80, L"%ls.txt", base);
  if (FILE* f = _wfopen(path, L"w")) {
    std::fwprintf(f, L"King Kong stopped drawing frames for %llu seconds (a freeze).\n"
                     L"Please attach this file, the .dmp file next to it and logs\\king_kong.log to a bug report.\n\n"
                     L"Threads:\n",
                  stalled_ms / 1000);
    WriteThreadList(f);
    std::fclose(f);
  }
  WriteMiniDump(base, nullptr);
  REXLOG_WARN("KK: no frame for {} s, wrote hang report {}", stalled_ms / 1000,
              std::filesystem::path(base).filename().string());
}

void WatchForHangs() {
  ULONGLONG previous_tick = GetTickCount64();
  ULONGLONG diagnostics_tick = previous_tick;
  ULONGLONG reported_at = 0;  // g_last_frame value the current report is about
  ULONGLONG hitch_at = 0;     // ... and the current hitch report
#if defined(KK_DEV_TOOLS)
  // Wall time from the first guest frame, so the crash test lasts two minutes
  // of rendering and does not depend on menu input or compiler null-write UB.
  ULONGLONG test_started = 0;
  const char* crash_after = std::getenv("KK_DEV_CRASH_AFTER");
  const double test_seconds = crash_after ? std::atof(crash_after) : 0;
#endif
  for (;;) {
    Sleep(100);
    const ULONGLONG now = GetTickCount64();
    if (now - diagnostics_tick >= 1000) {
      try { RefreshDiagnostics(); } catch (...) { /* Retain the last complete snapshot. */ }
      diagnostics_tick = now;
    }
    const ULONGLONG last = g_last_frame.load(std::memory_order_relaxed);
    if (now - previous_tick > 5000) {
      // This thread did not run for a while: the PC slept or the process was
      // paused (e.g. a debugger). Not a game freeze; start counting again.
      ULONGLONG expected = last;
      if (last) g_last_frame.compare_exchange_strong(expected, now);
      previous_tick = now;
      continue;
    }
    previous_tick = now;
    if (!last) continue;  // no frame yet (launcher, startup)
#if defined(KK_DEV_TOOLS)
    if (!test_started) test_started = now;
    if (test_seconds > 0 && now - test_started >= test_seconds * 1000) {
      REXLOG_ERROR("KK dev: forced crash after {} seconds; test path C:\\Users\\PrivacyProbe\\private.txt", test_seconds);
      RaiseException(0xE0430001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
#endif
    const int32_t hitch_ms = REXCVAR_GET(kk_hitch_report_ms);
    if (hitch_ms > 0 && last != hitch_at && now - last >= ULONGLONG(hitch_ms)) {
      hitch_at = last;
      WriteHitchReport(now - last);
    }
    if (reported_at) {
      if (last != reported_at) {
        REXLOG_WARN("KK: the game is drawing again after a {} s freeze", (last - reported_at) / 1000);
        reported_at = 0;
      }
    } else if (now - last >= kHangMs) {
      reported_at = last;
      WriteHangReport(now - last);
    }
  }
}

}  // namespace

void NoteGuestFrame() { g_last_frame.store(GetTickCount64(), std::memory_order_relaxed); }

void InstallCrashDumps(const std::filesystem::path& logs_dir, const std::filesystem::path& user_root) {
  static bool installed = false;
  if (installed) {
    // Runtime setup may have installed a newer handler since path finalization.
    auto previous = SetUnhandledExceptionFilter(OnCrash);
    if (previous != OnCrash) g_previous = previous;
    return;
  }
  installed = true;
  SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
  std::error_code ec;
  std::filesystem::create_directories(logs_dir, ec);
  wcsncpy_s(g_dir, logs_dir.wstring().c_str(), _TRUNCATE);
  const auto crash_dir = user_root / "crashes";
  std::filesystem::create_directories(crash_dir, ec);
  if (!ec && crash_dir.wstring().size() < MAX_PATH)
    wcsncpy_s(g_crash_dir, crash_dir.wstring().c_str(), _TRUNCATE);
  else REXLOG_WARN("KK: could not prepare local crash folder (error {})", ec.value());
  g_previous = SetUnhandledExceptionFilter(OnCrash);
  std::thread(WatchForHangs).detach();
}

}  // namespace kk

#else

namespace kk {
void NoteGuestFrame() {}
void InstallCrashDumps(const std::filesystem::path&, const std::filesystem::path&) {}
}  // namespace kk

#endif
