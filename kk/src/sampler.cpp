// Developer sampling profiler (Windows).
//
// Set KK_PROFILE=<seconds> to sample every thread's instruction pointer at
// ~1 kHz, starting 8 s after launch, then write profile.txt next to the exe:
// per-thread sample counts and the hottest addresses as module+RVA. Resolve
// RVAs with: llvm-symbolizer --obj=king_kong.exe --relative-address 0x<rva>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

std::string ModuleOf(uintptr_t addr, uintptr_t& base_out) {
  HMODULE mod = nullptr;
  base_out = 0;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(addr), &mod)) {
    return "?";
  }
  base_out = reinterpret_cast<uintptr_t>(mod);
  char path[MAX_PATH];
  GetModuleFileNameA(mod, path, MAX_PATH);
  const char* name = std::max(std::strrchr(path, '\\'), std::strrchr(path, '/'));
  return name ? name + 1 : path;
}

void Run(int seconds) {
  std::this_thread::sleep_for(std::chrono::seconds(8));
  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  std::unordered_map<DWORD, std::unordered_map<uintptr_t, uint32_t>> hist;
  std::unordered_map<DWORD, HANDLE> handles;

  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  auto next_enum = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < end) {
    if (std::chrono::steady_clock::now() >= next_enum) {
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 te{sizeof(te)};
      for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if (!handles.count(te.th32ThreadID)) {
          HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
          if (h) handles[te.th32ThreadID] = h;
        }
      }
      CloseHandle(snap);
      next_enum += std::chrono::milliseconds(500);
    }
    for (auto& [tid, h] : handles) {
      if (SuspendThread(h) == DWORD(-1)) continue;
      CONTEXT c{};
      c.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(h, &c)) hist[tid][c.Rip]++;
      ResumeThread(h);
    }
    Sleep(1);
  }

  FILE* f = std::fopen("profile.txt", "w");
  if (!f) return;
  std::vector<std::pair<DWORD, uint64_t>> totals;
  for (auto& [tid, m] : hist) {
    uint64_t n = 0;
    for (auto& [rip, c] : m) n += c;
    totals.push_back({tid, n});
  }
  std::sort(totals.begin(), totals.end(), [](auto& a, auto& b) { return a.second > b.second; });
  for (size_t ti = 0; ti < totals.size(); ++ti) {
    auto [tid, n] = totals[ti];
    // Aggregate by (module, rva) and drop samples parked in the kernel wait path.
    std::map<std::pair<std::string, uintptr_t>, uint32_t> by_loc;
    for (auto& [rip, c] : hist[tid]) {
      uintptr_t base;
      std::string mod = ModuleOf(rip, base);
      by_loc[{mod, rip - base}] += c;
    }
    std::vector<std::pair<std::pair<std::string, uintptr_t>, uint32_t>> v(by_loc.begin(), by_loc.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::fprintf(f, "== thread %lu: %llu samples\n", tid, static_cast<unsigned long long>(n));
    for (size_t i = 0; i < std::min<size_t>(v.size(), 5000); ++i) {
      std::fprintf(f, "%6u %5.1f%%  %s+0x%llX\n", v[i].second, 100.0 * v[i].second / n,
                   v[i].first.first.c_str(), static_cast<unsigned long long>(v[i].first.second));
    }
  }
  std::fclose(f);
  for (auto& [tid, h] : handles) CloseHandle(h);
}

const bool g_started = [] {
  const char* s = std::getenv("KK_PROFILE");
  if (s && std::atoi(s) > 0) std::thread(Run, std::atoi(s)).detach();
  return true;
}();

}  // namespace

#endif  // _WIN32
