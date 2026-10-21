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
#include "startup_clock.h"
#include "hooks/kk_native_ui.h"
#include "backend/startup_steps.h"

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
#include <shellapi.h>
#pragma comment(lib, "dbghelp.lib")

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>
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

// Task 353: one line in the start-up step log for an exception (no allocation).
void NoteException(EXCEPTION_POINTERS* info, const char* what) {
  wchar_t where[MAX_PATH + 64];
  DescribeAddress(info->ExceptionRecord->ExceptionAddress, where, std::size(where));
  nr::steps::Note("%s: exception 0x%08lX at %ls", what, static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                  where);
}

// The crash report, once a process (crash-<time>.dmp and .txt). False when one was written already.
bool WriteCrashReportOnce(EXCEPTION_POINTERS* info, const char* reason) {
  if (InterlockedExchange(&g_crashing, 1) != 0 || !g_crash_dir[0]) return false;
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW && g_writer_wake && g_writer_done) {
    g_writer_info = info;
    g_writer_thread = GetCurrentThreadId();
    SetEvent(g_writer_wake);
    WaitForSingleObject(g_writer_done, 20000);
  } else {
    WriteCrashFiles(info, GetCurrentThreadId(), reason);
  }
  nr::steps::Note("crash report written in the crashes folder");
  exit_evidence::NoteCrashReport();
  return true;
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
  // Called again from a filter that replaced ours and keeps ours as its previous: on to the one before us.
  if (t_in_filter) return g_original_previous ? g_original_previous(info) : EXCEPTION_CONTINUE_SEARCH;
  ++t_in_filter;
  if (!g_crashing) NoteException(info, "crash (unhandled)");
  WriteCrashReportOnce(info, nullptr);
  const LONG result = g_previous && g_previous != OnCrash ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
  --t_in_filter;
  return result;
}

// Task 353: codes that always end the process (nothing handles them). A filter another library put in front of
// ours, or a C runtime that ends the program itself, would leave no report: these are written from the
// vectored handler, which runs before any of them.
bool AlwaysFatal(DWORD code) {
  return code == 0xC0000374 /* heap corruption */ || code == 0xC0000409 /* stack buffer overrun, fail-fast */ ||
         code == 0xC0000420 /* assertion failure */ || code == 0xC0000602 /* fail-fast exception */;
}

// Exceptions worth a step-log line while the program starts (they may still be handled: only noted).
bool Serious(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return true;
    default:
      return AlwaysFatal(code);
  }
}

std::atomic<ULONGLONG> g_last_frame_seen{0};  // a copy of g_last_frame for the vectored handler (declared later)
volatile LONG g_noted_exceptions = 0;
thread_local int t_in_vectored = 0;

