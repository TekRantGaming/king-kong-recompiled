// Developer-only: hardware data watchpoints ("who touched this byte?").
//
// KK_DEV_WATCH="829868CB:1:w,82C0CDAB:4:rw" (guest address : length 1/2/4/8 :
// w = writes, rw = reads and writes; up to 4) with KK_DEV_WATCH_AT="57,70"
// (seconds from the save menu opening: arm, then disarm). While armed, every
// game thread has the CPU's debug registers set; each access traps once (after
// the instruction), and a background thread logs "KK dev watch:" lines with the
// instruction address and nearby return addresses as king_kong.exe offsets.
// Turn offsets into game functions with king_kong.map (tools: scratchpad).

#if defined(_WIN32)

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
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

#include "guest.h"

namespace kk {
namespace {

struct Watch {
  uint32_t guest;
  int len;      // 1, 2, 4 or 8
  bool reads;   // also trap reads
};

struct Hit {
  DWORD thread;
  int slot;
  uint64_t rip;
  uint64_t callers[6];
};

constexpr size_t kRing = 4096;
Hit g_ring[kRing];
std::atomic<uint64_t> g_written{0};
std::vector<Watch> g_watches;
std::atomic<bool> g_armed{false};
uintptr_t g_exe_begin = 0, g_exe_end = 0;

bool InExe(uint64_t a) { return a >= g_exe_begin && a < g_exe_end; }

LONG CALLBACK OnSingleStep(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = info->ContextRecord;
  const DWORD64 hit = c->Dr6 & 0xF;
  if (!hit) return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t n = g_written.fetch_add(1);
  Hit& h = g_ring[n % kRing];
  h.thread = GetCurrentThreadId();
  h.slot = hit & 1 ? 0 : hit & 2 ? 1 : hit & 4 ? 2 : 3;
  h.rip = c->Rip;
  // Return addresses into the game: code addresses found on the stack.
  int found = 0;
  const auto* sp = reinterpret_cast<const uint64_t*>(c->Rsp);
  for (int i = 0; i < 256 && found < 6; ++i) {
    const uint64_t v = sp[i];
    if (InExe(v)) h.callers[found++] = v;
  }
  for (; found < 6; ++found) h.callers[found] = 0;
  c->Dr6 = 0;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void SetThreadWatches(DWORD tid, bool arm) {
  HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
  if (!t) return;
  if (SuspendThread(t) != DWORD(-1)) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(t, &c)) {
      DWORD64 dr7 = 0;
      DWORD64* regs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
      for (size_t i = 0; i < 4; ++i) {
        if (!arm || i >= g_watches.size()) {
          *regs[i] = 0;
          continue;
        }
        const Watch& w = g_watches[i];
        *regs[i] = reinterpret_cast<DWORD64>(g_guest_base + w.guest);
        const DWORD64 rw = w.reads ? 3 : 1;
        const DWORD64 len = w.len == 1 ? 0 : w.len == 2 ? 1 : w.len == 8 ? 2 : 3;
        dr7 |= DWORD64(1) << (i * 2);
        dr7 |= (rw | (len << 2)) << (16 + i * 4);
      }
      c.Dr7 = dr7;
      c.Dr6 = 0;
      SetThreadContext(t, &c);
    }
    ResumeThread(t);
  }
  CloseHandle(t);
}

void SetAllThreads(bool arm) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId())
      SetThreadWatches(te.th32ThreadID, arm);
  CloseHandle(snap);
}

// Logs new hits, grouped by instruction, and keeps the watches on threads that
// started after arming.
void Worker() {
  uint64_t read = 0;
  std::map<uint64_t, int> counts;  // rip -> hits
  bool was_armed = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const bool armed = g_armed.load();
    if (armed) SetAllThreads(true);
    if (!armed && was_armed) {
      SetAllThreads(false);
      REXLOG_INFO("KK dev watch: disarmed, {} hits", g_written.load());
    }
    was_armed = armed;
    const uint64_t written = g_written.load();
    if (written - read > kRing) read = written - kRing;
    for (; read < written; ++read) {
      const Hit& h = g_ring[read % kRing];
      if (++counts[h.rip] > 3) continue;  // first few of each instruction
      std::string callers;
      for (uint64_t c : h.callers)
        if (c) callers += " " + std::to_string(c - g_exe_begin);
      REXLOG_INFO("KK dev watch: slot {} ({:08X}) thread {} rip +{} callers{}", h.slot,
                  g_watches[h.slot].guest, h.thread, h.rip - g_exe_begin, callers);
    }
  }
}

}  // namespace

void DevWatchTick(double t) {
  static const std::vector<double> times = [] {
    std::vector<double> out;
    if (const char* v = std::getenv("KK_DEV_WATCH_AT"); v && *v) {
      std::stringstream all(v);
      for (std::string s; std::getline(all, s, ',');) out.push_back(std::stod(s));
    }
    return out;
  }();
  static bool started = false;
  if (!started) {
    started = true;
    const char* v = std::getenv("KK_DEV_WATCH");
    if (!v || !*v || !g_guest_base) return;
    std::stringstream all(v);
    for (std::string item; std::getline(all, item, ',') && g_watches.size() < 4;) {
      Watch w{0, 4, false};
      std::stringstream parts(item);
      std::string a, l, m;
      std::getline(parts, a, ':');
      std::getline(parts, l, ':');
      std::getline(parts, m, ':');
      w.guest = uint32_t(std::stoul(a, nullptr, 16));
      if (!l.empty()) w.len = std::stoi(l);
      w.reads = m == "rw";
      w.guest &= ~uint32_t(w.len - 1);  // debug registers need aligned addresses
      g_watches.push_back(w);
    }
    // king_kong.exe's address range, from its PE header.
    HMODULE exe = GetModuleHandleW(nullptr);
    g_exe_begin = reinterpret_cast<uintptr_t>(exe);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(exe) + dos->e_lfanew);
    g_exe_end = g_exe_begin + nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, OnSingleStep);
    std::thread(Worker).detach();
    REXLOG_INFO("KK dev watch: {} watch(es) ready", g_watches.size());
  }
  if (g_watches.empty() || times.empty()) return;
  const bool arm = t >= times[0] && (times.size() < 2 || t < times[1]);
  if (arm != g_armed.load()) {
    g_armed = arm;
    REXLOG_INFO("KK dev watch: {} at {:.1f} s", arm ? "armed" : "disarming", t);
  }
}

}  // namespace kk

#else

namespace kk {
void DevWatchTick(double) {}
}  // namespace kk

#endif
