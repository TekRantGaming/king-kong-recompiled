#include "benchmark_monitor.h"
#include "benchmark_other_gpu.h"
#include "hooks/kk_native_ui.h"
#include "plugin/native_benchmark_stats.h"
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <pdhmsg.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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
  ~Counters() {
#if defined(KK_DEV_TOOLS)
    if (dump) std::fclose(dump);
#endif
    if (query) PdhCloseQuery(query); if (nvml) { if (shutdown) shutdown(); FreeLibrary(nvml); } }
  double Gpu() {
    last_count = 0;
    if (!gpu || PdhCollectQueryData(query) != ERROR_SUCCESS) return -1;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA) return -1;
    std::vector<unsigned char> buffer(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(gpu, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return -1;
    // The items' names point into this storage: moved, never copied (task 351: a copy left every name
    // pointing into the freed buffer, and the other-GPU-use count read them later: benchmark crashes).
    last = std::move(buffer);
    items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(last.data());
    last_count = count;
    const auto prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    double busiest = -1;
    for (DWORD i = 0; i < count; ++i)
      if (std::wstring_view(items[i].szName).starts_with(prefix) &&
          (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA))
        busiest = std::max(busiest, std::clamp(items[i].FmtValue.doubleValue, 0.0, 100.0));
    return busiest;
  }
  // The whole adapter's load from the last Gpu() sample (benchmark_other_gpu.h, AdapterGpuLoad).
  double GpuTotal() {
    if (!last_count) return -1;
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(last.data());
    std::vector<GpuInstance> instances;
    instances.reserve(last_count);
    for (DWORD i = 0; i < last_count; ++i)
      if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
        instances.push_back({std::wstring_view(items[i].szName), items[i].FmtValue.doubleValue});
    return AdapterGpuLoad(instances, GetCurrentProcessId());
  }
  // Other programs' GPU use (benchmark_other_gpu.h): the game's own processes, other adapters and
  // fixed-function engines (video encode/decode, copy) are not counted. -1 when unavailable.
  std::map<uint32_t, bool> same_exe;  // pid -> runs this king_kong.exe (startup parent, closing launcher)
  std::wstring own_path;
  bool SameExe(uint32_t pid) {
    if (own_path.empty()) {
      wchar_t path[32768]; DWORD size = 32768;
      if (QueryFullProcessImageNameW(GetCurrentProcess(), 0, path, &size)) own_path.assign(path, size);
    }
    bool same = false;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
      wchar_t path[32768]; DWORD size = 32768;
      same = !own_path.empty() && QueryFullProcessImageNameW(h, 0, path, &size) &&
             CompareStringOrdinal(path, int(size), own_path.c_str(), int(own_path.size()), TRUE) == CSTR_EQUAL;
      CloseHandle(h);
    }
    return same;
  }
  double other_all = -1;
  double OtherGpu() {
    other_all = -1;
    if (!gpu || !last_count) return -1;
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(last.data());
    std::vector<GpuInstance> instances;
    instances.reserve(last_count);
    std::map<uint32_t, bool> seen;
    std::set<uint32_t> own{uint32_t(GetCurrentProcessId())};
    for (DWORD i = 0; i < last_count; ++i) {
      if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
        continue;
      const std::wstring_view name(items[i].szName);
      instances.push_back({name, items[i].FmtValue.doubleValue});
      // Only processes with GPU use are looked up; a pid's answer is kept while it has instances.
      const uint32_t pid = InstancePid(name);
      if (!pid || items[i].FmtValue.doubleValue <= 0 || seen.count(pid)) continue;
      const auto it = same_exe.find(pid);
      seen[pid] = it != same_exe.end() ? it->second : SameExe(pid);
    }
    for (const auto& [pid, same] : seen) if (same) own.insert(pid);
    for (const auto& [pid, same] : same_exe) {
      if (seen.count(pid)) continue;
      // Idle this sample: keep the answer only while the pid still has counter instances.
      for (const auto& i : instances) if (InstancePid(i.name) == pid) { seen[pid] = same; if (same) own.insert(pid); break; }
    }
    same_exe = std::move(seen);
    const auto result = OtherGpuUse(instances, GetCurrentProcessId(), own);
    other_all = result.all_engines;
    return result.shader;
  }
  // Every logical processor's busy share since the last call (percent).
  double SystemCpu() {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return -1;
    const uint64_t i = Ticks(idle), k = Ticks(kernel), u = Ticks(user);
    double out = -1;
    if (sys_total && k + u > sys_total) {
      const double total = double(k + u - sys_total), busy = total - double(i - sys_idle);
      out = std::clamp(busy / total * 100, 0.0, 100.0);
    }
    sys_idle = i; sys_total = k + u;
    return out;
  }
  uint64_t sys_idle = 0, sys_total = 0;
  std::vector<unsigned char> last;
  DWORD last_count = 0;
