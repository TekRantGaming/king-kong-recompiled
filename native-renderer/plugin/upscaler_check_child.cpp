// Task 353: the upscaler check's own process. king_kong.exe started with --kk_upscaler_check=<report> (by the
// launcher's question, upscaler_query.cpp) loads this plugin before anything else of the game runs and calls
// nr_upscaler_check_main: a Direct3D 12 device on the game's graphics card, then each upscaler SDK asked in
// turn whether it runs here (FSR 3.1, FSR 4, DLSS, XeSS). Each step is written to the report before and after
// it (no buffer in this process), so when an SDK or the driver ends this process (a crash, a fail-fast, a
// hang the launcher stops) the report's last line names it, and the launcher marks that SDK and those not
// reached as unavailable. Nothing of the game, its window or its settings is involved.
//
// Report lines: "begin <step>", "end <step> <1|0> <why>", "vendor <hex>", "note <SDK log line>",
// "exception ..." / "crash ..." (from the handlers below), "done".

#include "plugin/upscaler_query.h"

#if NR_GAME_RENDERER && NR_HAS_D3D12

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dbghelp.h>
#include <dxgi1_6.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>

#include <nvrhi/d3d12.h>

#include "backend/log.h"
#include "backend/startup_steps.h"
#include "backend/upscaler.h"

namespace nr {

namespace {

HANDLE g_report = nullptr;   // opened for appending only: whole lines from any thread, no lock
char g_step[32] = "start";   // the step running now, for the crash line
volatile LONG g_crashed = 0;
volatile LONG g_exceptions = 0;

void Line(const char* format, ...) {
  char line[1200];
  va_list args;
  va_start(args, format);
  int n = std::vsnprintf(line, sizeof(line) - 2, format, args);
  va_end(args);
  if (n < 0) return;
  if (n > int(sizeof(line)) - 3) n = int(sizeof(line)) - 3;
  for (int i = 0; i < n; ++i)
    if (line[i] == '\r' || line[i] == '\n') line[i] = ' ';
  line[n] = 0;
  steps::Note("upscaler check process: %s", line);
  line[n++] = '\r';
  line[n++] = '\n';
  if (g_report) {
    DWORD written = 0;
    WriteFile(g_report, line, DWORD(n), &written, nullptr);
  }
}

void ToReport(LogLevel, const char* text) { Line("note %s", text); }

// "libxess.dll + 0x1234" for a code address.
void Where(const void* address, char* out, size_t size) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(address), &module) &&
      GetModuleFileNameA(module, name, MAX_PATH)) {
    const char* slash = std::strrchr(name, '\\');
    std::snprintf(out, size, "%s + 0x%llX", slash ? slash + 1 : name,
                  static_cast<unsigned long long>(static_cast<const char*>(address) -
                                                  reinterpret_cast<const char*>(module)));
  } else {
    std::snprintf(out, size, "%p", address);
  }
}

// A minidump in the game's crash folder (KK_CRASH_DIR, set by the launcher's process): upscaler-check-<time>.dmp.
void WriteDump(EXCEPTION_POINTERS* info) {
  wchar_t dir[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"KK_CRASH_DIR", dir, MAX_PATH);
  if (!n || n >= MAX_PATH - 64) return;
  using Write = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                              PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
  HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
  const auto write = dbghelp ? reinterpret_cast<Write>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
  if (!write) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t path[MAX_PATH + 80];
  std::swprintf(path, MAX_PATH + 80, L"%ls\\upscaler-check-%04u%02u%02u-%02u%02u%02u-%lu.dmp", dir, t.wYear, t.wMonth,
                t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
  const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file,
                        MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                        info ? &mei : nullptr, nullptr, nullptr);
  CloseHandle(file);
  const wchar_t* name = std::wcsrchr(path, L'\\');
  Line("crash dump %ls (%s)", name ? name + 1 : path, ok ? "written" : "failed");
}

void ReportCrash(EXCEPTION_POINTERS* info, const char* how) {
  if (InterlockedExchange(&g_crashed, 1)) return;
  char where[MAX_PATH + 64];
  Where(info->ExceptionRecord->ExceptionAddress, where, sizeof(where));
  Line("crash exception 0x%08lX at %s during %s (%s)", info->ExceptionRecord->ExceptionCode, where, g_step, how);
  WriteDump(info);
}

// Codes that always end the process: reported at once (an unhandled-exception filter may never see them).
bool AlwaysFatal(DWORD code) {
  return code == 0xC0000374 /* heap corruption */ || code == 0xC0000409 /* stack buffer overrun, fail-fast */ ||
         code == 0xC0000420 /* assertion failure */ || code == 0xC0000602 /* fail-fast exception */ ||
         code == EXCEPTION_STACK_OVERFLOW;
}

bool Serious(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return true;
    default:
      return AlwaysFatal(code);
  }
}

LONG CALLBACK OnVectored(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (!Serious(code)) return EXCEPTION_CONTINUE_SEARCH;
  if (InterlockedIncrement(&g_exceptions) <= 8) {
    char where[MAX_PATH + 64];
    Where(info->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    Line("exception 0x%08lX at %s during %s (first chance)", code, where, g_step);
  }
  if (AlwaysFatal(code)) ReportCrash(info, "fatal");
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info) {
  ReportCrash(info, "unhandled");
  TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER;
}

void OnAbort(int) {
  Line("crash abort() during %s", g_step);
  TerminateProcess(GetCurrentProcess(), 0xC0000409);
}

void PureCall() {
  Line("crash pure virtual call during %s", g_step);
  TerminateProcess(GetCurrentProcess(), 0xC0000409);
}

void InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
  Line("crash invalid C runtime argument during %s", g_step);
  TerminateProcess(GetCurrentProcess(), 0xC0000409);
}

