// Beta 7 uploads, the game's side (telemetry.h). The decisions and payloads are in telemetry_upload.cpp and
// telemetry_logic.h; this file collects the numbers while playing and runs the uploads on worker threads.

#include "telemetry.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <cpuid.h>
#include <time.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "benchmark.h"
#include "frame_stats.h"
#include "hooks/kk_native_ui.h"
#include "http.h"
#include "online.h"
#include "platform.h"
#include "plugin/native_benchmark_stats.h"
#include "settings.h"
#include "hardware_recommendation.h"
#include "telemetry_upload.h"

namespace kk::telemetry {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr int kQuitTimeoutMs = 1500;    // each quit request; quitting waits at most kQuitWait in all
constexpr auto kQuitWait = std::chrono::milliseconds(1800);
constexpr int kStartTimeoutMs = 10000;  // the start's uploads (nobody waits for them)

std::mutex g_mutex;  // the paths, the chapters and the renderer's last figures
fs::path g_user_root, g_logs_dir, g_cache_root;
bool g_disabled = false;  // benchmark processes send nothing (their user data is a private copy)
std::atomic<bool> g_game_running{false};
std::map<int, ChapterStats> g_chapters;
struct RendererSeen {
  int api = 0;  // 1 Vulkan, 2 Direct3D 12
  uint32_t vendor = 0, in_w = 0, in_h = 0, out_w = 0, out_h = 0;
  std::string upscaler;
} g_renderer;

// The game thread's own (NoteFrame).
std::atomic<int> g_entry{-1}, g_skip{0};
ChapterStats g_current;
Clock::time_point g_last_sample{};
uint64_t g_frames_since_sample = 0;
double g_cpu_base = -1, g_pipe_base = -1;
bool g_cpu_valid = false;
int g_samples = 0;

// The quit's uploads.
std::mutex g_quit_mutex;
std::condition_variable g_quit_cv;
int g_quit_running = 0;
bool g_quit_started = false;
Clock::time_point g_quit_begin{};
std::string g_quit_status;  // telemetry/last-quit.txt, logged at the next start

std::string Cvar(const char* name) { return rex::cvar::GetFlagByName(name); }
bool CvarOn(const char* name) {
  const std::string v = Cvar(name);
  return v == "true" || v == "1";
}

std::string Url() {
#if defined(KK_DEV_TOOLS)
  if (const char* v = std::getenv("KK_DEV_TELEMETRY_URL"); v && *v) return v;
#endif
  return online::ServerUrl();  // KK_DEV_ONLINE_URL in developer builds, else the port's server
}

double DevNowSeconds() {
#if defined(KK_DEV_TOOLS)
  if (const char* v = std::getenv("KK_DEV_TELEMETRY_NOW"); v && *v) return std::atof(v);
#endif
  return 0;
}

double ThreadCpuMs() {
#if defined(_WIN32)
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return -1;
  const auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return double(ticks(kernel) + ticks(user)) / 10000.0;
#else
  timespec t{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) return -1;
  return double(t.tv_sec) * 1000.0 + double(t.tv_nsec) / 1e6;
#endif
}

// The native renderer's frame figures (the benchmark's source): GPU frame time and the total pipeline wait.
bool ReadNative(NrBenchmarkStats& s) {
#if defined(_WIN32)
  using Read = uint32_t (*)(NrBenchmarkStats*);
  using Enable = void (*)(uint32_t);
  static Read read = [] {
    const HMODULE module = GetModuleHandleW(L"rexgpu-native.dll");
    if (!module) return Read(nullptr);
    if (const auto enable = reinterpret_cast<Enable>(GetProcAddress(module, "nr_benchmark_enable"))) enable(1);
    return reinterpret_cast<Read>(GetProcAddress(module, "nr_benchmark_stats"));
  }();
  s.size = sizeof(NrBenchmarkStats);
  return read && read(&s);
#else
  (void)s;
  return false;
#endif
}

void Commit() {  // game thread
  const int entry = g_current.entry;
  if (entry >= 0 && (g_current.frames || g_current.pipeline_wait_ms > 0)) {
    std::lock_guard lock(g_mutex);
    ChapterStats& c = g_chapters[entry];
    c.entry = entry;
    c.Merge(g_current);
  }
  g_current = ChapterStats{};
  g_current.entry = entry;
}

void Sample(bool attribute) {  // game thread, once a second
  const double cpu = ThreadCpuMs();
  NrBenchmarkStats native;
  const bool have = ReadNative(native);
  if (attribute) {
    if (g_cpu_valid && cpu >= g_cpu_base && g_frames_since_sample) {
      g_current.game_cpu_ms += cpu - g_cpu_base;
      g_current.game_cpu_frames += g_frames_since_sample;
    }
    if (have && g_pipe_base >= 0 && native.pipeline_total_ms >= g_pipe_base)
      g_current.pipeline_wait_ms += native.pipeline_total_ms - g_pipe_base;
    if (have && native.gpu_ms > 0 && native.gpu_ms < 1000) {
      g_current.gpu_ms_sum += native.gpu_ms;
      ++g_current.gpu_samples;
    }
  }
  g_cpu_base = cpu;
  g_cpu_valid = cpu >= 0;
  g_pipe_base = have ? native.pipeline_total_ms : -1;
  g_frames_since_sample = 0;
  if (g_samples++ % 10 == 0) {
    const NrEffectStats e = nr::kk::EffectTimes();
    if (e.size) {
      std::lock_guard lock(g_mutex);
      g_renderer.api = int(e.api);
      g_renderer.vendor = e.vendor;
      g_renderer.in_w = e.input_width, g_renderer.in_h = e.input_height;
      g_renderer.out_w = e.output_width, g_renderer.out_h = e.output_height;
      g_renderer.upscaler.assign(e.upscaler, strnlen(e.upscaler, sizeof(e.upscaler)));
    }
  }
  Commit();
}

std::string Api() {
  {
    std::lock_guard lock(g_mutex);
    if (g_renderer.api == 1) return "vulkan";
    if (g_renderer.api == 2) return "d3d12";
  }
  const std::string backend = Cvar("native_backend");
  if (backend == "d3d12" || backend == "vulkan") return backend;
#if defined(_WIN32)
  if (Cvar("native_failed_backends").find("vulkan") != std::string::npos) return "d3d12";
#endif
  return "vulkan";  // Auto uses Vulkan
}

// "windows"; "linux" on Linux and Proton (Wine).
std::string Os() {
#if defined(_WIN32)
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll && GetProcAddress(ntdll, "wine_get_version") ? "linux" : "windows";
#else
  return "linux";
#endif
}

std::string CpuName() {
  unsigned regs[4] = {};
  char brand[49] = {};
#if defined(_WIN32)
  int r[4];
  __cpuid(r, int(0x80000000));
  if (unsigned(r[0]) >= 0x80000004)
    for (int i = 0; i < 3; ++i) {
      __cpuid(r, int(0x80000002) + i);
      std::memcpy(brand + i * 16, r, 16);
    }
  (void)regs;
#else
  if (__get_cpuid(0x80000000, &regs[0], &regs[1], &regs[2], &regs[3]) && regs[0] >= 0x80000004)
    for (unsigned i = 0; i < 3; ++i) {
      __get_cpuid(0x80000002 + i, &regs[0], &regs[1], &regs[2], &regs[3]);
      std::memcpy(brand + i * 16, regs, 16);
    }
#endif
  std::string s = brand;
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

struct Hardware {
  std::string gpu, vendor, driver, cpu, display;
  int64_t vram_mib = -1, ram_mib = -1;
  int threads = -1;
};
// Once, on a worker (DXGI takes a moment).
const Hardware& GetHardware() {
  static std::once_flag once;
  static Hardware h;
  std::call_once(once, [] {
    const HardwareProfile p = DetectHardware();
    h.gpu = p.gpu.name;
    h.vendor = p.gpu.vendor == GpuVendor::kNvidia ? "nvidia"
               : p.gpu.vendor == GpuVendor::kAmd  ? "amd"
               : p.gpu.vendor == GpuVendor::kIntel ? "intel"
                                                   : "";
    h.driver = p.gpu.driver;
    if (p.gpu.dedicated_memory) h.vram_mib = int64_t(p.gpu.dedicated_memory / (1024 * 1024));
    if (p.system_memory) h.ram_mib = int64_t(p.system_memory / (1024 * 1024));
    if (p.width > 0 && p.height > 0)
      h.display = std::to_string(p.width) + "x" + std::to_string(p.height) +
                  (p.refresh_hz > 0 ? "@" + std::to_string(p.refresh_hz) : "");
    h.cpu = CpuName();
    h.threads = int(std::thread::hardware_concurrency());
  });
  return h;
}

Private PersonalNames() {
  Private p;
  for (const char* key : {"USERNAME", "USER", "LOGNAME", "COMPUTERNAME"})
    if (const char* v = std::getenv(key); v && *v) p.names.emplace_back(v);
  for (const char* key : {"USERPROFILE", "HOME"})
    if (const char* v = std::getenv(key); v && *v) p.homes.emplace_back(v);
  return p;
}

// The newest record file in the renderer's pipeline folder (plugin_settings.cpp PipelinePlaceUnder).
fs::path PipelinesFile() {
  fs::path folder = Cvar("native_pipeline_cache_dir");
  if (folder.empty()) {
    const std::string root = Cvar("cache_root");  // as the renderer's cache root (OnPreSetup)
    std::lock_guard lock(g_mutex);
    folder = (root.empty() ? g_cache_root : fs::path(root)) / "native-pipelines";
  }
  fs::path best;
  fs::file_time_type best_time{};
  std::error_code ec;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".pipelines") continue;
    std::error_code tec;
    const auto t = fs::last_write_time(it->path(), tec);
    if (!tec && (best.empty() || t > best_time)) best = it->path(), best_time = t;
  }
  return best;
}