#if defined(KK_DEV_TOOLS)
  // Test aid: KK_DEV_BENCH_GPU_DUMP=<file.csv> writes every GPU engine counter instance the monitor
  // sees (above 0.05%), with its process name, and the own/other values reported for that sample.
  FILE* dump = nullptr;
  bool dump_tried = false;
  std::unordered_map<DWORD, std::string> names;
  Clock::time_point dump_start = Clock::now();
  std::string ProcessName(DWORD pid) {
    if (auto it = names.find(pid); it != names.end()) return it->second;
    std::string out = "?";
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
      wchar_t path[MAX_PATH]; DWORD size = MAX_PATH;
      if (QueryFullProcessImageNameW(h, 0, path, &size)) {
        std::wstring w(path, size);
        w = w.substr(w.find_last_of(L"\\/") + 1);
        out.assign(w.begin(), w.end());
      }
      CloseHandle(h);
    }
    if (out == "?") {  // protected processes (dwm.exe, csrss.exe): the snapshot still names them
      if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W e{}; e.dwSize = sizeof(e);
        for (BOOL more = Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e))
          if (e.th32ProcessID == pid) { std::wstring w(e.szExeFile); out.assign(w.begin(), w.end()); break; }
        CloseHandle(snap);
      }
    }
    return names[pid] = out;
  }
  void Dump(const Values& v) {
    if (!dump_tried) {
      dump_tried = true;
      if (const char* path = std::getenv("KK_DEV_BENCH_GPU_DUMP"); path && *path) {
        dump = std::fopen(path, "w");
        if (dump) std::fprintf(dump, "seconds,kind,pid,process,engine,value,own_pid\n");
      }
    }
    if (!dump) return;
    const double t = std::chrono::duration<double>(Clock::now() - dump_start).count();
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(last.data());
    for (DWORD i = 0; i < last_count; ++i) {
      const std::wstring_view name(items[i].szName);
      const double value = items[i].FmtValue.doubleValue;
      if (!name.starts_with(L"pid_") || value < 0.05) continue;
      const DWORD pid = DWORD(std::wcstoul(std::wstring(name.substr(4)).c_str(), nullptr, 10));
      const auto luid = name.find(L"_luid_");
      const std::wstring engine(luid == std::wstring_view::npos ? name : name.substr(luid + 1));
      std::fprintf(dump, "%.2f,instance,%lu,%s,%s,%.2f,%lu\n", t, pid, ProcessName(pid).c_str(),
                   std::string(engine.begin(), engine.end()).c_str(), value, GetCurrentProcessId());
    }
    std::fprintf(dump, "%.2f,own,,,,%.2f,%lu\n%.2f,other,,,,%.2f,%lu\n%.2f,other_all_engines,,,,%.2f,%lu\n", t,
                 v[kGpuUse], GetCurrentProcessId(), t, v[kOtherGpu], GetCurrentProcessId(), t, other_all,
                 GetCurrentProcessId());
    for (const auto& [pid, same] : same_exe)
      if (same && pid != GetCurrentProcessId()) std::fprintf(dump, "%.2f,own_process,%u,,,,%lu\n", t, pid, GetCurrentProcessId());
    std::fflush(dump);
  }
