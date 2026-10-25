// Task 357: the owner's Easter egg game as a child process (easter_egg.h).
#include "easter_egg.h"

#include <rex/filesystem.h>
#include <rex/logging.h>

#include <cstdlib>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <SDL3/SDL.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace kk::egg {

std::filesystem::path AppPath() {
#if defined(_WIN32)
  return rex::filesystem::GetExecutableFolder() / "launcher" / "easter-egg" / "LegallyDistinctGorilla.exe";
#else
  return rex::filesystem::GetExecutableFolder() / "launcher" / "easter-egg" / "LegallyDistinctGorilla";
#endif
}

#if defined(_WIN32)

struct Process::State {
  HANDLE job = nullptr;
  HANDLE process = nullptr;
  DWORD pid = 0;
};

Process::Process() : s_(std::make_unique<State>()) {}

Process::~Process() {
  if (s_->process) CloseHandle(s_->process);
  if (s_->job) CloseHandle(s_->job);  // kill on close: nothing of the game outlives the launcher
}

bool Process::Start(const std::filesystem::path& exe) {
  Terminate();
  if (!s_->job) {
    s_->job = CreateJobObjectW(nullptr, nullptr);
    if (s_->job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(s_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
  }
  std::wstring command = L"\"" + exe.wstring() + L"\"";
  const std::wstring folder = exe.parent_path().wstring();
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, folder.c_str(),
                      &si, &pi)) {
    REXLOG_WARN("KK: easter egg: could not start it (error {})", GetLastError());
    return false;
  }
  if (s_->job && !AssignProcessToJobObject(s_->job, pi.hProcess))
    REXLOG_INFO("KK: easter egg: not in a job (error {}); only its main process is closed", GetLastError());
  AllowSetForegroundWindow(pi.dwProcessId);  // its window comes up in front of the launcher
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  s_->process = pi.hProcess;
  s_->pid = pi.dwProcessId;
  REXLOG_INFO("KK: easter egg started (process {})", s_->pid);
  return true;
}

bool Process::Running() {
  if (!s_->process) return false;
  if (WaitForSingleObject(s_->process, 0) != WAIT_OBJECT_0) return true;
  DWORD code = 0;
  GetExitCodeProcess(s_->process, &code);
  REXLOG_INFO("KK: easter egg ended (exit code {})", code);
  CloseHandle(s_->process);
  s_->process = nullptr;
  if (s_->job) TerminateJobObject(s_->job, 0);  // anything it left behind
  return false;
}

namespace {

// The processes of the game: the job's, or its main process.
std::vector<DWORD> GameProcesses(HANDLE job, DWORD main_pid) {
  std::vector<DWORD> out;
  if (job) {
    std::vector<char> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 256 * sizeof(ULONG_PTR));
    auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
    if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, DWORD(buffer.size()), nullptr))
      for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) out.push_back(DWORD(list->ProcessIdList[i]));
  }
  if (out.empty() && main_pid) out.push_back(main_pid);
  return out;
}

struct Windows {
  DWORD main_pid;
  std::vector<DWORD> pids;
  bool owned;  // also windows that have an owner window (when the game has no other)
  std::vector<HWND> found;
};

// The game's visible top level windows, the main process's first (those with an owner window only when there
// are no others).
std::vector<HWND> GameWindows(HANDLE job, DWORD main_pid, bool owned = false) {
  Windows w{main_pid, GameProcesses(job, main_pid), owned, {}};
  EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        auto* w = reinterpret_cast<Windows*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!IsWindowVisible(hwnd) || (!w->owned && GetWindow(hwnd, GW_OWNER))) return TRUE;
        for (DWORD p : w->pids)
          if (p == pid) {
            if (pid == w->main_pid) w->found.insert(w->found.begin(), hwnd);
            else w->found.push_back(hwnd);
            break;
          }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&w));
  if (w.found.empty() && !owned) return GameWindows(job, main_pid, true);
  return w.found;
}

}  // namespace

bool Process::WindowShown() { return Running() && !GameWindows(s_->job, s_->pid).empty(); }

bool Process::InFront() {
  if (!Running()) return false;
  const HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  if (!fg || !GetWindowThreadProcessId(fg, &pid)) return false;
  for (DWORD p : GameProcesses(s_->job, s_->pid))
    if (p == pid) return true;
  return false;
}

