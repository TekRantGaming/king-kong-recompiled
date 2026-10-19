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
#include "exit_evidence.h"

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
// The filter that was there before ours was first installed (KeepFilterInFront may change g_previous).
LPTOP_LEVEL_EXCEPTION_FILTER g_original_previous = nullptr;
volatile LONG g_crashing = 0;
std::atomic<int> g_api{0};  // task 270: SetCrashReportApi

const char* ApiName() {
  const int api = g_api.load(std::memory_order_relaxed);
  return api == 1 ? "vulkan" : api == 2 ? "d3d12" : nullptr;
}

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

void WriteMiniDump(const wchar_t* base, EXCEPTION_POINTERS* info, DWORD thread = GetCurrentThreadId()) {
  wchar_t path[MAX_PATH + 80];
  swprintf(path, MAX_PATH + 80, L"%ls.dmp", base);
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  MINIDUMP_EXCEPTION_INFORMATION mei{thread, info, FALSE};
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

// The faulting address and up to 16 callers, as "module + 0xoffset" (x64 unwind data; recompiled code
// without unwind info ends the walk). A damaged stack only ends the list early.
void WriteCrashStack(FILE* f, EXCEPTION_POINTERS* info) {
  wchar_t where[MAX_PATH + 64];
  DescribeAddress(info->ExceptionRecord->ExceptionAddress, where, std::size(where));
  std::fprintf(f, "Stack:\n  %ls\n", where);
#if defined(_M_X64) || defined(__x86_64__)
  CONTEXT ctx = *info->ContextRecord;
  for (int i = 0; i < 16; ++i) {
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
    if (!fn) break;  // a leaf, or code without unwind data: stop here
    void* handler_data = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
    if (!ctx.Rip) break;
    DescribeAddress(reinterpret_cast<const void*>(ctx.Rip), where, std::size(where));
    std::fprintf(f, "  %ls\n", where);
  }
#endif
}

// crash-<time>.dmp and .txt. `thread`: the thread that crashed (this one, or the one the writer thread
// works for). `reason`: why the program ends when no exception was raised (abort(), std::terminate).
void WriteCrashFiles(EXCEPTION_POINTERS* info, DWORD thread, const char* reason) {
  wchar_t base[MAX_PATH + 64];
  ReportBase(L"crash", base);
  WriteMiniDump(base, info, thread);
  wchar_t path[MAX_PATH + 80];
  swprintf(path, MAX_PATH + 80, L"%ls.txt", base);
  if (FILE* f = _wfopen(path, L"wb")) {
    const auto* rec = info->ExceptionRecord;
    // Addresses suffice for symbolication; never include a module's full path.
    std::fprintf(f, "King Kong crashed.\nException 0x%08X at %p\nThread %lu\n",
                 rec->ExceptionCode, rec->ExceptionAddress, thread);
    if (reason) std::fprintf(f, "Reason: %s\n", reason);
    if (const char* api = ApiName()) std::fprintf(f, "Graphics API: %s\n", api);
    // Module + offset (with king_kong.map, the game function), and the caller chain from the unwind
    // tables: the server groups identical crashes by the code and the first three of these.
    WriteCrashStack(f, info);
    std::fputs("\n", f);
    WriteCrashDiagnostics(f);
    // What the game was doing: the last 200 log lines (fixed memory, redacted when they were logged).
    std::fputs("\n", f);
    WriteLogTail(f);
    std::fclose(f);
  }
}

// Task 229: a stack overflow leaves the crashing thread almost no stack, too little for the minidump
// writer and the report, so a thread made up front writes them while the crashed one waits.
HANDLE g_writer_wake = nullptr, g_writer_done = nullptr;
EXCEPTION_POINTERS* volatile g_writer_info = nullptr;
volatile DWORD g_writer_thread = 0;

DWORD WINAPI CrashWriterThread(void*) {
  WaitForSingleObject(g_writer_wake, INFINITE);
  WriteCrashFiles(g_writer_info, g_writer_thread, "stack overflow");
  SetEvent(g_writer_done);
  return 0;
}

// Nested calls on one thread (another filter we call calls ours again) go straight on.
thread_local int t_in_filter = 0;

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
  // Called again from a filter that replaced ours and keeps ours as its previous: on to the one before us.
  if (t_in_filter) return g_original_previous ? g_original_previous(info) : EXCEPTION_CONTINUE_SEARCH;
  ++t_in_filter;
  if (InterlockedExchange(&g_crashing, 1) == 0 && g_crash_dir[0]) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW && g_writer_wake && g_writer_done) {
      g_writer_info = info;
      g_writer_thread = GetCurrentThreadId();
      SetEvent(g_writer_wake);
      WaitForSingleObject(g_writer_done, 20000);
    } else {
      WriteCrashFiles(info, GetCurrentThreadId(), nullptr);
    }
    exit_evidence::NoteCrashReport();
  }
  const LONG result = g_previous && g_previous != OnCrash ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
  --t_in_filter;
  return result;
}