#endif
  void Sample(Values& v, uint32_t vendor) {
    v[kGpuUse] = Gpu();
    v[kOtherGpu] = OtherGpu();
#if defined(KK_DEV_TOOLS)
    Dump(v);
#endif
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
#else
// Linux: /proc for the processors and memory, the amdgpu driver's sysfs files for the graphics card (its busy
// share, clock, temperature and power). There is no per program GPU share, so the game's GPU use is the whole
// card's and other programs' GPU use stays unavailable; readings a system lacks stay -1.
uint64_t CpuUs(clockid_t clock) {
  timespec t{};
  return clock_gettime(clock, &t) ? 0 : uint64_t(t.tv_sec) * 1000000 + uint64_t(t.tv_nsec) / 1000;
}
double ReadNumber(const std::filesystem::path& path) {
  std::ifstream in(path);
  double v = -1;
  return in >> v ? v : -1;
}
double MeminfoMiB(const std::string& text, const char* key) {
  const auto at = text.find(key);
  return at == std::string::npos ? -1 : std::atof(text.c_str() + at + std::strlen(key)) / 1024;
}
struct Counters {
  std::filesystem::path card, hwmon;  // /sys/class/drm/cardN/device and its hwmon folder (amdgpu)
  uint64_t sys_idle = 0, sys_total = 0;
  Counters() {
    std::error_code ec;
    for (int i = 0; i < 8 && card.empty(); ++i) {
      const std::filesystem::path d = "/sys/class/drm/card" + std::to_string(i) + "/device";
      if (std::filesystem::exists(d / "gpu_busy_percent", ec)) card = d;
    }
    if (!card.empty())
      for (const auto& e : std::filesystem::directory_iterator(card / "hwmon", ec)) { hwmon = e.path(); break; }
  }
  double Gpu() { return card.empty() ? -1 : ReadNumber(card / "gpu_busy_percent"); }
  // Every logical processor's busy share since the last call (percent), from /proc/stat.
  double SystemCpu() {
    std::ifstream in("/proc/stat");
    std::string cpu;
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (!(in >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) || cpu != "cpu") return -1;
    const uint64_t i = idle + iowait, total = user + nice + system + idle + iowait + irq + softirq + steal;
    double out = -1;
    if (sys_total && total > sys_total) {
      const double all = double(total - sys_total), busy = all - double(i - sys_idle);
      out = std::clamp(busy / all * 100, 0.0, 100.0);
    }
    sys_idle = i; sys_total = total;
    return out;
  }
  // The game's resident memory and the system's memory in use (MiB); the system's total in `total`.
  void Memory(double& game, double& used, double* total = nullptr) {
    std::ifstream statm("/proc/self/statm");
    uint64_t size = 0, resident = 0;
    if (statm >> size >> resident) game = double(resident) * double(sysconf(_SC_PAGESIZE)) / 1048576;
    std::ifstream meminfo("/proc/meminfo");
    std::stringstream text;
    text << meminfo.rdbuf();
    const double all = MeminfoMiB(text.str(), "MemTotal:"), available = MeminfoMiB(text.str(), "MemAvailable:");
    if (all > 0 && available >= 0) used = all - available;
    if (total && all > 0) *total = all;
  }
  void Card(double& clock, double& temperature, double& power) {
    if (hwmon.empty()) return;
    if (const double hz = ReadNumber(hwmon / "freq1_input"); hz > 0) clock = hz / 1e6;
    if (const double mc = ReadNumber(hwmon / "temp1_input"); mc > 0) temperature = mc / 1000;
    double uw = ReadNumber(hwmon / "power1_average");
    if (uw <= 0) uw = ReadNumber(hwmon / "power1_input");
    if (uw > 0) power = uw / 1e6;
  }
};
#endif
}
// ---------------------------------------------------------------- the overlay's sampler
struct SystemSampler::Impl {
#if defined(_WIN32)
  Counters counters;
  uint64_t process_cpu = 0;
  Clock::time_point last{};
  double cpus = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
#else
  Counters counters;
  uint64_t process_cpu = 0;
  Clock::time_point last{};
  double cpus = double(std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN)));
#endif
};
SystemSampler::SystemSampler() : impl_(std::make_unique<Impl>()) {
#if defined(_WIN32)
  impl_->process_cpu = Cpu(GetCurrentProcess(), true);
  impl_->last = Clock::now();
  impl_->counters.SystemCpu();  // the first call only starts the window
#else
  impl_->process_cpu = CpuUs(CLOCK_PROCESS_CPUTIME_ID);
  impl_->last = Clock::now();
  impl_->counters.SystemCpu();
#endif
}
SystemSampler::~SystemSampler() = default;
SystemSample SystemSampler::Sample(uint32_t vendor) {
  SystemSample out;
#if defined(_WIN32)
  Counters& c = impl_->counters;
  out.gpu_own = c.Gpu();
  out.gpu_load = c.GpuTotal();
  out.cpu_load = c.SystemCpu();
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - impl_->last).count();
  const uint64_t pc = Cpu(GetCurrentProcess(), true);
  if (seconds > 0 && pc >= impl_->process_cpu) out.cpu_game = std::min(100.0, (pc - impl_->process_cpu) / (seconds * 100000.0 * impl_->cpus));
  impl_->process_cpu = pc;
  impl_->last = now;
  PROCESS_MEMORY_COUNTERS_EX m{}; m.cb = sizeof(m);
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m)))
    out.ram_game = double(m.WorkingSetSize) / 1048576;
  MEMORYSTATUSEX system{sizeof(system)};
  if (GlobalMemoryStatusEx(&system)) {
    out.ram_used = double(system.ullTotalPhys - system.ullAvailPhys) / 1048576;
    out.ram_total = double(system.ullTotalPhys) / 1048576;
  }
  if (c.device && vendor == 0x10de) {
    unsigned value = 0;
    auto clocks = reinterpret_cast<int(*)(void*, unsigned, unsigned*)>(GetProcAddress(c.nvml, "nvmlDeviceGetClockInfo"));
    if (clocks && !clocks(c.device, 0, &value)) out.gpu_clock = value;
    if (c.temperature && !c.temperature(c.device, 0, &value)) out.gpu_temperature = value;
    if (c.power && !c.power(c.device, &value)) out.gpu_power = value / 1000.0;
  }
