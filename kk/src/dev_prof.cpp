// Developer-only call profiler for the recompiled game code (-DKK_DEV_PROFILE=ON,
// preset kk-prof): the generated sources are built with clang's
// -finstrument-functions-after-inlining, so every game function entry calls
// __cyg_profile_func_enter, which counts it.
//
// KK_DEV_PROF_AT="82,85,100,103" (seconds from the save menu opening): snapshot
// the counts at each time; after the last one, prof_diff.txt lists every
// function that ran in any window between snapshots, as a king_kong.exe offset
// followed by its call count in each window. Names come from king_kong.map.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace kk {
namespace {

uintptr_t g_begin = 0, g_end = 0;
std::atomic<uint32_t>* g_counts = nullptr;  // one per 16 bytes of king_kong.exe

__attribute__((no_instrument_function)) bool Setup() {
  HMODULE exe = GetModuleHandleW(nullptr);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(exe) + dos->e_lfanew);
  const size_t slots = nt->OptionalHeader.SizeOfImage / 16;
  void* mem = VirtualAlloc(nullptr, slots * sizeof(uint32_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!mem) return false;
  g_counts = static_cast<std::atomic<uint32_t>*>(mem);
  g_begin = reinterpret_cast<uintptr_t>(exe);
  g_end = g_begin + nt->OptionalHeader.SizeOfImage;
  return true;
}

}  // namespace

__attribute__((no_instrument_function)) void DevProfTick(double t) {
  static const bool ready = Setup();
  static std::vector<double> times = [] {
    std::vector<double> out;
    if (const char* v = std::getenv("KK_DEV_PROF_AT"); v && *v) {
      std::stringstream all(v);
      for (std::string s; std::getline(all, s, ',');) out.push_back(std::stod(s));
    }
    return out;
  }();
  static std::vector<std::vector<uint32_t>> snaps;
  static const size_t wanted = times.size();
  if (!ready || times.empty() || t < times.front()) return;
  times.erase(times.begin());
  const size_t slots = (g_end - g_begin) / 16;
  std::vector<uint32_t> snap(slots);
  for (size_t i = 0; i < slots; ++i) snap[i] = g_counts[i].load(std::memory_order_relaxed);
  snaps.push_back(std::move(snap));
  REXLOG_INFO("KK dev prof: snapshot {} at {:.1f} s", snaps.size(), t);
  if (snaps.size() != wanted || wanted < 2) return;
  FILE* f = std::fopen("prof_diff.txt", "w");
  if (!f) return;
  size_t rows = 0;
  for (size_t i = 0; i < slots; ++i) {
    bool any = false;
    for (size_t k = 1; k < snaps.size() && !any; ++k) any = snaps[k][i] != snaps[k - 1][i];
    if (!any) continue;
    std::fprintf(f, "%zu", i * 16);
    for (size_t k = 1; k < snaps.size(); ++k) std::fprintf(f, " %u", snaps[k][i] - snaps[k - 1][i]);
    std::fprintf(f, "\n");
    ++rows;
  }
  std::fclose(f);
  REXLOG_INFO("KK dev prof: wrote prof_diff.txt, {} functions", rows);
}

}  // namespace kk

extern "C" __attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* fn, void*) {
  const auto a = reinterpret_cast<uintptr_t>(fn);
  if (kk::g_counts && a >= kk::g_begin && a < kk::g_end)
    kk::g_counts[(a - kk::g_begin) / 16].fetch_add(1, std::memory_order_relaxed);
}

extern "C" __attribute__((no_instrument_function)) void __cyg_profile_func_exit(void*, void*) {}

#else

namespace kk {
void DevProfTick(double) {}
}  // namespace kk

#endif