Context MakeContext(int timeout_ms) {
  Context c;
  c.switch_on = REXCVAR_GET(kk_online_count);
  if (!c.switch_on) return c;
  c.install_id = online::InstallId();
  c.version = KK_VERSION;
  c.api = Api();
  {
    std::lock_guard lock(g_mutex);
    c.state_dir = g_user_root / "telemetry";
    c.report_dirs = {g_user_root / "crashes", g_logs_dir};
  }
  c.personal = PersonalNames();
  c.pipelines_file = PipelinesFile();
  c.force = DevNowSeconds() > 0;
  const std::string base = Url();
  const fs::path scratch = c.state_dir / "upload.tmp";
  c.send = [base, timeout_ms, scratch](const Request& r) {
    std::vector<uint8_t> reply;
    // Each request its own scratch file (Linux curl), as the quit's two may run at once.
    const std::string file = scratch.string() + "." + r.path.substr(r.path.rfind('/') + 1);
    return HttpPostBody(base + r.path, r.content_type, r.headers, r.body, timeout_ms, reply, file);
  };
  return c;
}

// A setting's value as JSON: null when it is not set.
std::string Quoted(const std::string& s) { return s.empty() ? "null" : JsonString(s, 64); }

std::string BuildPerfJson(const Context& c) {
  PerfSummary s;
  s.install_id = c.install_id;
  s.version = c.version;
  s.api = c.api;
  const Hardware& h = GetHardware();
  s.gpu = h.gpu;
  s.driver = h.driver;
  s.vram_mib = h.vram_mib;
  s.cpu = h.cpu;
  s.cpu_threads = h.threads;
  s.ram_mib = h.ram_mib;
  s.os = Os();
  s.display = h.display;
  RendererSeen r;
  {
    std::lock_guard lock(g_mutex);
    r = g_renderer;
    for (const auto& [entry, stats] : g_chapters) s.chapters.push_back(stats);
  }
  s.gpu_vendor = h.vendor;
  if (s.gpu_vendor.empty())
    s.gpu_vendor = r.vendor == 0x10DE ? "nvidia" : r.vendor == 0x1002 ? "amd" : r.vendor == 0x8086 ? "intel" : "other";
  const auto size = [](uint32_t w, uint32_t hgt) {
    return w && hgt ? Quoted(std::to_string(w) + "x" + std::to_string(hgt)) : std::string("null");
  };
  const auto boolean = [](bool b) { return std::string(b ? "true" : "false"); };
  s.settings = {
      {"preset", Quoted(Cvar("kk_graphics_preset"))},
      {"upscaler", Quoted(Cvar("native_upscaler"))},
      {"quality", Quoted(Cvar("native_upscaler_quality"))},
      {"upscalerActive", Quoted(r.upscaler)},
      {"frameCap", std::to_string(std::atoi(Cvar("kk_frame_rate").c_str()))},
      {"gi", Quoted(Cvar("native_gi"))},
      {"ao", Quoted(Cvar("native_ao_method"))},
      {"shadows", Quoted(Cvar("native_shadow_quality"))},
      {"water", Quoted(Cvar("native_water_reflections"))},
      {"shafts", Quoted(Cvar("native_volumetrics"))},
      {"texturePack", boolean(CvarOn("native_texture_pack"))},
      {"originalLook", boolean(CvarOn("kk_original_look"))},
      {"renderSize", size(r.in_w, r.in_h)},
      {"outputSize", size(r.out_w, r.out_h)},
  };
  s.pipeline_library = c.api == "vulkan" && CvarOn("native_vk_pipeline_library");
  s.startup_fallback = !Cvar("native_failed_backends").empty();
  return PerfJson(s);
}

