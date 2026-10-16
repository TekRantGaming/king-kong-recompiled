#include "handoff.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kk::handoff {
namespace {

constexpr const char* kEnv = "KK_HANDOFF_EVENT";

#if defined(_WIN32)
std::mutex g_mutex;
HANDLE g_event = nullptr;    // parent: signalled by the child's first frame
HANDLE g_process = nullptr;  // parent: the child
std::string g_child_event;   // child: the event's name, until signalled
std::atomic<bool> g_child_armed{false};
std::atomic<int> g_presents{0};       // presented frames since ContentDrawn
std::atomic<bool> g_content{false};   // ContentDrawn was called

// The program's own top-level window when it is the foreground window (the player is looking at it).
HWND OwnForegroundWindow() {
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  if (w) GetWindowThreadProcessId(w, &pid);
  return w && pid == GetCurrentProcessId() ? w : nullptr;
}
#endif

}  // namespace

long long NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void BeforeChild() {
#if defined(_WIN32)
  std::lock_guard lock(g_mutex);
  if (g_event) return;
  static int counter = 0;
  const std::string name = "Local\\KingKongHandoff-" + std::to_string(GetCurrentProcessId()) + "-" +
                           std::to_string(++counter);
  g_event = CreateEventA(nullptr, TRUE, FALSE, name.c_str());
  // The child inherits this process's environment (CreateProcess with no environment block).
  SetEnvironmentVariableA(kEnv, g_event ? name.c_str() : nullptr);
#endif
}

void ChildStarted(void* process_handle) {
#if defined(_WIN32)
  std::lock_guard lock(g_mutex);
  SetEnvironmentVariableA(kEnv, nullptr);  // only for that child
  if (g_process) CloseHandle(g_process);
  g_process = static_cast<HANDLE>(process_handle);
  REXLOG_INFO("KK handoff: started the next process at {} ms; this window stays until its first frame", NowMs());
#else
  (void)process_handle;
#endif
}

void ChildFailed() {
#if defined(_WIN32)
  std::lock_guard lock(g_mutex);
  SetEnvironmentVariableA(kEnv, nullptr);
  if (g_event) CloseHandle(g_event);
  g_event = nullptr;
#endif
}

void QuitWhenChildShown(std::function<void()> quit) {
#if defined(_WIN32)
  HANDLE event = nullptr, process = nullptr;
  {
    std::lock_guard lock(g_mutex);
    event = std::exchange(g_event, nullptr);
    process = std::exchange(g_process, nullptr);
  }
  if (!event || !process) {
    if (event) CloseHandle(event);
    if (process) CloseHandle(process);
    if (quit) quit();
    return;
  }
  // Above the new window while it starts (only when the player is looking at this one).
  HWND top = OwnForegroundWindow();
  if (top) SetWindowPos(top, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  std::thread([event, process, top, quit = std::move(quit)] {
    const auto start = std::chrono::steady_clock::now();
    HANDLE both[2] = {event, process};
    const DWORD r = WaitForMultipleObjects(2, both, FALSE, DWORD(kTimeoutSeconds * 1000));
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const char* how = r == WAIT_OBJECT_0       ? "its first frame is up"
                      : r == WAIT_OBJECT_0 + 1 ? "it exited"
                                               : "it took too long";
    REXLOG_INFO("KK handoff: the next process is shown ({}) after {:.2f} s; closing this window at {} ms", how, waited,
                NowMs());
    rex::FlushLogging();
    CloseHandle(event);
    CloseHandle(process);
    // The new window is shown: let it come forward as this one goes.
    if (top) SetWindowPos(top, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (quit) quit();
  }).detach();
#else
  if (quit) quit();
#endif
}

void ArmChild() {
#if defined(_WIN32)
  char name[256] = {};
  const DWORD n = GetEnvironmentVariableA(kEnv, name, DWORD(sizeof(name)));
  if (n == 0 || n >= sizeof(name)) return;
  SetEnvironmentVariableA(kEnv, nullptr);  // not for this process's own children
  g_child_event = name;
  g_child_armed = true;
  // The SDK's present observer (rexruntime, patch 0041): called after every presented frame.
  using Setter = void (*)(void (*)());
  const HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
  const auto setter = runtime ? reinterpret_cast<Setter>(GetProcAddress(runtime, "RexSetPresentObserver096")) : nullptr;
  if (setter) {
    setter([] { OnPresent(); });
    REXLOG_INFO("KK handoff: started by the previous process; it closes after this one's first frame");
  } else {
    // Without the observer the previous window still closes after its timeout.
    REXLOG_WARN("KK handoff: no present observer in this runtime");
  }
#endif
}

void ContentDrawn() {
#if defined(_WIN32)
  if (g_child_armed.load(std::memory_order_relaxed)) g_content.store(true, std::memory_order_release);
#endif
}

void OnPresent() {
#if defined(_WIN32)
  if (!g_child_armed.load(std::memory_order_relaxed) || !g_content.load(std::memory_order_acquire)) return;
  // The second presented frame after the content was drawn: it is surely on screen by then (a blank frame
  // the presenter shows before the launcher or the game has drawn does not count).
  if (g_presents.fetch_add(1) + 1 < 2) return;
  if (!g_child_armed.exchange(false)) return;
  if (HANDLE e = OpenEventA(EVENT_MODIFY_STATE, FALSE, g_child_event.c_str())) {
    SetEvent(e);
    CloseHandle(e);
  }
  REXLOG_INFO("KK handoff: first frame presented at {} ms; the previous process may close", NowMs());
#endif
}

}  // namespace kk::handoff
