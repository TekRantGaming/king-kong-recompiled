// Developer-only (task 217): where the game's threads are while no frame comes.
//
// KK_DEV_GAME_WATCH=<ms> (default 1000 when set to 1): once the guest has not
// swapped a frame for that long, every 250 ms each thread of the process is
// briefly suspended and its native stack logged ("KK dev gamewatch:" lines,
// king_kong.exe+0x... frames; a thread whose stack has not changed since its
// last line is not logged again during the same gap). When the next frame
// comes, "gap ended after N ms" is logged. Recompiled guest functions are named
// sub_XXXXXXXX in king_kong.map: kk\out\task217\symbolize.py turns the
// offsets into names.
#if defined(KK_DEV_TOOLS) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

namespace kk::dev {
namespace {

using Clock = std::chrono::steady_clock;
std::atomic<int64_t> g_last_frame_us{0};
constexpr int kFrames = 40;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

int Capture(HANDLE h, uint64_t* out) {
  if (SuspendThread(h) == DWORD(-1)) return 0;
  int n = 0;
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  if (GetThreadContext(h, &ctx)) {
    while (n < kFrames && ctx.Rip) {
      out[n++] = ctx.Rip;
      DWORD64 image = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
      if (!fn) {
        if (!ctx.Rsp) break;
        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
        ctx.Rsp += 8;
      } else {
        void* handler = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler, &frame, nullptr);
      }
    }
  }
  ResumeThread(h);
  return n;
}

std::string Name(uint64_t addr) {
  HMODULE m = nullptr;
  char buf[96];
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(addr), &m) || !m) {
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(addr));
    return buf;
  }
  char path[MAX_PATH] = {};
  GetModuleFileNameA(m, path, MAX_PATH);
  std::string mod(path);
  mod = mod.substr(mod.find_last_of("\\/") + 1);
  std::snprintf(buf, sizeof(buf), "+0x%llx", static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(m)));
  return mod + buf;
}

std::string Description(HANDLE h) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const Fn get = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!get) return {};
  PWSTR w = nullptr;
  std::string s;
  if (SUCCEEDED(get(h, &w)) && w) {
    for (const wchar_t* p = w; *p; ++p) s.push_back(*p < 128 ? char(*p) : '?');
    LocalFree(w);
  }
  return s;
}

void WatchLoop(int64_t threshold_ms) {
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  bool in_gap = false;
  int64_t next_sample = 0;
  std::unordered_map<DWORD, std::string> last;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int64_t last_frame = g_last_frame_us.load(std::memory_order_relaxed);
    const int64_t now = NowUs();
    const int64_t gap_ms = (now - last_frame) / 1000;
    if (in_gap && gap_ms < threshold_ms) {
      in_gap = false;
      REXLOG_INFO("KK dev gamewatch: gap ended (next frame came; the gap was over {} ms)", threshold_ms);
      last.clear();
      continue;
    }
    if (gap_ms < threshold_ms) continue;
    if (!in_gap) {
      in_gap = true;
      next_sample = 0;
    }
    if (now < next_sample) continue;
    next_sample = now + 250000;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) continue;
    std::vector<DWORD> tids;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
      if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    for (DWORD tid : tids) {
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
      if (!h) continue;
      uint64_t frames[kFrames];
      const int n = Capture(h, frames);  // nothing allocated while the thread is suspended
      std::string stack;
      bool game = false;
      for (int i = 0; i < n; ++i) {
        const std::string s = Name(frames[i]);
        if (s.rfind("king_kong.exe", 0) == 0) game = true;
        stack += (i ? " < " : "") + s;
      }
      if (game && last[tid] != stack) {
        last[tid] = stack;
        REXLOG_INFO("KK dev gamewatch: gap {} ms tid {} '{}': {}", gap_ms, tid, Description(h), stack);
      }
      CloseHandle(h);
    }
  }
}

}  // namespace

void GameWatchFrame() {
  g_last_frame_us.store(NowUs(), std::memory_order_relaxed);
  static const bool started = [] {
    const char* v = std::getenv("KK_DEV_GAME_WATCH");
    if (!v || !*v) return false;
    int64_t ms = std::atoll(v);
    if (ms <= 1) ms = 1000;
    REXLOG_INFO("KK dev gamewatch: on, threshold {} ms", ms);
    std::thread(WatchLoop, ms).detach();
    return true;
  }();
  (void)started;
}

}  // namespace kk::dev

#endif