void WriteStatus(const std::string& line) {
  std::lock_guard lock(g_quit_mutex);
  g_quit_status += line + "\n";
  fs::path dir;
  {
    std::lock_guard paths(g_mutex);
    dir = g_user_root / "telemetry";
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::ofstream(dir / "last-quit.txt", std::ios::trunc) << g_quit_status;
}

// The quit's two uploads, each on its own thread; `done` is called by each when it ends.
void RunQuitUploads(const std::function<void()>& done) {
  Context c = MakeContext(kQuitTimeoutMs);
  if (!Allowed(c)) {
    done();
    done();
    return;
  }
  std::thread([c, done] {
    const std::string json = BuildPerfJson(c);
    const int status = json.empty() ? -1 : SendPerf(c, json);
    WriteStatus("perf " + std::to_string(status) + " (" + std::to_string(json.size()) + " bytes)");
    done();
  }).detach();
  std::thread([c, done] {
    const int status = SendPipelines(c);
    WriteStatus("pipelines " + std::to_string(status) + " (" + c.pipelines_file.filename().string() + ")");
    done();
  }).detach();
}

}  // namespace

void Configure(const fs::path& user_root, const fs::path& logs_dir, const fs::path& cache_root) {
  std::lock_guard lock(g_mutex);
  g_user_root = user_root;
  g_logs_dir = logs_dir;
  g_cache_root = cache_root.empty() ? user_root / "cache" : cache_root;
  g_disabled = benchmark::Requested() || benchmark::Active();
}

