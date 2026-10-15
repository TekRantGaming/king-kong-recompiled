#include "benchmark.h"
#include "benchmark_data.h"
#include "benchmark_writer.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <stdexcept>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "diagnostics.h"
#include "debug_overlay.h"
#include "launcher_text.h"
#include "play_session.h"
#include "ui_paint.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(kk_benchmark, false, "KK", "Run the built-in benchmark once (not saved)");

namespace kk::benchmark {
namespace {
using Clock = std::chrono::steady_clock;
enum class Phase { kOff, kLoading, kWarmup, kRunning, kSaving, kComplete };
std::atomic<Phase> phase{Phase::kOff};
std::atomic<double> game_seconds{0};
std::atomic<bool> interrupted{false};
std::mutex mutex;
std::filesystem::path result_dir;
std::vector<double> guest_times, host_times;
Clock::time_point last_guest{}, last_host{};
std::string report, settings, save_error;
Stats guest_stats, host_stats;
std::vector<float> graph;
bool observer_available = false;
ResultWriter writer;

std::string Settings() {
  std::string out = "Version: " KK_VERSION "\nRoute: " + std::string(kRoute) +
      "\nChapter entry: 8\nWarmup simulation seconds: 10\nReplay simulation seconds: 75\n";
  out += DiagnosticsHardware();
  out += "\nSettings (requested; renderer fallbacks are recorded in diagnostics):\n";
  for (const char* key : {"native_backend", "kk_frame_rate", "fullscreen", "kk_fullscreen_mode",
      "window_width", "window_height", "video_mode_width", "video_mode_height", "monitor",
      "d3d12_allow_variable_refresh_rate_and_tearing", "vulkan_allow_present_mode_immediate",
      "vulkan_allow_present_mode_mailbox", "vulkan_allow_present_mode_fifo_relaxed", "ui_paint_on_demand",
      "kk_original_look", "kk_render_quality", "native_render_scale", "resolution_scale",
      "draw_resolution_scale_x", "draw_resolution_scale_y", "native_upscaler", "native_upscaler_quality",
      "native_ao_method", "native_ao_quality", "native_ao_samples", "ao_mode", "ao_strength", "native_gi",
      "native_shadow_quality", "native_water_reflections", "native_volumetrics", "kk_fov",
      "kk_camera_sensitivity", "kk_camera_modern", "kk_hfr_fixes", "kk_cheats", "present_effect"})
    out += std::string(key) + " = " + rex::cvar::GetFlagByName(key) + '\n';
  return RedactDiagnostics(std::move(out));
}

void Finish() {
  // Called by the game thread after the last measured swap. IO and sorting are
  // outside the measured interval and off the game/presenter threads.
  writer.Start([] {
    try {
      std::vector<double> guest, host;
      {
        std::lock_guard lock(mutex);
        guest.swap(guest_times);
        host.swap(host_times);
      }
      const Stats gs = Summarize(guest), hs = Summarize(host);
      std::string text = "King Kong benchmark v1\n";
      text += interrupted ? "Status: interrupted (do not compare)\n" : "Status: complete\n";
      text += settings;
      text += "\nGame swap intervals (includes cap and stalls):\n" + Describe(gs);
      text += observer_available ? "\nSuccessful host present return intervals (not scanout):\n" + Describe(hs)
                                : "\nHost present timing: unavailable (SDK patch 0041 required)\n";
      text += "\n1% low = reciprocal of the mean of the slowest ceil(N/100) intervals.\n"
              "Graph = peak frame time per elapsed-time bucket. CSV contains every interval.\n"
              "Compare the same route, settings, cap, API, cache and power state. AI can differ.\n";
      text = RedactDiagnostics(std::move(text));
      const std::string csv = "series,frame,elapsed_seconds,frame_ms\n" + Csv(guest, "game") + Csv(host, "host");
      const bool txt_ok = WriteResult(result_dir / "results.txt", text);
      const bool csv_ok = WriteResult(result_dir / "frames.csv", csv);
      {
        std::lock_guard lock(mutex);
        report = std::move(text);
        guest_stats = gs;
        host_stats = hs;
        graph = Graph(guest);
        if (!txt_ok || !csv_ok) save_error = "Could not save benchmark results.";
      }
      REXLOG_INFO("KK benchmark: complete, average {:.3f}, 1% low {:.3f}, worst {:.3f} ms, saved {}",
                  gs.average_fps, gs.low_fps, gs.worst_ms, txt_ok && csv_ok);
    } catch (const std::exception& error) {
      REXLOG_ERROR("KK benchmark: saving results failed: {}", error.what());
      std::lock_guard lock(mutex);
      save_error = "Could not save benchmark results.";
    }
    phase = Phase::kComplete;
    RequestUiPaint();
  }, [] { phase = Phase::kSaving; });
  RequestUiPaint();
}

void HostPresent() {
  if (phase != Phase::kRunning) return;
  const auto now = Clock::now();
  std::lock_guard lock(mutex);
  if (phase != Phase::kRunning) return;
  if (last_host != Clock::time_point{} && host_times.size() < kMaxSamples)
    host_times.push_back(std::chrono::duration<double, std::milli>(now - last_host).count());
  else if (host_times.size() >= kMaxSamples) interrupted = true;
  last_host = now;
}

class Dialog final : public rex::ui::ImGuiDialog {
 public:
  Dialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> quit)
      : ImGuiDialog(drawer), quit_(std::move(quit)) {}
  bool NeedsContinuousPaint() override {
    return phase == Phase::kSaving || phase == Phase::kComplete;
  }
 protected:
  void OnDraw(ImGuiIO& io) override {
    using text::T;
    const auto p = phase.load();
    if (p == Phase::kOff) return;
    // Settings were captured when the level loaded, before warmup. Opening
    // either settings UI during warmup invalidates that snapshot too.
    if ((p == Phase::kWarmup || p == Phase::kRunning) &&
        (DebugOverlayOpen() || play::PauseMenuOpen())) Interrupted();
    if (p == Phase::kLoading || p == Phase::kWarmup || p == Phase::kRunning) {
      ImGui::SetNextWindowPos(ImVec2(12, 50), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(.8f);
      ImGui::Begin("##benchmark-progress", nullptr, ImGuiWindowFlags_NoDecoration |
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
      ImGui::TextUnformatted(T("Benchmark"));
      if (p == Phase::kLoading) ImGui::TextUnformatted(T("Loading benchmark..."));
      else if (p == Phase::kWarmup) ImGui::TextUnformatted(T("Warming up..."));
      else ImGui::Text("%s %.0f / %.0f s", T("Running benchmark..."),
                       game_seconds.load() - kWarmupSeconds, kRunSeconds);
      ImGui::End();
      return;
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.f, io.DisplaySize.x - 24),
                                   std::min(600.f, io.DisplaySize.y - 24)), ImGuiCond_Always);
    ImGui::Begin(T("Benchmark results"), nullptr, ImGuiWindowFlags_NoCollapse);
    if (p == Phase::kSaving) {
      ImGui::TextUnformatted(T("Saving results..."));
    } else {
      std::lock_guard lock(mutex);
      if (interrupted) ImGui::TextWrapped("%s", T("Benchmark interrupted. Run it again before comparing results."));
      ImGui::Text("%s: %.1f", T("Average FPS"), guest_stats.average_fps);
      ImGui::Text("%s: %.1f", T("1% low FPS"), guest_stats.low_fps);
      ImGui::Text("%s: %.2f ms", T("Worst frame"), guest_stats.worst_ms);
      ImGui::TextUnformatted(T("Frame time (ms)"));
      if (!graph.empty()) ImGui::PlotLines("##frame-times", graph.data(), int(graph.size()), 0, nullptr,
          0, std::max(40.f, float(guest_stats.worst_ms)), ImVec2(-1, 100));
      ImGui::TextWrapped("%s", T("Results are saved in your user data folder under benchmarks. Nothing is uploaded."));
      ImGui::TextWrapped("%s", result_dir.string().c_str());
      if (!save_error.empty()) ImGui::TextWrapped("%s", T(save_error).c_str());
      if (ImGui::Button(T("Copy results"))) ImGui::SetClipboardText(report.c_str());
      ImGui::SameLine();
      if (ImGui::Button(T("Quit game")) && quit_) quit_();
      ImGui::Separator();
      ImGui::TextUnformatted(T("Settings and hardware"));
      ImGui::TextWrapped("%s", report.c_str());
    }
    ImGui::End();
  }
 private:
  std::function<void()> quit_;
};
}  // namespace

