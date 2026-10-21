#include "startup_recovery.h"
#include "exit_evidence.h"
#include "diagnostics.h"
#include "startup_recovery_state.h"
#include "amd_shader_cache.h"
#include "launcher.h"
#include "launcher_text.h"
#include "settings.h"
#include <rex/system.h>
#include <cstdlib>
#include <memory>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "hooks/kk_native_ui.h"
#include "startup_clock.h"
#include "backend/startup_steps.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <cwchar>
#if defined(KK_DEV_TOOLS)
#include <tlhelp32.h>
#endif
#endif

REXCVAR_DEFINE_BOOL(kk_startup_child, false, "KK/Internal", "Internal startup monitor child").debug_only();
// Task 228, owner 18 Oct: on by default. The launcher explains how to empty the AMD driver's shader
// caches by hand and also offers to empty them (at the next start, only with the player's consent).
REXCVAR_DEFINE_BOOL(kk_amd_shader_cache_offer, true, "KK/Internal",
    "After a start that ended unexpectedly, offer to empty the AMD driver shader caches (VkCache, DxCache, "
    "DxcCache in LOCALAPPDATA/AMD) at the next start");
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
constexpr const char* kAmdHint =
    "A damaged graphics driver shader cache can also cause this, most often on AMD graphics cards: close the "
    "game, empty the folders VkCache, DxCache and DxcCache in %LOCALAPPDATA%\\AMD, then start the game again.";
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
// Task 228: the launcher's own start (LauncherGuard). `left` = how the last launcher start ended.
std::filesystem::path user_root;
LauncherGuard::Left left;
bool launcher_active = false;  // this process is the launcher's and its marker is written
std::string launcher_api, launcher_phase;
int launcher_count = 0;
bool crash_notice_shown = false;
std::string amd_report;  // what emptying the AMD caches at this start did (once, for the launcher)

std::filesystem::path AmdRequestFile() { return state->directory / "clear-amd-cache.request"; }
std::filesystem::path LocalAppData() {
#if defined(_WIN32)
  if (const wchar_t* v = _wgetenv(L"LOCALAPPDATA"); v && *v) return std::filesystem::path(v);
#endif
  return {};
}
// Our own Vulkan driver pipeline cache files (<cache root>/native-pipelines/*.vkcache, or
// native_pipeline_cache_dir). They are checked against the driver (vendor, device, pipelineCacheUUID,
// driverUUID, driver version, data hash) before use; after a start that ended unexpectedly they go
// anyway: they are rebuilt from the driver, and no file of ours can then be involved in a second crash.
void DiscardOwnDriverCaches() {
  std::filesystem::path folder = rex::cvar::GetFlagByName("native_pipeline_cache_dir");
  if (folder.empty()) {
    const std::string root = rex::cvar::GetFlagByName("cache_root");
    folder = (root.empty() ? user_root / "cache" : std::filesystem::path(root)) / "native-pipelines";
  }
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator();
       it.increment(ec)) {
    if (it->path().extension() != ".vkcache") continue;
    std::error_code e;
    std::filesystem::remove(it->path(), e);
    REXLOG_INFO("KK recovery: removed our pipeline cache {} after an unexpected end ({})", it->path().string(),
                e ? e.message() : std::string("ok"));
  }
}

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

// End the recovery chain after both native APIs fail. No monitor retry is signalled.
[[noreturn]] void Exhausted() {
#if defined(__linux__)
  REXLOG_ERROR("KK recovery: Vulkan could not start; see logs for details");
  rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
      kk::text::T("Vulkan could not start. Update your graphics driver and check that your GPU meets the requirements in README.md. For help, use the support links in README.md and include the logs folder."));
#else
  REXLOG_ERROR("KK recovery: both native graphics APIs failed; see logs for details");
  rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
      std::string(kk::text::T("Neither Vulkan nor DirectX 12 could start. Update your graphics driver and check that your GPU meets the requirements in README.md. For help, use the support links in README.md and include the logs folder.")) +
          "\n\n" + kk::text::T(kAmdHint));
#endif
  if (state) state->Clear();
  Signal('C');
  std::_Exit(1);
}