// Registered last (after the runtime's own vectored handlers, which take the guest's expected memory faults).
LONG CALLBACK OnVectored(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (t_in_vectored || !Serious(code)) return EXCEPTION_CONTINUE_SEARCH;
  ++t_in_vectored;
  if (AlwaysFatal(code)) {
    NoteException(info, "crash (fatal)");
    WriteCrashReportOnce(info, "a fatal exception (written from the vectored handler)");
  } else if (!g_last_frame_seen.load(std::memory_order_relaxed) && InterlockedIncrement(&g_noted_exceptions) <= 16) {
    // Before the first game frame only, and a few: once the game draws, the hang and crash reports cover it.
    NoteException(info, "first-chance");
  }
  --t_in_vectored;
  return EXCEPTION_CONTINUE_SEARCH;
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
// Task 297: the renderer's shader compile clock (nr::kk::ShaderCompileMilliseconds) at g_game_start; the
// time spent compiling shaders the game waited for (no shader pack) is not counted (startup_clock.h).
std::atomic<ULONGLONG> g_compile_at_start{0};

// Where each thread is right now: suspend it just long enough to read its
// instruction pointer, and (with `stacks`) copy the top of its stack (nothing
// is resolved or allocated while it is stopped). Afterwards, code addresses
// found in that copy are listed as likely callers, newest first.
void WriteThreadList(FILE* f, bool stacks = false) {
  constexpr size_t kStackBytes = 16 * 1024;
  struct Seen {
    DWORD id;
    DWORD64 rip;
    DWORD64 start;  // task 335: where the thread began (which module made it), as Windows records it
    wchar_t name[64];
    std::vector<uint64_t> stack;
  };
  std::vector<Seen> threads;
  std::vector<uint64_t> copy(kStackBytes / 8);
  using QueryThread = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  static const auto query_thread = reinterpret_cast<QueryThread>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
                              THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    Seen s{te.th32ThreadID, 0, 0, L"", {}};
    if (query_thread) {
      PVOID start = nullptr;  // ThreadQuerySetWin32StartAddress
      if (query_thread(h, 9, &start, sizeof(start), nullptr) >= 0) s.start = reinterpret_cast<DWORD64>(start);
    }
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
    wchar_t began[MAX_PATH + 64] = L"";
    if (s.start) DescribeAddress(reinterpret_cast<const void*>(s.start), began, std::size(began));
    if (began[0])
      std::fwprintf(f, L"  thread %5lu %-32ls %ls (started at %ls)\n", s.id, s.name[0] ? s.name : L"", where, began);
    else
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
    // Task 335: likely callers in every hang report (a Beta 13 freeze report had only each thread's wait call).
    WriteThreadList(f, true);
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
  bool startup_compile_noted = false;  // task 297: the log line, once
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
      if (started && g_game_start.compare_exchange_strong(started, now))
        g_compile_at_start.store(nr::kk::ShaderCompileMilliseconds(), std::memory_order_relaxed);
      previous_tick = now;
      continue;
    }
    previous_tick = now;
    if (!last) {  // no frame yet (launcher, startup)
      const ULONGLONG started = g_game_start.load(std::memory_order_relaxed);
      if (started && !startup_reported && now - started >= kStartupHangMs) {
        // Task 297: shader compiles the game waited for (no shader pack) are not a freeze.
        const startup_clock::Window window{started, g_compile_at_start.load(std::memory_order_relaxed)};
        const uint64_t compiled = nr::kk::ShaderCompileMilliseconds();
        if (startup_clock::Reached(window, now, compiled, kStartupHangMs)) {
          startup_reported = true;
          WriteHangReport(startup_clock::CountedMs(window, now, compiled), true);
        } else if (!startup_compile_noted && startup_clock::ExtendedByCompile(window, now, compiled, kStartupHangMs)) {
          startup_compile_noted = true;
          REXLOG_INFO("KK: no first game frame yet after {} s, but {} s of it went to compiling shaders (no "
                      "shader pack): the startup hang report does not count that time",
                      (now - started) / 1000, startup_clock::CompiledMs(window, compiled) / 1000);
        }
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
  const uint64_t compiled = nr::kk::ShaderCompileMilliseconds();  // 0 before the plugin loads
  if (g_game_start.compare_exchange_strong(expected, GetTickCount64(), std::memory_order_relaxed))
    g_compile_at_start.store(compiled, std::memory_order_relaxed);
}

void NoteGuestFrame() {
  const ULONGLONG now = GetTickCount64();
  if (!g_last_frame_seen.exchange(now, std::memory_order_relaxed)) nr::steps::Note("first game frame (guest swap)");
  g_last_frame.store(now, std::memory_order_relaxed);
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

namespace {
// The folders (again when a later call names others: a benchmark's own user folder) and, once, the handlers.
void InstallHandlers(const std::filesystem::path& logs_dir, const std::filesystem::path& user_root) {
  static bool installed = false;
  std::error_code ec;
  std::filesystem::create_directories(logs_dir, ec);
  if (logs_dir.wstring().size() < MAX_PATH) wcsncpy_s(g_dir, logs_dir.wstring().c_str(), _TRUNCATE);
  const auto crash_dir = user_root / "crashes";
  ec.clear();
  std::filesystem::create_directories(crash_dir, ec);
  if (!ec && crash_dir.wstring().size() < MAX_PATH) {
    wcsncpy_s(g_crash_dir, crash_dir.wstring().c_str(), _TRUNCATE);
    SetEnvironmentVariableW(L"KK_CRASH_DIR", g_crash_dir);  // task 353: the upscaler check's dumps go here too
  } else {
    REXLOG_WARN("KK: could not prepare local crash folder (error {})", ec.value());
  }
  if (installed) {
    // Runtime setup may have installed a newer handler since the first call.
    auto previous = SetUnhandledExceptionFilter(OnCrash);
    if (previous != OnCrash && previous) g_previous = previous;
    return;
  }
  installed = true;
  SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
  g_previous = SetUnhandledExceptionFilter(OnCrash);
  g_original_previous = g_previous;
  AddVectoredExceptionHandler(0, OnVectored);  // task 353
  g_writer_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_writer_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_writer_wake && g_writer_done) {
    // Its own 1 MB stack, so the report can be written for a thread whose stack ran out.
    if (HANDLE t = CreateThread(nullptr, 1 << 20, CrashWriterThread, nullptr, 0, nullptr)) CloseHandle(t);
  }
}

// Task 353: logs\startup-steps.txt for this process and its children (KK_STEP_LOG). Kept under 1 MB: the
// older lines move to startup-steps-old.txt. The startup monitor (startup_recovery.cpp) learns the path
// through KK_MONITOR_NOTE, so it can add how this process ended.
void OpenStepLog(const std::filesystem::path& logs_dir) {
  static bool opened = false;
  if (opened) return;
  opened = true;
  const std::filesystem::path path = logs_dir / "startup-steps.txt";
  if (path.wstring().size() >= MAX_PATH) return;
  std::error_code ec;
  if (std::filesystem::file_size(path, ec) > (1u << 20) && !ec)
    MoveFileExW(path.c_str(), (logs_dir / "startup-steps-old.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
  SetEnvironmentVariableW(L"KK_STEP_LOG", path.c_str());
  wchar_t note[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"KK_MONITOR_NOTE", note, MAX_PATH);
  if (n && n < MAX_PATH) {
    HANDLE f = CreateFileW(note, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      const std::wstring text = path.wstring();
      DWORD written = 0;
      WriteFile(f, text.data(), DWORD(text.size() * sizeof(wchar_t)), &written, nullptr);
      CloseHandle(f);
    }
    SetEnvironmentVariableW(L"KK_MONITOR_NOTE", nullptr);  // this process's own children have their own monitor
  }
  // The switches given (names only, never a value: values can hold folder names).
  std::string flags;
  int argc = 0;
  if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      std::wstring a = argv[i];
      a = a.substr(0, a.find(L'='));
      if (a.rfind(L"--", 0) == 0 && flags.size() < 400) flags += " " + std::string(a.begin() + 2, a.end());
    }
    LocalFree(argv);
  }
  wchar_t diagnostics[8] = L"", skip[8] = L"";
  GetEnvironmentVariableW(L"KK_DIAGNOSTICS", diagnostics, 8);
  GetEnvironmentVariableW(L"KK_SKIP_UPSCALER_CHECK", skip, 8);
  nr::steps::Note("---- king_kong %s started (diagnostics %s, upscaler check %s); switches:%s", KK_VERSION,
                  diagnostics[0] == L'1' ? "on" : "off", skip[0] == L'1' ? "skipped" : "on",
                  flags.empty() ? " none" : flags.c_str());
}
}  // namespace

void InstallEarlyCrashHandlers(const std::filesystem::path& logs_dir, const std::filesystem::path& user_root) {
  InstallHandlers(logs_dir, user_root);
  OpenStepLog(logs_dir);
#if defined(KK_DEV_TOOLS)
  // Developer test (task 353): KK_DEV_FAULT=av|fastfail|heap|abort ends this process that way on a background
  // thread half a second from now, while the window and graphics device are being made (as the Intel UHD
  // reports did), to check what the handlers and the step log leave behind.
  if (const char* fault = std::getenv("KK_DEV_FAULT"); fault && *fault) {
    const std::string kind = fault;
    std::thread([kind] {
      Sleep(500);
      nr::steps::Note("dev: injected fault %s", kind.c_str());
      if (kind == "av") *static_cast<volatile int*>(nullptr) = 1;
      else if (kind == "fastfail") __fastfail(7);
      else if (kind == "heap") RaiseException(0xC0000374, EXCEPTION_NONCONTINUABLE, 0, nullptr);
      else if (kind == "abort") std::abort();
    }).detach();
  }
#endif
}

void InstallCrashDumps(const std::filesystem::path& logs_dir, const std::filesystem::path& user_root) {
  InstallHandlers(logs_dir, user_root);
  OpenStepLog(logs_dir);
  static bool watching = false;
  if (watching) return;
  watching = true;
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
void InstallEarlyCrashHandlers(const std::filesystem::path&, const std::filesystem::path&) {}
}  // namespace kk

#endif
