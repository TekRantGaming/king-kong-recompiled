#include "startup_recovery.h"
#include "startup_recovery_state.h"
#include "launcher.h"
#include <cstdlib>
#include <memory>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>
#include <rex/cvar.h>
#include <rex/logging.h>
#if defined(KK_NATIVE_RENDERER)
#include "hooks/kk_native_ui.h"
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#if defined(KK_DEV_TOOLS)
#include <tlhelp32.h>
#endif
#endif

REXCVAR_DEFINE_BOOL(kk_startup_child, false, "KK/Internal", "Internal startup monitor child").debug_only();
#if defined(KK_DEV_TOOLS)
REXCVAR_DEFINE_BOOL(kk_dev_startup_skip_notice, false, "KK/Debug",
    "Skip the recovery launcher during isolated gameplay tests").debug_only();
REXCVAR_DEFINE_STRING(kk_dev_startup_failure, "", "KK/Debug",
    "Force native startup failure: init, crash or device-lost").allowed({"", "init", "crash", "device-lost"}).debug_only();
REXCVAR_DEFINE_STRING(kk_dev_startup_api, "all", "KK/Debug",
    "API to fail: all, vulkan or d3d12").allowed({"all", "vulkan", "d3d12"}).debug_only();
#endif

namespace kk::startup {
namespace {
std::mutex mutex;
std::unique_ptr<RecoveryState> state;
// Armed: this process runs the game and has not shown its first frame yet. A failure, a device loss
// or a crash (the marker) then switches the API.
bool armed = false;
// The launcher's process before the game: only a renderer that cannot start at all switches the API
// (the launcher could not open otherwise). A device loss or crash there never does.
bool init_only = false;
unsigned arm_generation = 0;
// Startup ends at the first shown game frame, or after this long when no frame came.
constexpr unsigned kArmSeconds = 120;
constexpr unsigned kRetryExit = 0x4B4B0039;
// The player's own settings before Apply replaced them with the fallback's, put back when the player
// then chooses a renderer or API by hand.
bool overridden = false;
std::string original_renderer, original_plugin, original_backend;

void Signal(char value) {
#if defined(_WIN32)
  if (const char* pipe = std::getenv("KK_STARTUP_PIPE")) {
    const auto handle = reinterpret_cast<HANDLE>(std::strtoull(pipe, nullptr, 10));
    DWORD written = 0;
    WriteFile(handle, &value, 1, &written, nullptr);
  }
#endif
}

bool IsDeviceLoss(const char* why) { return why && std::string_view(why).find("lost") != std::string_view::npos; }

int Failure(const char* why) {
  std::lock_guard lock(mutex);
  // Device loss after startup, or in the launcher, retains the existing fatal path.
  if (!state || !(armed || (init_only && !IsDeviceLoss(why)))) return 0;
  if (!state->Advance(why)) {
    REXLOG_ERROR("KK recovery: could not persist startup fallback");
    return 0;
  }
  armed = false;
  init_only = false;
  REXLOG_ERROR("KK recovery: {}", state->reason);
  Signal('F');
  // A fresh process is essential after a failed or lost device. The monitor waits for this one to exit.
  std::_Exit(kRetryExit);
}
void Apply() {
  if (!state || (state->reason.empty() && !state->retry_requested)) return;
  if (!overridden) {
    overridden = true;
    original_renderer = rex::cvar::GetFlagByName("kk_renderer");
    original_plugin = rex::cvar::GetFlagByName("gpu_plugin");
    original_backend = rex::cvar::GetFlagByName("native_backend");
  }
  rex::cvar::SetFlagByName("kk_renderer", state->selected == "classic" ? "classic" : "native");
  rex::cvar::SetFlagByName("gpu_plugin", state->selected == "classic" ? "xenos" : "native");
  if (RecoveryState::Native(state->selected)) rex::cvar::SetFlagByName("native_backend", state->selected);
}
// Mutex held. Startup is over: the marker goes and a fallback that reached the game is kept.
void Disarm(const char* why) {
  if (!armed || !state) return;
  if (!state->Marker()) { REXLOG_ERROR("KK recovery: could not clear startup marker"); return; }
  if (state->selected == state->preferred) state->reason.clear();
  state->retry_requested = false;
  state->crashes = 0;
  state->Save();
  armed = false;
  ++arm_generation;
  Signal('G');
  REXLOG_INFO("KK recovery: startup finished ({}), startup marker cleared", why);
}
void FirstFramePresented() {
  std::lock_guard lock(mutex);
  Disarm("first game frame shown");
}
}  // namespace

void MonitorStartup() {
#if defined(_WIN32)
  // Called by the factory before any app/window/runtime is constructed. The parent has no game window.
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool child = false;
  for (int i = 1; argv && i < argc; ++i) if (std::wstring(argv[i]) == L"--kk_startup_child=true") child = true;
  if (argv) LocalFree(argv);
  if (child) return;
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  HANDLE read = nullptr, write = nullptr;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  if (!CreatePipe(&read, &write, &sa, 0)) return;
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  wchar_t exe[32768];
  if (!GetModuleFileNameW(nullptr, exe, 32768)) { CloseHandle(read); CloseHandle(write); return; }
  const std::string pipe = std::to_string(reinterpret_cast<uintptr_t>(write));
  _putenv_s("KK_STARTUP_PIPE", pipe.c_str());
  DWORD result = 0;
  for (int attempt = 0; attempt < 6; ++attempt) {
#if defined(KK_DEV_TOOLS)
    // A test reserves the shared lock for this whole recovery run. Recheck before every child launch,
    // including automatic retries, so a user takeover cannot start another test window.
    if (const char* lock_path = std::getenv("KK_TEST_STARTUP_LOCK")) {
      std::ifstream lock_file(lock_path);
      std::string owner;
      std::getline(lock_file, owner);
      const char* test_owner = std::getenv("KK_TEST_STARTUP_OWNER");
      const std::string prefix = std::string(test_owner ? test_owner : "task039") + " ";
      if (owner.rfind(prefix, 0) != 0) { result = 3; break; }
      // Never launch a recovery child while another copy of the game is running.
      // This also covers copies whose executable path is inaccessible.
      bool other_game = false;
      const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snapshot == INVALID_HANDLE_VALUE) { result = 3; break; }
      PROCESSENTRY32W entry{};
      entry.dwSize = sizeof(entry);
      for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"king_kong.exe") != 0 || entry.th32ProcessID == GetCurrentProcessId()) continue;
        other_game = true;
        break;
      }
      CloseHandle(snapshot);
      if (other_game) { result = 3; break; }
    }