int Failure(const char* why) {
  std::lock_guard lock(mutex);
  // Device loss after startup, or in the launcher, retains the existing fatal path.
  if (!state || !(armed || (init_only && !IsDeviceLoss(why)))) return 0;
  if (!state->Advance(why)) {
    REXLOG_ERROR("KK recovery: could not persist startup fallback");
    return 0;
  }
  if (state->selected == "failed") Exhausted();
  armed = false;
  init_only = false;
  REXLOG_ERROR("KK recovery: {}", state->reason);
  Signal('F');
  // A fresh process is essential after a failed or lost device. The monitor waits for this one to exit.
  std::_Exit(kRetryExit);
}
// Mutex held. Tells the native renderer which APIs failed here, so its setup does not prepare them
// (--native_failed_backends; nothing happens without the plugin).
void PublishFailed() {
  std::string failed;
  if (state && !state->reason.empty() && state->selected != state->preferred) {
    failed = state->preferred;
  }
  rex::cvar::SetFlagByName("native_failed_backends", failed);
}
void Apply() {
  if (!state || (state->reason.empty() && !state->retry_requested)) return;
  if (!overridden) {
    overridden = true;
    original_renderer = rex::cvar::GetFlagByName("kk_renderer");
    original_plugin = rex::cvar::GetFlagByName("gpu_plugin");
    original_backend = rex::cvar::GetFlagByName("native_backend");
  }
  rex::cvar::SetFlagByName("kk_renderer", "native");
  rex::cvar::SetFlagByName("gpu_plugin", "native");
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
  nr::steps::Note("first game frame shown");
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
  // Task 353: the child writes the path of its start-up step log here (crash_dump.cpp), so this process can add
  // how it ended: a fail-fast or a driver ending the program leaves nothing else.
  wchar_t note[MAX_PATH] = L"";
  {
    wchar_t temp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    if (n && n < MAX_PATH - 48) {
      swprintf(note, MAX_PATH, L"%lsking_kong-monitor-%lu.txt", temp, GetCurrentProcessId());
      DeleteFileW(note);
      SetEnvironmentVariableW(L"KK_MONITOR_NOTE", note);
    }
  }
  auto note_end = [&](DWORD pid, DWORD code) {
    if (!note[0]) return;
    wchar_t path[MAX_PATH] = L"";
    HANDLE f = CreateFileW(note, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    ReadFile(f, path, sizeof(path) - sizeof(wchar_t), &got, nullptr);
    CloseHandle(f);
    path[got / sizeof(wchar_t)] = 0;
    if (!path[0]) return;
    SetEnvironmentVariableW(L"KK_STEP_LOG", path);
    const char* what = code == 0            ? "ended normally"
                       : code == kRetryExit ? "ended to start again with the other graphics API"
                       : code == 0xC0000005 ? "crashed (access violation)"
                       : code == 0xC0000409 ? "was ended by a fail-fast (or a stack buffer overrun)"
                       : code == 0xC00000FD ? "crashed (stack overflow)"
                       : code == 0xC0000374 ? "crashed (heap corruption)"
                       : code == 0xC000001D ? "crashed (illegal instruction)"
                       : code == 0x40000015 ? "ended by abort()"
                       : code == 1          ? "ended (exit code 1)"
                                            : "ended";
    nr::steps::Note("monitor: king_kong process %lu %s, exit code 0x%08lX", pid, what, code);
  };
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
    note_end(pi.dwProcessId, result);
    DWORD available = 0, got = 0;
    char last = 0;
    while (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available) {
      char value;
      if (!ReadFile(read, &value, 1, &got, nullptr) || !got) break;
      last = value;
    }
    // A normal close is not a crash. Never retry a crash after a chapter, or an exhausted recovery.
    if (last != 'F' && !(last == 'B' && result != 0)) break;
  }
  CloseHandle(read); CloseHandle(write);
  if (note[0]) DeleteFileW(note);
  std::_Exit(result);
#endif
}

void Configure(const std::filesystem::path& root) {
  std::lock_guard lock(mutex);
  user_root = root;
  state = std::make_unique<RecoveryState>(root);
  left = LauncherGuard(state->directory).Read();
  if (left.count > 0)
    REXLOG_WARN("KK recovery: the last launcher start ended unexpectedly ({} {}, {} in a row)", left.api, left.phase,
                left.count);
  if (kk::NormalizeRendererSettings()) state->Clear();
  else if (state->Load()) state->RecoverCrash();
  if (state->selected == "failed") Exhausted();
  Apply();
  PublishFailed();
  nr::kk::SetStartupFailureHandler(Failure);
  nr::kk::SetStartupPresentedHandler(FirstFramePresented);
}

std::string ApplyChoice(const std::string& plugin) {
  std::lock_guard lock(mutex);
  Apply();
  return state && (!state->reason.empty() || state->retry_requested)
      ? ("native") : plugin;
}

