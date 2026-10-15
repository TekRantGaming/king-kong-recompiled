#include "benchmark_monitor.h"
#include "hooks/kk_native_ui.h"
#include "plugin/native_benchmark_stats.h"
#include <map>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

namespace kk::benchmark {
namespace {
using Clock = std::chrono::steady_clock;
#if defined(_WIN32)
uint64_t Ticks(FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
uint64_t Cpu(HANDLE h, bool process) {
  FILETIME c{}, e{}, k{}, u{};
  if (!(process ? GetProcessTimes(h, &c, &e, &k, &u) : GetThreadTimes(h, &c, &e, &k, &u))) return 0;
  return Ticks(k) + Ticks(u);
}
// Counter and optional vendor calls live exclusively on this worker.
struct Counters {
  PDH_HQUERY query = nullptr;
  PDH_HCOUNTER gpu = nullptr;
  HMODULE nvml = nullptr;
  void* device = nullptr;
  using DeviceUInt = int(*)(void*, unsigned*);
  DeviceUInt power = nullptr;
  int(*temperature)(void*, unsigned, unsigned*) = nullptr;
  int(*shutdown)() = nullptr;
  Counters() {
    if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS &&
        PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu) == ERROR_SUCCESS)
      PdhCollectQueryData(query);
    // Installed NVIDIA driver API only. No SDK/runtime download or required dependency.
    nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nvml) return;
    auto init = reinterpret_cast<int(*)()>(GetProcAddress(nvml, "nvmlInit_v2"));
    shutdown = reinterpret_cast<int(*)()>(GetProcAddress(nvml, "nvmlShutdown"));
    auto count = reinterpret_cast<int(*)(unsigned*)>(GetProcAddress(nvml, "nvmlDeviceGetCount_v2"));
    auto get = reinterpret_cast<int(*)(unsigned, void**)>(GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex_v2"));
    unsigned n = 0;
    if (!init || !shutdown || !count || !get || init() || count(&n) || n != 1 || get(0, &device)) {
      device = nullptr; return;
    }
    power = reinterpret_cast<DeviceUInt>(GetProcAddress(nvml, "nvmlDeviceGetPowerUsage"));
    temperature = reinterpret_cast<decltype(temperature)>(GetProcAddress(nvml, "nvmlDeviceGetTemperature"));
  }
  ~Counters() { if (query) PdhCloseQuery(query); if (nvml) { if (shutdown) shutdown(); FreeLibrary(nvml); } }
  double Gpu() {
    if (!gpu || PdhCollectQueryData(query) != ERROR_SUCCESS) return -1;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA) return -1;
    std::vector<unsigned char> buffer(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return -1;
    const auto prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    double busiest = -1;
    for (DWORD i = 0; i < count; ++i)
      if (std::wstring_view(items[i].szName).starts_with(prefix) &&
          (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA))
        busiest = std::max(busiest, std::clamp(items[i].FmtValue.doubleValue, 0.0, 100.0));
    return busiest;
  }
  void Sample(Values& v, uint32_t vendor) {
    v[kGpuUse] = Gpu();
    PROCESS_MEMORY_COUNTERS_EX m{}; m.cb = sizeof(m);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m)))
      v[kRam] = double(m.WorkingSetSize) / 1048576;
    MEMORYSTATUSEX system{sizeof(system)};
    if (GlobalMemoryStatusEx(&system)) v[kSystemRam] = double(system.ullTotalPhys - system.ullAvailPhys) / 1048576;
    if (device && vendor == 0x10de) {
      unsigned value = 0;
      auto clocks = reinterpret_cast<int(*)(void*, unsigned, unsigned*)>(GetProcAddress(nvml, "nvmlDeviceGetClockInfo"));
      if (clocks && !clocks(device, 0, &value)) v[kClock] = value;
      if (temperature && !temperature(device, 0, &value)) v[kTemperature] = value;
      if (power && !power(device, &value)) v[kPower] = value / 1000.0;
    }
  }
};
#endif
}
void Monitor::Start() { worker_ = std::thread([this] { Run(); }); }
void Monitor::Stop() {
  { std::lock_guard lock(stop_mutex_); stop_ = true; }
  stop_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
#if defined(_WIN32)
  if (auto h = game_thread_.exchange(nullptr)) CloseHandle(h);
#endif
}
void Monitor::GameThread() {
#if defined(_WIN32)
  if (!game_thread_.load(std::memory_order_relaxed)) {
    HANDLE h = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
      game_thread_.store(h);
  }
#endif
}
void Monitor::Record(double ms, double latency) {
  const auto write = write_.load(std::memory_order_relaxed);
  if (write - read_.load(std::memory_order_acquire) >= kQueue) { ++dropped_; return; }
  queue_[write % kQueue] = {ms, latency, chapter_.load()};
  write_.store(write + 1, std::memory_order_release);
}
void Monitor::Run() {
  MonitorView view;
  std::vector<double> frames;
  frames.reserve(65536);
  size_t recent_at = 0;
  double last_latency = -1;
  std::array<float, 240> recent{};
  auto last = Clock::now();
  int previous_chapter = -1;
  NrBenchmarkStats previous;
#if defined(_WIN32)
  Counters counters;
  const auto process = GetCurrentProcess();
  uint64_t process_cpu = Cpu(process, true), game_cpu = 0;
  const double cpus = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
  const auto module = GetModuleHandleW(L"rexgpu-native.dll");
  const auto read_native = module ? reinterpret_cast<uint32_t(*)(NrBenchmarkStats*)>(GetProcAddress(module, "nr_benchmark_stats")) : nullptr;
  const auto enable_native = module ? reinterpret_cast<void(*)(uint32_t)>(GetProcAddress(module, "nr_benchmark_enable")) : nullptr;
  if (enable_native) enable_native(1);
#endif
  for (;;) {
    const auto start = Clock::now();
    auto read = read_.load(std::memory_order_relaxed);
    const auto write = write_.load(std::memory_order_acquire);
    size_t interval_count = 0;
    double interval_ms = 0;
    while (read < write) {
      const auto f = queue_[read++ % kQueue];
      ++interval_count; interval_ms += f.ms; last_latency = f.latency;
      frames.push_back(f.ms);
      recent[recent_at++ % recent.size()] = float(f.ms);
      if (f.chapter >= 0 && f.chapter < 4) {
        view.metrics[0][kLatency].Add(f.latency, f.ms / 1000);
        view.metrics[f.chapter + 1][kLatency].Add(f.latency, f.ms / 1000);
      }
    }
    read_.store(read, std::memory_order_release);
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    const int chapter = chapter_.load();
    Values v = MissingValues();
    const auto effects = nr::kk::EffectTimes();
    view.width = effects.input_width; view.height = effects.input_height;
    view.output_width = effects.output_width; view.output_height = effects.output_height;
    view.api = effects.api; view.upscaler = effects.upscaler;
    NrBenchmarkStats native;
#if defined(_WIN32)
    counters.Sample(v, effects.vendor);
    const auto pc = Cpu(process, true);
    if (seconds > 0 && pc >= process_cpu) v[kCpuUse] = (pc - process_cpu) / (seconds * 100000.0 * cpus);
    process_cpu = pc;
    const auto gc = game_thread_.load() ? Cpu(game_thread_.load(), false) : 0;
    if (read_native && read_native(&native)) {
      v[kGpuMs] = native.gpu_ms;
      if (native.memory_valid) { v[kVram] = native.vram_used / 1048576.0; v[kBudget] = native.vram_budget / 1048576.0; }
      if (native.frame > previous.frame && previous.frame && chapter == previous_chapter && chapter >= 0) {
        const double n = double(native.frame - previous.frame);
        if (native.render_cpu_us >= previous.render_cpu_us) v[kRenderMs] = (native.render_cpu_us - previous.render_cpu_us) / (1000.0 * n);
        if (gc && game_cpu && gc >= game_cpu) v[kGameMs] = (gc - game_cpu) / (10000.0 * n);
        if (native.pipeline_total_ms >= previous.pipeline_total_ms) v[kPipelineMs] = (native.pipeline_total_ms - previous.pipeline_total_ms) / n;
      }
    }
    game_cpu = gc;
#endif
    // Discard counter windows spanning chapter loads/transitions.
    if (chapter >= 0 && chapter < 4 && chapter == previous_chapter)
      for (size_t i = 0; i < kMetricCount; ++i) if (i != kLatency) {
        view.metrics[0][i].Add(v[i], seconds); view.metrics[chapter + 1][i].Add(v[i], seconds);
      }
    previous = native; previous_chapter = chapter; last = now;
    view.current = v;
    view.current[kLatency] = last_latency;
    view.frames = Summarize(frames);
    view.recent_count = std::min(recent_at, recent.size());
    for (size_t i = 0; i < view.recent_count; ++i) {
      view.recent[i] = recent[(recent_at - view.recent_count + i) % recent.size()];
    }
    view.current_fps = interval_ms > 0 ? 1000 * interval_count / interval_ms : 0;
    view.dropped = dropped_; ++view.samples;
    view.worker_ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    { std::lock_guard lock(view_mutex_); view_ = view; }
    std::unique_lock lock(stop_mutex_);
    if (stop_) break;
    stop_cv_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stop_; });
  }
#if defined(_WIN32)
  if (enable_native) enable_native(0);
#endif
}
}