// Task 229: a library loaded after us (an overlay's DLL in the Vulkan layer chain, a driver) may install its
// own unhandled-exception filter, and then a crash never reached ours (no crash file). Called every two
// seconds: ours goes back in front and calls theirs after writing the report.
void KeepFilterInFront() {
  const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(OnCrash);
  if (current == OnCrash || current == nullptr) return;
  g_previous = current;
  static int logged = 0;
  if (++logged > 3) return;
  HMODULE module = nullptr;
  wchar_t name[MAX_PATH] = L"?";
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(current), &module))
    GetModuleFileNameW(module, name, MAX_PATH);
  const wchar_t* slash = wcsrchr(name, L'\\');
  REXLOG_WARN("KK: {} had replaced the crash report handler; ours is first again and calls it after",
              std::filesystem::path(slash ? slash + 1 : name).string());
}

// ------------------------------------------------------------------ hangs ---

constexpr ULONGLONG kHangMs = 20000;
std::atomic<ULONGLONG> g_last_frame{0};  // GetTickCount64 at the last guest frame; 0 before the first
// Task 285: GetTickCount64 when the game started in this process (NoteGameStarting); 0 = not started. With
// no first frame kStartupHangMs later, one startup hang report says where every thread waits.
constexpr ULONGLONG kStartupHangMs = 60000;
std::atomic<ULONGLONG> g_game_start{0};

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

// startup (task 285): no first frame yet since the game started; the thread list then has likely callers.
void WriteHangReport(ULONGLONG stalled_ms, bool startup = false) {
  wchar_t base[MAX_PATH + 64];
  ReportBase(startup ? L"hang-startup" : L"hang", base);
  wchar_t path[MAX_PATH + 80];
  swprintf(path, MAX_PATH + 80, L"%ls.txt", base);
  if (FILE* f = _wfopen(path, L"w")) {
    std::fwprintf(f, startup ? L"King Kong started the game but drew no first frame in %llu seconds (a freeze at startup).\n"
                             : L"King Kong stopped drawing frames for %llu seconds (a freeze).\n",
                  stalled_ms / 1000);
    std::fwprintf(f, L"Version: " KK_VERSION_W L"\n"
                     L"Please attach this file, the .dmp file next to it and logs\\king_kong.log to a bug report.\n\n"
                     L"Threads:\n");
    if (const char* api = ApiName()) std::fwprintf(f, L"Graphics API: %hs\n", api);
    WriteThreadList(f, startup);
    std::fputs("\n", f);
    WriteLogTail(f);
    std::fclose(f);
  }
  WriteMiniDump(base, nullptr);
  if (startup)
    REXLOG_WARN("KK: no first game frame {} s after the game started; wrote startup hang report {} (where every "
                "thread waits)", stalled_ms / 1000, std::filesystem::path(base).filename().string());
  else
    REXLOG_WARN("KK: no frame for {} s, wrote hang report {}", stalled_ms / 1000,
                std::filesystem::path(base).filename().string());
}

