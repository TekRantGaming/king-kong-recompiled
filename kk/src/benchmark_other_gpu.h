// Other programs' GPU use for the benchmark (task 172), as a pure function so tests can pin it.
// Input: the "\GPU Engine(*)\Utilization Percentage" instances
//   pid_<n>_luid_<a>_<b>_phys_<p>_eng_<e>_engtype_<t>
// and which pids are the game's own (this process, its startup parent, the launcher that started
// it and is closing: every process of the same king_kong.exe).
// Counted: other processes' use of the shader engines (3D, Compute, CUDA, Graphics) of the adapter
// the game renders on, summed per engine; the busiest engine is the result.
// Not counted: the game's own processes; other adapters (a laptop's integrated GPU composing or
// decoding video does not slow the game's GPU); fixed-function engines (video encode/decode,
// copy, security), which run beside rendering: recording or streaming the game (NVENC), or a
// video playing, would otherwise mark every pass busy at a steady 40-90%.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace kk::benchmark {
struct GpuInstance {
  std::wstring_view name;
  double value = 0;  // percent
};

inline uint32_t InstancePid(std::wstring_view name) {
  if (!name.starts_with(L"pid_")) return 0;
  uint32_t pid = 0;
  for (size_t i = 4; i < name.size() && name[i] >= L'0' && name[i] <= L'9'; ++i) pid = pid * 10 + uint32_t(name[i] - L'0');
  return pid;
}
// "luid_<a>_<b>" (the adapter), empty when missing.
inline std::wstring_view InstanceAdapter(std::wstring_view name) {
  const auto at = name.find(L"luid_"), phys = name.find(L"_phys_");
  return at == std::wstring_view::npos || phys == std::wstring_view::npos || phys < at ? std::wstring_view()
                                                                                       : name.substr(at, phys - at);
}
// "luid_..._eng_<e>_engtype_<t>" (one engine of one adapter), empty when missing.
inline std::wstring_view InstanceEngine(std::wstring_view name) {
  const auto at = name.find(L"luid_");
  return at == std::wstring_view::npos ? std::wstring_view() : name.substr(at);
}
inline bool ShaderEngine(std::wstring_view name) {
  const auto at = name.find(L"_engtype_");
  if (at == std::wstring_view::npos) return false;
  const auto type = name.substr(at + 9);
  return type.starts_with(L"3D") || type.find(L"Compute") != std::wstring_view::npos ||
         type.starts_with(L"Cuda") || type.starts_with(L"Graphics");
}

struct OtherGpuResult {
  double shader = -1;       // the reported value: other processes, game's adapter, shader engines
  double all_engines = -1;  // the old value: other pids, any adapter or engine (for developer logs)
};

// own_pid: this process; own: every pid of the game (this one included).
inline OtherGpuResult OtherGpuUse(const std::vector<GpuInstance>& instances, uint32_t own_pid,
                                  const std::set<uint32_t>& own) {
  OtherGpuResult out;
  if (instances.empty()) return out;
  // The game's adapters: the one with its busiest engine, or every one it has a context on when idle.
  std::set<std::wstring_view> adapters;
  double busiest = 0;
  std::wstring_view busiest_adapter;
  for (const auto& i : instances) {
    if (InstancePid(i.name) != own_pid) continue;
    const auto adapter = InstanceAdapter(i.name);
    if (adapter.empty()) continue;
    adapters.insert(adapter);
    if (i.value > busiest) { busiest = i.value; busiest_adapter = adapter; }
  }
  if (busiest > 0) adapters = {busiest_adapter};
  std::map<std::wstring_view, double> shader, all;
  for (const auto& i : instances) {
    const uint32_t pid = InstancePid(i.name);
    const auto engine = InstanceEngine(i.name);
    if (!pid || engine.empty()) continue;
    const double v = std::clamp(i.value, 0.0, 100.0);
    if (pid != own_pid) all[engine] += v;
    if (own.count(pid) || !ShaderEngine(i.name)) continue;
    if (!adapters.empty() && !adapters.count(InstanceAdapter(i.name))) continue;
    shader[engine] += v;
  }
  out.shader = out.all_engines = 0;
  for (const auto& [k, v] : shader) out.shader = std::max(out.shader, std::min(100.0, v));
  for (const auto& [k, v] : all) out.all_engines = std::max(out.all_engines, std::min(100.0, v));
  return out;
}
}  // namespace kk::benchmark