bool Requested() { return REXCVAR_GET(kk_benchmark); }
bool Active() { return phase != Phase::kOff; }
void Shutdown() { writer.Shutdown(); }

std::filesystem::path Prepare(const std::filesystem::path& root) {
  // Unique directory, never an existing player save. Failure aborts startup,
  // rather than falling through to a benchmark that writes into normal saves.
  const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  result_dir = root / "benchmarks" / ("run-" + std::to_string(stamp));
  std::filesystem::create_directories(result_dir);
  const auto private_root = result_dir / "user";
  if (!std::filesystem::create_directory(private_root))
    throw std::runtime_error("Could not create private benchmark user data");
  phase = Phase::kLoading;
  rex::cvar::ResetToDefault("kk_benchmark");
  play::SetStartRequest({play::StartKind::kChapter, 0, kChapter, "Benchmark"});
  guest_times.reserve(65536);
  host_times.reserve(65536);
  return private_root;
}

void InstallPresentObserver() {
  if (!Active()) return;
#if defined(_WIN32)
  using Setter = void (*)(void (*)());
  const auto runtime = GetModuleHandleW(L"rexruntime.dll");
  const auto setter = runtime ? reinterpret_cast<Setter>(GetProcAddress(runtime, "RexSetPresentObserver096")) : nullptr;
  if (setter) { setter(HostPresent); observer_available = true; }
#endif
  REXLOG_INFO("KK benchmark: host present observer {}", observer_available ? "available" : "unavailable");
}