void WatchForHangs() {
  ULONGLONG previous_tick = GetTickCount64();
  ULONGLONG diagnostics_tick = previous_tick;
  ULONGLONG filter_tick = previous_tick;
  ULONGLONG reported_at = 0;  // g_last_frame value the current report is about
  ULONGLONG hitch_at = 0;     // ... and the current hitch report
  bool startup_reported = false;  // task 285: once a process
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
    if (now - filter_tick >= 2000) {
      KeepFilterInFront();
      filter_tick = now;
    }
    const ULONGLONG last = g_last_frame.load(std::memory_order_relaxed);
    if (now - previous_tick > 5000) {
      // This thread did not run for a while: the PC slept or the process was
      // paused (e.g. a debugger). Not a game freeze; start counting again.
      ULONGLONG expected = last;
      if (last) g_last_frame.compare_exchange_strong(expected, now);
      ULONGLONG started = g_game_start.load(std::memory_order_relaxed);
      if (started) g_game_start.compare_exchange_strong(started, now);
      previous_tick = now;
      continue;
    }
    previous_tick = now;
    if (!last) {  // no frame yet (launcher, startup)
      const ULONGLONG started = g_game_start.load(std::memory_order_relaxed);
      if (started && !startup_reported && now - started >= kStartupHangMs) {
        startup_reported = true;
        WriteHangReport(now - started, true);
      }
      continue;
    }
#if defined(KK_DEV_TOOLS)
    if (!test_started) test_started = now;
    if (test_seconds > 0 && now - test_started >= test_seconds * 1000) {
      REXLOG_ERROR("KK dev: forced crash after {} seconds; test path C:\\Users\\PrivacyProbe\\private.txt", test_seconds);
      RaiseException(0xE0430001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
#endif
    const int32_t hitch_ms = REXCVAR_GET(kk_hitch_report_ms);
    const ULONGLONG stalled_ms = FrameStallMilliseconds(now, last);
    if (hitch_ms > 0 && last != hitch_at && stalled_ms >= ULONGLONG(hitch_ms)) {
      hitch_at = last;
      WriteHitchReport(stalled_ms);
    }
    if (reported_at) {
      if (last != reported_at) {
        REXLOG_WARN("KK: the game is drawing again after a {} s freeze", FrameStallMilliseconds(last, reported_at) / 1000);
        reported_at = 0;
      }
    } else if (stalled_ms >= kHangMs) {
      reported_at = last;
      WriteHangReport(stalled_ms);
    }
  }
}

}  // namespace

void SetCrashReportApi(int api) { g_api.store(api, std::memory_order_relaxed); }

void NoteGameStarting() {
  ULONGLONG expected = 0;
  g_game_start.compare_exchange_strong(expected, GetTickCount64(), std::memory_order_relaxed);
}

void NoteGuestFrame() {
  g_last_frame.store(GetTickCount64(), std::memory_order_relaxed);
  exit_evidence::NoteFrame();
}

void WriteCrashReportNow(const char* reason) {
  if (InterlockedExchange(&g_crashing, 1) != 0 || !g_crash_dir[0]) return;
  // No exception was raised (abort(), std::terminate, an invalid C runtime argument): a made-up record
  // (STATUS_FATAL_APP_EXIT, as abort() reports) and this thread's registers, so the stack shows the caller.
  CONTEXT context{};
  RtlCaptureContext(&context);
  EXCEPTION_RECORD record{};
  record.ExceptionCode = 0x40000015;  // STATUS_FATAL_APP_EXIT
  record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
  EXCEPTION_POINTERS info{&record, &context};
  WriteCrashFiles(&info, GetCurrentThreadId(), reason);
}

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
  g_original_previous = g_previous;
  g_writer_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_writer_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_writer_wake && g_writer_done) {
    // Its own 1 MB stack, so the report can be written for a thread whose stack ran out.
    if (HANDLE t = CreateThread(nullptr, 1 << 20, CrashWriterThread, nullptr, 0, nullptr)) CloseHandle(t);
  }
  std::thread(WatchForHangs).detach();
}

}  // namespace kk

#else

namespace kk {
void NoteGuestFrame() {}
void NoteGameStarting() {}
void SetCrashReportApi(int) {}
void WriteCrashReportNow(const char*) {}
void InstallCrashDumps(const std::filesystem::path&, const std::filesystem::path&) {}
}  // namespace kk

#endif