#endif
    std::wstring cmd = GetCommandLineW();
    cmd += L" --kk_startup_child=true";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
      result = GetLastError(); break;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &result);
    CloseHandle(pi.hProcess);
    DWORD available = 0, got = 0;
    char last = 0;
    while (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available) {
      char value;
      if (!ReadFile(read, &value, 1, &got, nullptr) || !got) break;
      last = value;
    }
    // A normal close is not a crash. Never retry a crash after a chapter, or a Classic failure.
    if (last != 'F' && !(last == 'B' && result != 0)) break;
  }
  CloseHandle(read); CloseHandle(write);
  std::_Exit(result);
#endif
}

void Configure(const std::filesystem::path& root) {
  std::lock_guard lock(mutex);
  state = std::make_unique<RecoveryState>(root);
  if (state->Load()) state->RecoverCrash();
  Apply();
#if defined(KK_NATIVE_RENDERER)
  nr::kk::SetStartupFailureHandler(Failure);
  nr::kk::SetStartupPresentedHandler(FirstFramePresented);
#endif
}

std::string ApplyChoice(const std::string& plugin) {
  std::lock_guard lock(mutex);
  Apply();
  return state && (!state->reason.empty() || state->retry_requested)
      ? (state->selected == "classic" ? "xenos" : "native") : plugin;
}

