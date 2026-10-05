// Crash reports: if the game crashes outright, write logs/crash-<time>.dmp (a
// minidump for debugging) and logs/crash-<time>.txt (what happened, in short)
// so players have something to attach to a bug report. Windows only.

#include "crash_dump.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <cstdio>
#include <cwchar>
#include <filesystem>

#include <rex/filesystem.h>
#include <rex/logging.h>

namespace kk {
namespace {

wchar_t g_dir[MAX_PATH];  // logs folder, prepared up front (no allocation while crashing)
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
volatile LONG g_crashing = 0;

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
  if (InterlockedExchange(&g_crashing, 1) == 0 && g_dir[0]) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t base[MAX_PATH + 64];
    swprintf(base, MAX_PATH + 64, L"%ls\\crash-%04u%02u%02u-%02u%02u%02u", g_dir, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
    wchar_t path[MAX_PATH + 80];
    swprintf(path, MAX_PATH + 80, L"%ls.dmp", base);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                        MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &mei, nullptr,
                        nullptr);
      CloseHandle(file);
    }
    swprintf(path, MAX_PATH + 80, L"%ls.txt", base);
    if (FILE* f = _wfopen(path, L"w")) {
      const auto* rec = info->ExceptionRecord;
      HMODULE module = nullptr;
      wchar_t name[MAX_PATH] = L"?";
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCWSTR>(rec->ExceptionAddress), &module))
        GetModuleFileNameW(module, name, MAX_PATH);
      std::fwprintf(f, L"King Kong crashed.\nException 0x%08X at %p (%ls + 0x%llX)\nThread %lu\n"
                       L"Please attach this file, the .dmp file next to it and logs\\king_kong.log to a bug report.\n",
                    rec->ExceptionCode, rec->ExceptionAddress, name,
                    static_cast<unsigned long long>(static_cast<const char*>(rec->ExceptionAddress) -
                                                    reinterpret_cast<const char*>(module)),
                    GetCurrentThreadId());
      std::fclose(f);
    }
  }
  return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashDumps(const std::filesystem::path& logs_dir) {
  std::error_code ec;
  std::filesystem::create_directories(logs_dir, ec);
  wcsncpy_s(g_dir, logs_dir.wstring().c_str(), _TRUNCATE);
  g_previous = SetUnhandledExceptionFilter(OnCrash);
}

}  // namespace kk

#else

namespace kk {
void InstallCrashDumps(const std::filesystem::path&) {}
}  // namespace kk

#endif