void StartupUploads() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (g_disabled) return;
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      fs::path dir;
      {
        std::lock_guard lock(g_mutex);
        dir = g_user_root / "telemetry";
      }
      // What the last quit managed (written then; logging was not allowed at that point).
      std::error_code ec;
      if (std::ifstream f(dir / "last-quit.txt"); f) {
        std::string line, all;
        while (std::getline(f, line)) all += (all.empty() ? "" : ", ") + line;
        f.close();
        REXLOG_INFO("KK uploads: last quit: {}", all);
        fs::rename(dir / "last-quit.txt", dir / "last-quit-logged.txt", ec);
      }
      Context c = MakeContext(kStartTimeoutMs);
      if (!Allowed(c)) {
        REXLOG_INFO("KK uploads: Online player count is off; nothing is sent");
        return;
      }
      (void)GetHardware();  // ready for the quit
      const int perf = SendPendingPerf(c);
      const auto pending = PendingReports(c);
      const int reports = SendCrashReports(c, 1);
      REXLOG_INFO("KK uploads: at start: earlier summary {}, crash/hang reports waiting {}, sent {}", perf,
                  pending.size(), reports);
    }).detach();
  });
}

void SetGameRunning() {
  if (g_disabled || g_game_running.exchange(true)) return;
  if (const double seconds = DevNowSeconds(); seconds > 0) {
    // Developer aid: the quit's uploads now, without quitting.
    RunAfterFirstFrame(seconds, [] {
      REXLOG_INFO("KK uploads: KK_DEV_TELEMETRY_NOW: sending the quit uploads now");
      std::thread([] {
        struct Wait {
          std::mutex m;
          std::condition_variable cv;
          int left = 2;
        };
        const auto w = std::make_shared<Wait>();
        RunQuitUploads([w] {
          std::lock_guard lock(w->m);
          --w->left;
          w->cv.notify_all();
        });
        std::unique_lock lock(w->m);
        w->cv.wait_for(lock, std::chrono::seconds(30), [&] { return w->left == 0; });
        REXLOG_INFO("KK uploads: KK_DEV_TELEMETRY_NOW done: {}", [] {
          std::lock_guard l(g_quit_mutex);
          return g_quit_status;
        }());
      }).detach();
    });
  }
}

void NoteFrame(double frame_ms) {
  if (g_disabled) return;
  const int entry = g_entry.load(std::memory_order_relaxed);
  if (entry != g_current.entry) {
    Commit();
    g_current.entry = entry;
    g_cpu_valid = false;
  }
  const auto now = Clock::now();
  bool attribute = entry >= 0;
  if (g_skip.load(std::memory_order_relaxed) > 0) {
    g_skip.fetch_sub(1, std::memory_order_relaxed);
    g_cpu_valid = false;  // the load's CPU time is not a frame's
    attribute = false;
  } else if (entry >= 0) {
    g_current.AddFrame(frame_ms);
    ++g_frames_since_sample;
  }
  if (now - g_last_sample >= std::chrono::seconds(1)) {
    g_last_sample = now;
    Sample(attribute);
  }
}

void NoteChapter(int entry, bool loading) {
  g_entry.store(entry, std::memory_order_relaxed);
  if (loading) g_skip.store(2, std::memory_order_relaxed);
}

void BeginQuitUploads() {
  {
    std::lock_guard lock(g_quit_mutex);
    if (g_quit_started) return;
    g_quit_started = true;
    if (g_disabled || !g_game_running.load()) return;
    g_quit_begin = Clock::now();
    g_quit_running = 2;
    g_quit_status.clear();
  }
  RunQuitUploads([] {
    std::lock_guard lock(g_quit_mutex);
    --g_quit_running;
    g_quit_cv.notify_all();
  });
}

void WaitQuitUploads() {
  std::unique_lock lock(g_quit_mutex);
  if (!g_quit_started || g_quit_running == 0) return;
  g_quit_cv.wait_until(lock, g_quit_begin + kQuitWait, [] { return g_quit_running == 0; });
}

}  // namespace kk::telemetry