bool EnterKeyDown() { return (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0; }
bool EscapeKeyDown() { return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0; }

void Process::ToFront() {
  if (!Running()) return;
  const std::vector<HWND> windows = GameWindows(s_->job, s_->pid);
  if (windows.empty()) {
    REXLOG_INFO("KK: easter egg: no window to bring back");
    return;
  }
  const bool front = ForceToFront(windows.front());
  REXLOG_INFO("KK: easter egg back in front{}", front ? "" : " (Windows kept another window in front)");
}

void Process::RequestClose() {
  if (!Running()) return;
  const std::vector<HWND> windows = GameWindows(s_->job, s_->pid);
  for (HWND hwnd : windows) PostMessageW(hwnd, WM_CLOSE, 0, 0);
  REXLOG_INFO("KK: easter egg asked to close ({} windows)", windows.size());
}

void Process::Terminate() {
  if (s_->job) TerminateJobObject(s_->job, 0);
  if (s_->process) {
    if (WaitForSingleObject(s_->process, 0) != WAIT_OBJECT_0) {
      TerminateProcess(s_->process, 0);
      REXLOG_INFO("KK: easter egg ended by the launcher");
    }
    CloseHandle(s_->process);
    s_->process = nullptr;
  }
}

bool ForceToFront(void* handle) {
  const HWND hwnd = static_cast<HWND>(handle);
  if (!hwnd || !IsWindow(hwnd)) return false;
#if defined(KK_DEV_TOOLS)
  // Test runs on the owner's PC never take the foreground: the call is only logged.
  if (std::getenv("KK_DEV_EGG_NO_FRONT")) {
    REXLOG_INFO("KK dev: easter egg: window to the front skipped (KK_DEV_EGG_NO_FRONT)");
    return false;
  }
#endif
  if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
  if (GetForegroundWindow() == hwnd) return true;
  // Windows gives the foreground only to the program that has it or had the last input (a controller does
  // not count). Sharing the input state with the window in front for a moment lets it pass on.
  const DWORD self = GetCurrentThreadId();
  const HWND fg = GetForegroundWindow();
  const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  const bool attached = fg_thread && fg_thread != self && AttachThreadInput(self, fg_thread, TRUE);
  BringWindowToTop(hwnd);
  SetForegroundWindow(hwnd);
  if (attached) AttachThreadInput(self, fg_thread, FALSE);
  if (GetForegroundWindow() != hwnd) {
    // In front of the other windows at least (on top for a moment, then back to normal).
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
  }
  return GetForegroundWindow() == hwnd;
}

#else

// Linux (owner, 25 Oct: the egg ships in the AppImage too): the Electron game built for Linux, in its own
// process group (the group is closed and ended as one, as the Windows job is). A Wayland program cannot put
// another program's window in front, so the launcher swaps places with the game instead: Start raises the
// launcher's own window (the pause panel), Resume minimises it so the game's window is the one in front again.
// Enter and Esc reach the launcher only while its own window has the keyboard (no system wide key state here).

namespace {

// The launcher's window: the only SDL window of this process.
SDL_Window* LauncherWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* w = windows && count > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  return w;
}

bool LauncherFocused() {
  SDL_Window* w = LauncherWindow();
  return w && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS);
}

}  // namespace

struct Process::State {
  pid_t pid = 0;
  double started = 0;  // steady seconds
};

Process::Process() : s_(std::make_unique<State>()) {}
Process::~Process() { Terminate(); }  // nothing of the game outlives the launcher

bool Process::Start(const std::filesystem::path& exe) {
  Terminate();
  // Chromium's sandbox needs a setuid helper, which an AppImage cannot hold; the game loads only its own
  // files. The AppImage's library path is not passed on (the game brings its own libraries).
  std::vector<std::string> env_strings;
  for (char** e = environ; *e; ++e)
    if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) != 0) env_strings.emplace_back(*e);
  std::vector<char*> env;
  for (auto& s : env_strings) env.push_back(s.data());
  env.push_back(nullptr);
  std::string path = exe.string(), sandbox = "--no-sandbox";
  char* argv[] = {path.data(), sandbox.data(), nullptr};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addchdir_np(&actions, exe.parent_path().c_str());
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);  // its own group: the pid is the group id
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, path.c_str(), &actions, &attr, argv, env.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    REXLOG_WARN("KK: easter egg: could not start it ({})", std::strerror(rc));
    return false;
  }
  s_->pid = pid;
  s_->started = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  REXLOG_INFO("KK: easter egg started (process {})", pid);
  return true;
}

bool Process::Running() {
  if (!s_->pid) return false;
  int status = 0;
  const pid_t r = waitpid(s_->pid, &status, WNOHANG);
  if (r == 0) return true;
  REXLOG_INFO("KK: easter egg ended (exit code {})", r == s_->pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  kill(-s_->pid, SIGKILL);  // anything it left behind
  s_->pid = 0;
  return false;
}

// No way to see another program's windows here: the game's window counts as up a moment after it started, or
// as soon as the launcher has lost the keyboard to it.
bool Process::WindowShown() {
  if (!Running()) return false;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return !LauncherFocused() || now - s_->started > 3.0;
}

bool Process::InFront() { return Running() && !LauncherFocused(); }

bool EnterKeyDown() { return false; }
bool EscapeKeyDown() { return false; }

void Process::ToFront() {
  if (!Running()) return;
  SDL_Window* w = LauncherWindow();
  if (w) SDL_MinimizeWindow(w);
  REXLOG_INFO("KK: easter egg back in front (the launcher minimised{})", w ? "" : ": no launcher window");
}

void Process::RequestClose() {
  if (!Running()) return;
  kill(-s_->pid, SIGTERM);  // Electron closes its windows and quits
  REXLOG_INFO("KK: easter egg asked to close");
}

void Process::Terminate() {
  if (!s_->pid) return;
  kill(-s_->pid, SIGKILL);
  waitpid(s_->pid, nullptr, 0);
  REXLOG_INFO("KK: easter egg ended by the launcher");
  s_->pid = 0;
}

bool ForceToFront(void*) {
  SDL_Window* w = LauncherWindow();
  if (!w) return false;
  SDL_RestoreWindow(w);
  SDL_RaiseWindow(w);  // the compositor decides (Wayland: an activation request)
  return LauncherFocused();
}

#endif

}  // namespace kk::egg
