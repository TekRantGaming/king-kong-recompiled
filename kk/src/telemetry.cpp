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
#include "content.h"
#include "crash_dump.h"
#include "frame_stats.h"
#include "guest.h"
#include "hooks/kk_native_ui.h"
#include "http.h"
#include "io_stats.h"
#include "online.h"
#include "platform.h"
#include "play_session.h"
#include "photo_mode.h"
#include "ps2_movies.h"
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
  // Task 270: the session's highest video memory use and the budget then (MiB), and whether texture pack
  // replacements were loaded (-1: the renderer's figures are not known).
  int64_t vram_peak_mib = -1, vram_peak_budget_mib = -1;
  int pack_used = -1;
} g_renderer;

// The game thread's own (NoteFrame).
std::atomic<int> g_entry{-1}, g_skip{0};
ChapterStats g_current;
Clock::time_point g_last_sample{};
uint64_t g_frames_since_sample = 0;
double g_cpu_base = -1, g_pipe_base = -1;
bool g_cpu_valid = false;
int g_samples = 0;

// Task 211, the worst-frame detail (game thread): running totals as the previous frame ended, and a slow
// frame waiting for the renderer's totals to catch up.
struct Snapshot {
  double cpu_ms = -1;
  bool native = false;
  NrBenchmarkStats n;
  iostats::Counters io;
  double sleep_bias_ms = -1;
};
Snapshot g_prev;
bool g_prev_valid = false;
struct Pending {
  bool active = false;
  bool hitch = false;  // task 270: a 50-250 ms frame, only counted by cause
  int frames_left = 0;
  WorstFrame f;
  Snapshot start;
} g_pending;
constexpr int kPendingFrames = 2;  // the renderer runs up to 2 frames behind the game
Clock::time_point g_visit_start{};
int g_focus_sampled = -1;  // FocusState() at the last once-a-second sample
int64_t g_vram_peak_seen = -1;  // task 270: g_renderer.vram_peak_mib, without the lock
#if defined(_WIN32)
HWND g_own_window = nullptr;
#endif
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

#if defined(_WIN32)
// GetThreadTimes: in clock ticks (15.6 ms).
double CoarseThreadCpuMs() {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return -1;
  const auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return double(ticks(kernel) + ticks(user)) / 10000.0;
}

double QpcMs() {
  static const double per_ms = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart) / 1000.0;
  }();
  LARGE_INTEGER t{};
  QueryPerformanceCounter(&t);
  return per_ms > 0 ? double(t.QuadPart) / per_ms : -1;
}

// Task 270: the processor's cycle rate (cycles per ms), measured against QPC from the first call on: 0 for
// the first second, then fixed; -1 when it makes no sense.
double CyclesPerMs() {
  static const uint64_t tsc0 = __rdtsc();
  static const double ms0 = QpcMs();
  static double rate = 0;
  if (rate == 0) rate = PreciseCpuClock::Rate(tsc0, ms0, __rdtsc(), QpcMs());
  return rate;
}

PreciseCpuClock g_cpu_clock;  // game thread

// The cycle-based thread time (ms): -1 while the rate is being measured or when the cycle count is not used.
double PreciseThreadCpuMs() {
  if (!g_cpu_clock.Trusted()) return -1;
  const double rate = CyclesPerMs();
  ULONG64 cycles = 0;
  if (rate < 0 || (rate > 0 && !QueryThreadCycleTime(GetCurrentThread(), &cycles))) {
    g_cpu_clock.Fail();
    REXLOG_INFO("KK uploads: thread cycle time unavailable; frame CPU times use the 15.6 ms system clock");
    return -1;
  }
  return rate > 0 ? double(cycles) / rate : -1;
}
#endif

// The game thread's CPU time (ms): at full resolution where the cycle count is trusted (task 270).
double ThreadCpuMs() {
#if defined(_WIN32)
  if (g_cpu_clock.Trusted()) {
    const double ms = PreciseThreadCpuMs();
    if (g_cpu_clock.Trusted()) return ms;
  }
  static bool coarse = false;
  if (!coarse) {  // the cycle count was just dropped: no difference across the change
    coarse = true;
    g_cpu_valid = false;
    g_prev.cpu_ms = -1;
    return -1;
  }
  return CoarseThreadCpuMs();
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
  if (entry >= 0 && (g_current.frames || g_current.pipeline_wait_ms > 0 || !g_current.worst.empty())) {
    std::lock_guard lock(g_mutex);
    ChapterStats& c = g_chapters[entry];
    c.entry = entry;
    c.Merge(g_current);
  }
  g_current = ChapterStats{};
  g_current.entry = entry;
}

