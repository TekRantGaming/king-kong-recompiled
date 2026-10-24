// king_kong - ReXGlue Recompiled Project

#include "generated/default/king_kong_init.h"

#include "king_kong_app.h"

REX_DEFINE_APP(king_kong, KingKongApp::Create)

#ifdef _WIN32
// Hybrid laptops (Intel/AMD iGPU + NVIDIA/AMD dGPU): the drivers start an exe that exports these
// on the discrete GPU (task 203; Beta 7/8 crash reports from an Intel UHD + RTX 4050 laptop).
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

#include <shellapi.h>

#include <string>

namespace {
// Task 353: the launcher's upscaler check runs in a process of its own (native-renderer/plugin/
// upscaler_check_child.cpp): this program started with --kk_upscaler_check=<report>
// --kk_upscaler_check_adapter=<luid>. It is caught here, before the SDK's main and anything of the game (no
// window, no settings, no startup monitor): the renderer plugin asks the upscalers' SDKs and this process ends
// with its answer's exit code. A crash or hang in an SDK or the driver then ends only this process.
bool RunUpscalerCheckIfAsked() {
  const wchar_t* line = GetCommandLineW();
  if (!line || !wcsstr(line, L"--kk_upscaler_check=")) return false;
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(line, &argc);
  std::wstring report, adapter;
  for (int i = 1; argv && i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a.rfind(L"--kk_upscaler_check=", 0) == 0) report = a.substr(20);
    else if (a.rfind(L"--kk_upscaler_check_adapter=", 0) == 0) adapter = a.substr(28);
  }
  if (argv) LocalFree(argv);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  wchar_t exe[MAX_PATH * 2] = L"";
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
  std::wstring dir(exe, n);
  dir.resize(dir.find_last_of(L"\\/") + 1);
  int code = 2;  // no plugin
  for (const wchar_t* name : {L"rexgpu-nativerd.dll", L"rexgpu-native.dll", L"rexgpu-natived.dll"}) {
    HMODULE plugin = LoadLibraryExW((dir + name).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!plugin) continue;
    using Main = int (*)(const wchar_t*, const wchar_t*);
    if (const auto main = reinterpret_cast<Main>(GetProcAddress(plugin, "nr_upscaler_check_main")))
      code = main(report.c_str(), adapter.c_str());
    break;
  }
  // At once: no library's shutdown code (the SDKs') runs in this short-lived process.
  TerminateProcess(GetCurrentProcess(), UINT(code));
  return true;
}
const bool g_upscaler_check = RunUpscalerCheckIfAsked();
}  // namespace
#endif