void Begin(const std::string& plugin, bool game) {
  {
    std::lock_guard lock(mutex);
    if (!state || plugin != "native" || armed) return;
    const std::string wanted = rex::cvar::GetFlagByName("native_backend");
    const std::string api = wanted == "d3d12" ? "d3d12" : "vulkan";
    if (state->reason.empty()) { state->preferred = api; state->selected = api; }
    if (!game) {
      // The launcher shows first: only a renderer that cannot start, or (task 228) one that crashed
      // twice in a row before the launcher showed, switches the API.
      init_only = true;
      REXLOG_INFO("KK recovery: launcher process, only a renderer that cannot start switches the API");
      if (LauncherGuard::SwitchApi(left, state->selected) &&
          state->Advance("crashed twice before the main menu showed")) {
        if (state->selected == "failed") Exhausted();
        Apply();
        PublishFailed();
        REXLOG_ERROR("KK recovery: {}", state->reason);
      }
      if (left.count > 0) DiscardOwnDriverCaches();
      std::error_code ec;
      if (std::filesystem::exists(AmdRequestFile(), ec)) {
        std::filesystem::remove(AmdRequestFile(), ec);
        if (REXCVAR_GET(kk_amd_shader_cache_offer)) {
          // Before this process opens the driver; only at a launcher start (the launcher that took the
          // request has exited). A file another program still has open is kept.
          const auto r = amd_cache::Clear(LocalAppData());
          REXLOG_INFO("KK recovery: AMD shader cache emptied at the player's request: {} files ({} MB), {} kept",
                      r.removed, r.bytes >> 20, r.kept);
          amd_report = std::to_string(r.removed) + " " + std::to_string(r.kept);
        }
      }
      launcher_api = state->selected.empty() ? api : state->selected;
      launcher_phase = "window";
      launcher_count = LauncherGuard::Next(left, launcher_api);
      launcher_active = LauncherGuard(state->directory).Write(launcher_api, launcher_phase, launcher_count);
      return;
    }
    init_only = false;
    if (!state->Save() || !state->Marker(api)) {
      REXLOG_ERROR("KK recovery: startup marker could not be written");
      return;
    }
    armed = true;
    const unsigned generation = ++arm_generation;
    Signal('B');
    REXLOG_INFO("KK recovery: startup armed for {}", api);
    nr::steps::Note("game start in this process (%s)", api.c_str());
    std::ofstream journal(state->directory / "attempts.log", std::ios::app);
    journal << api << " crashes=" << state->crashes << " reason=" << state->reason << '\n';
    // Startup also ends after a time limit, in case no frame is reported. Task 297: time spent compiling
    // shaders the game waited for (no shader pack: minutes of DXC on the game thread) is not counted, so a
    // device loss right after such a start still recovers (startup_clock.h; capped at ten minutes).
    std::thread([generation] {
      const auto clock_ms = [] {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
      };
      const startup_clock::Window window{clock_ms(), nr::kk::ShaderCompileMilliseconds()};
      constexpr uint64_t limit = uint64_t(kArmSeconds) * 1000;
      bool noted = false;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const uint64_t now = clock_ms();
        const uint64_t compiled = nr::kk::ShaderCompileMilliseconds();
        std::lock_guard lock(mutex);
        if (generation != arm_generation) return;  // disarmed (first frame, exit) or armed again
        if (startup_clock::Reached(window, now, compiled, limit)) {
          Disarm("time limit reached");
          return;
        }
        if (!noted && startup_clock::ExtendedByCompile(window, now, compiled, limit)) {
          noted = true;
          REXLOG_INFO("KK recovery: startup window extended: {} s of the {} s so far went to compiling shaders "
                      "(no shader pack); recovery keeps watching until {} s without compiling",
                      startup_clock::CompiledMs(window, compiled) / 1000, (now - window.started) / 1000, kArmSeconds);
        }
      }
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
  kk::StopBackgroundLogOutput();
  kk::exit_evidence::NoteCleanExit("quit or restart");  // task 229: logs\session-<role>.txt
  std::lock_guard lock(mutex);
  if (launcher_active && state) LauncherGuard(state->directory).Clear();
  launcher_active = false;
  if (armed && state) { state->Marker(); state->crashes = 0; state->Save(); }
  armed = false;
  init_only = false;
  ++arm_generation;
  Signal('C');
}
std::string Message() { std::lock_guard lock(mutex); return state ? state->reason : ""; }
void NoticeApis(std::string& failed, std::string& selected) {
  std::lock_guard lock(mutex);
  failed = state ? RecoveryState::Name(state->preferred) : "";
  selected = state ? RecoveryState::Name(state->selected) : "";
}
void LauncherShown(bool preparing_shaders) {
  std::lock_guard lock(mutex);
  if (!launcher_active || !state) return;
  const std::string phase = preparing_shaders ? "shaders" : "";
  if (phase == launcher_phase) return;
  launcher_phase = phase;
  const LauncherGuard guard(state->directory);
  if (phase.empty()) guard.Clear();
  else guard.Write(launcher_api, phase, launcher_count);
}
std::string CrashNotice() {
  std::lock_guard lock(mutex);
  return crash_notice_shown ? std::string() : left.phase;
}
void CrashNoticeShown() {
  std::lock_guard lock(mutex);
  crash_notice_shown = true;
}
bool AmdCacheOffer() { return REXCVAR_GET(kk_amd_shader_cache_offer) && amd_cache::Present(LocalAppData()); }
bool RequestAmdCacheClear() {
  std::lock_guard lock(mutex);
  if (!state || !REXCVAR_GET(kk_amd_shader_cache_offer)) return false;
  std::error_code ec;
  std::filesystem::create_directories(state->directory, ec);
  std::ofstream out(AmdRequestFile(), std::ios::trunc);
  out << "requested by the player\n";
  REXLOG_INFO("KK recovery: the player asked to empty the AMD shader cache at the next start");
  return bool(out);
}
std::string AmdCacheReport() {
  std::lock_guard lock(mutex);
  return std::exchange(amd_report, std::string());
}
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
  PublishFailed();
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
  PublishFailed();
  REXLOG_INFO("KK recovery: renderer chosen by hand, startup fallback cleared");
}
}  // namespace kk::startup