// ---------------------------------------------------------------------- task 211: worst-frame detail ---

// The time the PC has spent asleep or hibernating since it started (ms): it only grows while it sleeps.
double SleepBiasMs() {
#if defined(_WIN32)
  ULONGLONG unbiased = 0;
  if (!QueryUnbiasedInterruptTime(&unbiased)) return -1;
  return double(GetTickCount64()) - double(unbiased) / 10000.0;
#else
  timespec boot{}, mono{};
  if (clock_gettime(CLOCK_BOOTTIME, &boot) != 0 || clock_gettime(CLOCK_MONOTONIC, &mono) != 0) return -1;
  return double(boot.tv_sec - mono.tv_sec) * 1000.0 + double(boot.tv_nsec - mono.tv_nsec) / 1e6;
#endif
}

// 0 the game's window is the foreground window, 1 another program's is, 2 the game's is minimised; -1 not known.
int FocusState() {
#if defined(_WIN32)
  const HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  if (fg) GetWindowThreadProcessId(fg, &pid);
  if (fg && pid == GetCurrentProcessId()) {
    g_own_window = fg;
    return IsIconic(fg) ? 2 : 0;
  }
  if (g_own_window && IsWindow(g_own_window) && IsIconic(g_own_window)) return 2;
  return 1;
#else
  return -1;
#endif
}