// Tests (task 353): KK_UPSCALER_CHECK_TEST=<step>=<crash|hang|fastfail|abort>, e.g. xess=crash, makes this process
// fail in that way at the start of that step. Only this process reads it.
void TestFault(const char* step) {
  char v[64] = "";
  if (!GetEnvironmentVariableA("KK_UPSCALER_CHECK_TEST", v, sizeof(v))) return;
  const size_t len = std::strlen(step);
  if (std::strncmp(v, step, len) != 0 || v[len] != '=') return;
  const char* kind = v + len + 1;
  Line("note test: %s at %s", kind, step);
  if (!std::strcmp(kind, "crash")) {
    *static_cast<volatile int*>(nullptr) = 1;
  } else if (!std::strcmp(kind, "hang")) {
    Sleep(INFINITE);
  } else if (!std::strcmp(kind, "fastfail")) {
    __fastfail(7 /* FAST_FAIL_FATAL_APP_EXIT */);
  } else if (!std::strcmp(kind, "abort")) {
    std::abort();
  }
}

void Begin(const char* step) {
  std::snprintf(g_step, sizeof(g_step), "%s", step);
  Line("begin %s", step);
  TestFault(step);
}

}  // namespace

int UpscalerCheckMain(const wchar_t* report, const wchar_t* adapter) {
  SetUpscalerGate(nullptr);  // this process is the check: every SDK may load here
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  if (!report || !*report) return 3;
  HANDLE created = CreateFileW(report, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
  if (created == INVALID_HANDLE_VALUE) return 3;
  CloseHandle(created);
  g_report = CreateFileW(report, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_report == INVALID_HANDLE_VALUE) {
    g_report = nullptr;
    return 3;
  }
  ULONG guarantee = 64 * 1024;  // room for the handlers after a stack overflow
  SetThreadStackGuarantee(&guarantee);
  AddVectoredExceptionHandler(0, OnVectored);
  SetUnhandledExceptionFilter(OnUnhandled);
  std::signal(SIGABRT, OnAbort);
  _set_purecall_handler(PureCall);
  _set_invalid_parameter_handler(InvalidParameter);
  SetLogSink(ToReport);
  Line("kk-upscaler-check 1 (process %lu)", GetCurrentProcessId());

  Begin("device");
  LUID luid{};
  {
    unsigned long high = 0, low = 0;
    if (!adapter || std::swscanf(adapter, L"%lx:%lx", &high, &low) != 2) {
      Line("end device 0 no graphics card given");
      return 4;
    }
    luid.HighPart = LONG(high);
    luid.LowPart = DWORD(low);
  }
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  std::string name;
  {
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* chosen = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
      factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&chosen));
      factory->Release();
    }
    if (!chosen) {
      Line("end device 0 the graphics card was not found");
      return 4;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(chosen->GetDesc1(&desc))) {
      char text[256];
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, text, sizeof(text), nullptr, nullptr);
      name = text;
    }
    // The lowest feature level any of the SDKs could run at; each SDK's own check decides.
    for (const D3D_FEATURE_LEVEL level :
         {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}) {
      if (SUCCEEDED(D3D12CreateDevice(chosen, level, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device))))
        break;
    }
    chosen->Release();
  }
  if (device) {
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))))
      queue = nullptr;
  }
  if (!device || !queue) {
    Line("end device 0 no Direct3D 12 device on %s", name.c_str());
    return 4;
  }
  struct Quiet final : nvrhi::IMessageCallback {
    void message(nvrhi::MessageSeverity severity, const char* text) override {
      if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal)
        Line("note NVRHI: %s", text);
    }
  };
  static Quiet quiet;
  nvrhi::d3d12::DeviceDesc dd;
  dd.errorCB = &quiet;
  dd.pDevice = device;
  dd.pGraphicsCommandQueue = queue;
  nvrhi::DeviceHandle nvrhi_device = nvrhi::d3d12::createDevice(dd);
  if (!nvrhi_device) {
    Line("end device 0 no NVRHI device on %s", name.c_str());
    return 4;
  }
  Line("end device 1 %s", name.c_str());
  Line("vendor %x", UpscalerDeviceVendor(nvrhi_device.Get()));

  UpscaleConfig probe;
  probe.quality = UpscaleQuality::kQuality;
  struct Sdk {
    const char* step;
    std::unique_ptr<Upscaler> (*create)(nvrhi::IDevice*, const UpscaleConfig&, std::string*);
  };
  for (const Sdk& sdk : {Sdk{"fsr3", CreateFsr3Upscaler}, Sdk{"fsr4", CreateFsr4Upscaler},
                         Sdk{"dlss", CreateDlssUpscaler}, Sdk{"xess", CreateXessUpscaler}}) {
    Begin(sdk.step);
    std::string why;
    bool ok = false;
    {
      std::unique_ptr<Upscaler> made = sdk.create(nvrhi_device.Get(), probe, &why);
      ok = made != nullptr;
    }  // destroyed inside the step too
    Line("end %s %d %s", sdk.step, ok ? 1 : 0, why.c_str());
  }
  Begin("finish");
  nvrhi_device->waitForIdle();
  nvrhi_device->runGarbageCollection();
  Line("done");
  return 0;
}

}  // namespace nr

#endif