#else
  (void)vendor;
  Counters& c = impl_->counters;
  out.gpu_own = out.gpu_load = c.Gpu();
  out.cpu_load = c.SystemCpu();
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - impl_->last).count();
  const uint64_t pc = CpuUs(CLOCK_PROCESS_CPUTIME_ID);
  if (seconds > 0 && pc >= impl_->process_cpu) out.cpu_game = std::min(100.0, (pc - impl_->process_cpu) / (seconds * 10000.0 * impl_->cpus));
  impl_->process_cpu = pc;
  impl_->last = now;
  c.Memory(out.ram_game, out.ram_used, &out.ram_total);
  c.Card(out.gpu_clock, out.gpu_temperature, out.gpu_power);
#endif
  return out;
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
#else
  if (!game_clock_set_.load(std::memory_order_relaxed)) {
    clockid_t clock{};
    if (pthread_getcpuclockid(pthread_self(), &clock) == 0) {
      game_clock_.store(clock);
      game_clock_set_.store(true);
    }
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
#else
  Counters counters;
  uint64_t process_cpu = CpuUs(CLOCK_PROCESS_CPUTIME_ID), game_cpu = 0;
  const double cpus = double(std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN)));
  void* module = dlopen("librexgpu-native.so", RTLD_NOW | RTLD_NOLOAD);
  const auto read_native = module ? reinterpret_cast<uint32_t(*)(NrBenchmarkStats*)>(dlsym(module, "nr_benchmark_stats")) : nullptr;
  const auto enable_native = module ? reinterpret_cast<void(*)(uint32_t)>(dlsym(module, "nr_benchmark_enable")) : nullptr;
  if (enable_native) enable_native(1);
  counters.SystemCpu();  // the first call only starts the window
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
    const int chapter = chapter_.load(), pass = pass_.load();
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
    if (const double system = counters.SystemCpu(); system >= 0 && v[kCpuUse] >= 0)
      v[kOtherCpu] = std::max(0.0, system - v[kCpuUse]);
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
#else
    v[kGpuUse] = counters.Gpu();
    counters.Memory(v[kRam], v[kSystemRam]);
    counters.Card(v[kClock], v[kTemperature], v[kPower]);
    const auto pc = CpuUs(CLOCK_PROCESS_CPUTIME_ID);
    if (seconds > 0 && pc >= process_cpu) v[kCpuUse] = (pc - process_cpu) / (seconds * 10000.0 * cpus);
    process_cpu = pc;
    if (const double system = counters.SystemCpu(); system >= 0 && v[kCpuUse] >= 0)
      v[kOtherCpu] = std::max(0.0, system - v[kCpuUse]);
    const auto gc = game_clock_set_.load() ? CpuUs(game_clock_.load()) : 0;
    if (read_native && read_native(&native)) {
      v[kGpuMs] = native.gpu_ms;
      if (native.memory_valid) { v[kVram] = native.vram_used / 1048576.0; v[kBudget] = native.vram_budget / 1048576.0; }
      if (native.frame > previous.frame && previous.frame && chapter == previous_chapter && chapter >= 0) {
        const double n = double(native.frame - previous.frame);
        if (native.render_cpu_us >= previous.render_cpu_us) v[kRenderMs] = (native.render_cpu_us - previous.render_cpu_us) / (1000.0 * n);
        if (gc && game_cpu && gc >= game_cpu) v[kGameMs] = (gc - game_cpu) / (1000.0 * n);
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
    if (chapter >= 0 && chapter < 4 && chapter == previous_chapter && pass >= 0 && pass < kMaxPasses) {
      for (size_t i = 0; i < kPassMetricCount; ++i)
        view.background[size_t(pass) * kRouteChapters + size_t(chapter)][i].Add(v[kPassMetrics[i]], seconds);
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
  if (enable_native) enable_native(0);
}
}