// The game's loading screen is up (loading_fix.cpp: its thread handle at +0, state at +12, 1 shown, 2 starting).
bool LoadingScreenUp() {
  const uint8_t* base = g_guest_base;
  if (!base) return false;
  constexpr uint32_t kLoadingScreen = 0x82D64080;
  const auto load32 = [base](uint32_t a) {
    const uint8_t* p = base + a;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  const uint32_t state = load32(kLoadingScreen + 12);
  return load32(kLoadingScreen) != 0 && (state == 1 || state == 2);
}

Snapshot TakeSnapshot() {  // every frame: a few reads, no allocation
  Snapshot s;
  s.cpu_ms = ThreadCpuMs();
  s.native = ReadNative(s.n);
  s.io = iostats::Take();
  s.sleep_bias_ms = SleepBiasMs();
  return s;
}

// A slow frame just ended: what the game's side knows now; the renderer's part follows in FinishPending.
void StartPending(double frame_ms, double swap_ms, double cap_ms, bool level_load, bool hitch,
                  const Snapshot& now, Clock::time_point t) {
  WorstFrame f;
  f.ms = frame_ms;
  if (now.native && now.n.memory_valid && now.n.vram_budget) {
    f.vram_mib = int64_t(now.n.vram_used >> 20);
    f.budget_mib = int64_t(now.n.vram_budget >> 20);
  }
  if (g_visit_start != Clock::time_point{}) f.at_s = std::chrono::duration<double>(t - g_visit_start).count();
  f.loading = level_load || LoadingScreenUp();
  if (swap_ms >= 0) f.swap_ms = std::min(swap_ms, frame_ms);
  if (cap_ms >= 0) f.cap_wait_ms = std::min(cap_ms, frame_ms);
  if (g_prev_valid) {
    if (g_prev.cpu_ms >= 0 && now.cpu_ms >= g_prev.cpu_ms) f.cpu_ms = now.cpu_ms - g_prev.cpu_ms;
    f.reads = uint32_t(now.io.reads - g_prev.io.reads);
    f.video_reads = uint32_t(now.io.video_reads - g_prev.io.video_reads);
    f.read_mb = double(now.io.bytes - g_prev.io.bytes) / (1024.0 * 1024.0);
    f.read_ms = double(now.io.us - g_prev.io.us) / 1000.0;
    f.read_max_ms = double(now.io.longest_us) / 1000.0;
    // The bias moves in clock ticks (about 16 ms): less than 100 ms is no sleep.
    if (g_prev.sleep_bias_ms >= 0 && now.sleep_bias_ms - g_prev.sleep_bias_ms >= 100)
      f.sleep_ms = now.sleep_bias_ms - g_prev.sleep_bias_ms;
  }
  f.focus = FocusState();
  f.focus_changed = f.focus >= 0 && g_focus_sampled >= 0 && f.focus != g_focus_sampled;
  f.paused = play::PauseMenuOpen() || photo::Active();
  {
    auto& service = content::Service::Get();
    f.download = service.Ready() && !service.Idle();
  }
  f.texture_pack = CvarOn("native_texture_pack");
  f.frame_cap = std::max(0, std::atoi(Cvar("kk_frame_rate").c_str()));
  f.vsync = CvarOn("vsync") ? 1 : 0;
  f.hfr = CvarOn("kk_hfr_fixes") && f.frame_cap != 30 ? 1 : 0;
  {
    std::lock_guard lock(g_mutex);
    f.upscaler = g_renderer.upscaler;
  }
  g_pending.active = true;
  g_pending.hitch = hitch;
  g_pending.frames_left = kPendingFrames;
  g_pending.f = std::move(f);
  g_pending.start = g_prev_valid ? g_prev : Snapshot{};
}

// The renderer's totals since the frame before the slow one; then the frame joins its chapter's list.
void FinishPending() {
  if (!g_pending.active) return;
  g_pending.active = false;
  WorstFrame& f = g_pending.f;
  const NrBenchmarkStats& a = g_pending.start.n;
  NrBenchmarkStats b;
  if (g_pending.start.native && ReadNative(b) && b.frame >= a.frame) {
    const auto du = [](uint64_t to, uint64_t from) { return to >= from ? to - from : 0; };
    const auto dd = [](double to, double from) { return to >= from ? to - from : 0.0; };
    constexpr double kMiB = 1024.0 * 1024.0;
    f.native = true;
    f.render_frames = uint32_t(du(b.frame, a.frame));
    f.render_cpu_ms = double(du(b.render_cpu_us, a.render_cpu_us)) / 1000.0;
    if (const uint64_t timed = du(b.gpu_frames, a.gpu_frames)) f.gpu_ms = dd(b.gpu_ms_total, a.gpu_ms_total) / double(timed);
    f.gpu_wait_ms = double(du(b.gpu_wait_us, a.gpu_wait_us)) / 1000.0;
    f.submit_ms = double(du(b.submit_us, a.submit_us)) / 1000.0;
    f.pipe_waits = uint32_t(du(b.pipeline_waits, a.pipeline_waits));
    f.pipe_wait_ms = dd(b.pipeline_stutter_ms, a.pipeline_stutter_ms);
    f.pipes_made = uint32_t(du(b.pipelines_made, a.pipelines_made));
    f.draws_skipped = uint32_t(du(b.draws_skipped, a.draws_skipped));
    f.compile_ms = dd(b.pipeline_compile_ms, a.pipeline_compile_ms);
    f.tex_uploads = uint32_t(du(b.texture_uploads, a.texture_uploads));
    f.upload_mb = double(du(b.upload_bytes, a.upload_bytes)) / kMiB;
    f.tex_ms = dd(b.texture_ms, a.texture_ms);
    f.prepare_ms = dd(b.prepare_ms, a.prepare_ms);
    f.pack_textures = uint32_t(du(b.pack_textures, a.pack_textures));
    f.pack_mb = double(du(b.pack_bytes, a.pack_bytes)) / kMiB;
    f.renderer_loading = a.resource_loading || b.resource_loading;
    f.worker_jobs = int(std::min<uint32_t>(std::max(a.worker_jobs, b.worker_jobs), 1u << 20));
  }
  // Task 270: the likely cause; a 50-250 ms frame is only counted by it.
  const Hitch cause = ClassifyHitch(f);
  f.cause = HitchName(cause);
  if (g_pending.hitch) g_current.AddHitch(cause);
  else AddWorstFrame(g_current.worst, f);
}

void Sample(bool attribute) {  // game thread, once a second
#if defined(_WIN32)
  // Task 270: the cycle-based CPU time checked against the system's (ThreadCpuMs drops it when they disagree).
  if (g_cpu_clock.Trusted()) {
    const double precise = PreciseThreadCpuMs();
    if (precise >= 0 && !g_cpu_clock.Check(precise, CoarseThreadCpuMs()))
      REXLOG_INFO("KK uploads: thread cycle time disagrees with the system clock; frame CPU times use the 15.6 ms clock");
  }
#endif
  const double cpu = ThreadCpuMs();
  NrBenchmarkStats native;
  const bool have = ReadNative(native);
  if (have) {
    std::lock_guard lock(g_mutex);
    g_renderer.pack_used = native.pack_textures > 0 ? 1 : 0;
  }
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
  g_focus_sampled = FocusState();
  if (g_samples++ % 10 == 0) {
    const NrEffectStats e = nr::kk::EffectTimes();
    if (e.size) {
      std::lock_guard lock(g_mutex);
      g_renderer.api = int(e.api);
      g_renderer.vendor = e.vendor;
      g_renderer.in_w = e.input_width, g_renderer.in_h = e.input_height;
      g_renderer.out_w = e.output_width, g_renderer.out_h = e.output_height;
      g_renderer.upscaler.assign(e.upscaler, strnlen(e.upscaler, sizeof(e.upscaler)));
      if (e.api == 1 || e.api == 2) kk::SetCrashReportApi(int(e.api));  // task 270
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
  s.vram_peak_mib = r.vram_peak_mib;  // task 270
  s.vram_peak_budget_mib = r.vram_peak_budget_mib;
  s.texture_pack_used = r.pack_used;
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

void NoteFrame(double frame_ms, double swap_ms, double cap_wait_ms) {
  if (g_disabled) return;
  const int entry = g_entry.load(std::memory_order_relaxed);
  // Task 217: a PS2 cutscene between chapters is not a frame (CutsceneFrames).
  static CutsceneFrames cutscenes;
  const bool cutscene = cutscenes.Spans(movies::LoaderWaitsEnded(), movies::LoaderWaiting());
  const bool slow = !cutscene && WorstFrameWanted(entry, frame_ms);
  // Task 270: a 50-250 ms gameplay frame is measured the same way, then only counted by cause.
  const bool hitch_frame =
      !cutscene && HitchWanted(entry, frame_ms) && g_skip.load(std::memory_order_relaxed) <= 0;
  // Task 211: a slow frame's detail is finished kPendingFrames later, or sooner when the chapter changes or
  // another slow frame comes.
  if (g_pending.active && (entry != g_current.entry || slow || hitch_frame || --g_pending.frames_left <= 0))
    FinishPending();
  const auto now = Clock::now();
  if (entry != g_current.entry) {
    Commit();
    g_current.entry = entry;
    g_cpu_valid = false;
    g_visit_start = now;
  }
  bool attribute = entry >= 0;
  bool level_load = false, counted = false;
  if (g_skip.load(std::memory_order_relaxed) > 0) {
    g_skip.fetch_sub(1, std::memory_order_relaxed);
    g_cpu_valid = false;  // the load's CPU time is not a frame's
    attribute = false;
    level_load = true;
  } else if (cutscene) {
    g_cpu_valid = false;
    attribute = false;
  } else if (entry >= 0) {
    g_current.AddFrame(frame_ms);
    ++g_frames_since_sample;
    counted = true;
  }
  // Task 211: the running totals every frame (cheap reads); the detail only for a slow frame.
  const Snapshot snap = TakeSnapshot();
  // Task 270: video memory (the renderer refreshes it once a second): the session's peak, the chapter's
  // peak and the value at the chapter's worst frame.
  const bool vram = snap.native && snap.n.memory_valid && snap.n.vram_budget;
  const int64_t vram_mib = vram ? int64_t(snap.n.vram_used >> 20) : -1;
  const int64_t budget_mib = vram ? int64_t(snap.n.vram_budget >> 20) : -1;
  if (vram && vram_mib > g_vram_peak_seen) {
    g_vram_peak_seen = vram_mib;
    std::lock_guard lock(g_mutex);
    g_renderer.vram_peak_mib = vram_mib;
    g_renderer.vram_peak_budget_mib = budget_mib;
  }
  if (counted) g_current.AddVram(vram_mib, budget_mib, frame_ms >= g_current.worst_ms);
  if (slow || (hitch_frame && counted))
    StartPending(frame_ms, swap_ms, cap_wait_ms, level_load, !slow, snap, now);
  g_prev = snap;
  g_prev_valid = true;
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