void LevelLoaded() {
  if (phase != Phase::kLoading) { if (Active()) Interrupted(); return; }
  settings = Settings();
  game_seconds = 0;
  phase = Phase::kWarmup;
  REXLOG_INFO("KK benchmark: level loaded, 10 s warmup then 75 s replay ({})", kRoute);
}

void Frame(double dt) {
  const auto p = phase.load();
  if (p != Phase::kWarmup && p != Phase::kRunning) return;
  if (interrupted) { Finish(); return; }
  if (std::isfinite(dt) && dt > 0) game_seconds = game_seconds.load() + dt;
  if (p == Phase::kWarmup && game_seconds >= kWarmupSeconds) {
    last_guest = last_host = {};
    phase = Phase::kRunning;
    REXLOG_INFO("KK benchmark: measurement started");
  }
}

void GuestSwap() {
  if (phase != Phase::kRunning) return;
  const auto now = Clock::now();
  {
    std::lock_guard lock(mutex);
    if (last_guest != Clock::time_point{} && guest_times.size() < kMaxSamples)
      guest_times.push_back(std::chrono::duration<double, std::milli>(now - last_guest).count());
    else if (guest_times.size() >= kMaxSamples) interrupted = true;
    last_guest = now;
  }
  if (game_seconds >= kWarmupSeconds + kRunSeconds || interrupted) Finish();
}

bool Pad(uint32_t user, uint8_t* state) {
  if (!Active()) return false;
  std::memset(state, 0, 16);
  if (user != 0) return true;
  const double seconds = game_seconds.load();
  const uint32_t packet = std::byteswap(uint32_t(seconds * 1000));
  std::memcpy(state, &packet, sizeof(packet));
  if (phase == Phase::kRunning) {
    const uint16_t x = std::byteswap(uint16_t(int16_t(CameraX(seconds - kWarmupSeconds))));
    std::memcpy(state + 12, &x, sizeof(x));
  }
  return true;
}

void Interrupted() {
  const auto p = phase.load();
  if (p == Phase::kRunning || p == Phase::kWarmup || p == Phase::kLoading) interrupted = true;
}

void CreateBenchmarkDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> quit) {
  new Dialog(drawer, std::move(quit));
}
}  // namespace kk::benchmark