void Begin(const std::string& plugin, bool game) {
  {
    std::lock_guard lock(mutex);
    if (!state || plugin != "native" || armed) return;
    if (!game) {
      // The launcher shows first: nothing is written and a crash is not counted.
      init_only = true;
      REXLOG_INFO("KK recovery: launcher process, only a renderer that cannot start switches the API");
      return;
    }
    init_only = false;
    const std::string wanted = rex::cvar::GetFlagByName("native_backend");
    const std::string api = wanted == "d3d12" ? "d3d12" : "vulkan";
    if (state->reason.empty()) { state->preferred = api; state->selected = api; }
    if (!state->Save() || !state->Marker(api)) {
      REXLOG_ERROR("KK recovery: startup marker could not be written");
      return;
    }
    armed = true;
    const unsigned generation = ++arm_generation;
    Signal('B');
    REXLOG_INFO("KK recovery: startup armed for {}", api);
    std::ofstream journal(state->directory / "attempts.log", std::ios::app);
    journal << api << " crashes=" << state->crashes << " reason=" << state->reason << '\n';
    // Startup also ends after a time limit, in case no frame is reported.
    std::thread([generation] {
      std::this_thread::sleep_for(std::chrono::seconds(kArmSeconds));
      std::lock_guard lock(mutex);
      if (generation == arm_generation) Disarm("time limit reached");
    }).detach();
  }
#if defined(KK_DEV_TOOLS)
  const auto mode = REXCVAR_GET(kk_dev_startup_failure);
  const auto target = REXCVAR_GET(kk_dev_startup_api);
  const auto api = rex::cvar::GetFlagByName("native_backend") == "d3d12" ? "d3d12" : "vulkan";
  if (target == "all" || target == api) {
    if (mode == "crash") std::_Exit(0xC0000005);
    // Init and loss injections live inside the native plugin and use its real failure paths.
  }
#endif
}

void CleanExit() {
  std::lock_guard lock(mutex);
  if (armed && state) { state->Marker(); state->crashes = 0; state->Save(); }
  armed = false;
  init_only = false;
  ++arm_generation;
  Signal('C');
}
std::string Message() { std::lock_guard lock(mutex); return state ? state->reason : ""; }
bool NeedsNotice() {
#if defined(KK_DEV_TOOLS)
  if (REXCVAR_GET(kk_dev_startup_skip_notice)) return false;
#endif
  std::lock_guard lock(mutex);
  return state && !state->reason.empty() && state->selected != state->preferred && !state->notice_shown;
}
void NoticeShown() {
  std::lock_guard lock(mutex);
  if (!state || state->notice_shown) return;
  state->notice_shown = true;
  state->Save();
}
bool TryNativeAgain() {
  std::lock_guard lock(mutex);
  if (!state || !state->Retry()) return false;
  rex::cvar::SetFlagByName("kk_renderer", "native");
  rex::cvar::SetFlagByName("gpu_plugin", "native");
  rex::cvar::SetFlagByName("native_backend", state->preferred);
  return true;
}
void ManualChoice() {
  std::lock_guard lock(mutex);
  if (!state || (state->reason.empty() && !state->retry_requested && !overridden)) return;
  // The player's own values come back first; the choice being made is set after this.
  if (overridden) {
    rex::cvar::SetFlagByName("kk_renderer", original_renderer);
    rex::cvar::SetFlagByName("gpu_plugin", original_plugin);
    rex::cvar::SetFlagByName("native_backend", original_backend);
    overridden = false;
  }
  if (!state->Clear()) REXLOG_ERROR("KK recovery: could not clear the startup fallback");
  REXLOG_INFO("KK recovery: renderer chosen by hand, startup fallback cleared");
}
}  // namespace kk::startup
