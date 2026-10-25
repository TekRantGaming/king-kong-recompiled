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
#include <fcntl.h>
#include <string>
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

// Linux (owner, 25 Oct: the egg ships in the AppImage too): the Electron game for Linux, started through
// launcher/easter-egg/resources/app (kk/egg-linux/main.js), which loads the game unchanged from egg.asar and
// talks to the launcher over two pipes. A Wayland program cannot put its own window in front of another
// program's, so for the pause panel the game hides its window ("hide") and Resume shows it again ("show"); the
// game tells its focus and its Enter and Esc presses (task 358/364: they pause it as on Windows). The game is in
// its own process group, closed and ended as one (the Windows job), and quits when the launcher goes.

namespace {

// The launcher's window: the only SDL window of this process.
SDL_Window* LauncherWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* w = windows && count > 0 ? windows[0] : nullptr;
  SDL_free(windows);
  return w;
}

Process::State* g_running = nullptr;  // the game started last (ForceToFront hides its window)

}  // namespace

struct Process::State {
  pid_t pid = 0;
  int to = -1, from = -1;  // its stdin (commands), its stdout (what it tells)
  std::string pending;
  bool shown = false, focused = false, enter = false, escape = false;

  void Send(const char* command) {
    if (to < 0) return;
    const std::string line = std::string(command) + "\n";
    if (write(to, line.data(), line.size()) < 0) REXLOG_INFO("KK: easter egg: '{}' not sent", command);
  }
  void Read() {
    char buf[512];
    for (ssize_t n; from >= 0 && (n = read(from, buf, sizeof(buf))) > 0;) pending.append(buf, size_t(n));
    for (size_t at; (at = pending.find('\n')) != std::string::npos;) {
      const std::string line = pending.substr(0, at);
      pending.erase(0, at + 1);
      if (line == "shown") shown = true;
      else if (line == "focus 1" || line == "focus 0") focused = line == "focus 1";
      else if (line == "down enter" || line == "up enter") enter = line == "down enter";
      else if (line == "down escape" || line == "up escape") escape = line == "down escape";
    }
  }
  void Close() {
    if (to >= 0) close(to);
    if (from >= 0) close(from);
    to = from = -1;
    pending.clear();
    shown = focused = enter = escape = false;
    if (g_running == this) g_running = nullptr;
  }
};

Process::Process() : s_(std::make_unique<State>()) {}
Process::~Process() { Terminate(); }  // nothing of the game outlives the launcher

bool Process::Start(const std::filesystem::path& exe) {
  Terminate();
  int to[2], from[2];
  if (pipe2(to, O_CLOEXEC) != 0) return false;
  if (pipe2(from, O_CLOEXEC) != 0) {
    close(to[0]), close(to[1]);
    return false;
  }
  // Chromium's sandbox needs a setuid helper, which an AppImage cannot hold (the game loads only its own files).
  // The AppImage's library path is not passed on: the game brings its own libraries.
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
  posix_spawn_file_actions_adddup2(&actions, to[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, from[1], STDOUT_FILENO);
  posix_spawn_file_actions_addchdir_np(&actions, exe.parent_path().c_str());
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attr, 0);  // its own group: the pid is the group id
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, path.c_str(), &actions, &attr, argv, env.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  close(to[0]);
  close(from[1]);
  if (rc != 0) {
    close(to[1]), close(from[0]);
    REXLOG_WARN("KK: easter egg: could not start it ({})", std::strerror(rc));
    return false;
  }
  fcntl(from[0], F_SETFL, fcntl(from[0], F_GETFL) | O_NONBLOCK);
  s_->pid = pid;
  s_->to = to[1];
  s_->from = from[0];
  g_running = s_.get();
  REXLOG_INFO("KK: easter egg started (process {})", pid);
  return true;
}

bool Process::Running() {
  if (!s_->pid) return false;
  s_->Read();
  int status = 0;
  const pid_t r = waitpid(s_->pid, &status, WNOHANG);
  if (r == 0) return true;
  REXLOG_INFO("KK: easter egg ended (exit code {})", r == s_->pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  kill(-s_->pid, SIGKILL);  // anything it left behind
  s_->pid = 0;
  s_->Close();
  return false;
}

bool Process::WindowShown() { return Running() && s_->shown; }
bool Process::InFront() { return Running() && s_->focused; }

bool EnterKeyDown() { return g_running && (g_running->Read(), g_running->enter); }
bool EscapeKeyDown() { return g_running && (g_running->Read(), g_running->escape); }

void Process::ToFront() {
  if (!Running()) return;
  s_->Send("show");
  if (SDL_Window* w = LauncherWindow()) SDL_MinimizeWindow(w);  // out of the way too, whatever the desktop allows
  REXLOG_INFO("KK: easter egg back in front (asked to show its window)");
}

void Process::RequestClose() {
  if (!Running()) return;
  s_->Send("close");
  REXLOG_INFO("KK: easter egg asked to close");
}

void Process::Terminate() {
  if (!s_->pid) return;
  kill(-s_->pid, SIGKILL);
  waitpid(s_->pid, nullptr, 0);
  REXLOG_INFO("KK: easter egg ended by the launcher");
  s_->pid = 0;
  s_->Close();
}

bool ForceToFront(void*) {
  if (g_running) g_running->Send("hide");  // the game's window steps aside (a Wayland raise is only a request)
  SDL_Window* w = LauncherWindow();
  if (!w) return false;
  SDL_RestoreWindow(w);
  SDL_RaiseWindow(w);
  return true;
}

#endif

}  // namespace kk::egg
