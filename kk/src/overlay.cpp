#include "ui_paint.h"
#include "handoff.h"

#include "overlay.h"
#include "freecam.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/logging.h>
#if defined(KK_DEV_TOOLS)
namespace kk { double DevSecondsSinceMenu(); }
#endif

#include "benchmark.h"
#include "benchmark_monitor.h"
#include "frame_stats.h"
#include "launcher_text.h"
#include "perf_overlay_logic.h"
#include "platform.h"
#include "play_internal.h"
#include "settings.h"
#include "hooks/kk_native_ui.h"  // the native renderer's pipeline stutter line and effect readout
#include "plugin/native_benchmark_stats.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kk {
namespace {
std::mutex paint_mutex;
std::function<void()> paint_request;
}
void SetUiPaintRequest(std::function<void()> request) {
  std::lock_guard lock(paint_mutex);
  paint_request = std::move(request);
}
void RequestUiPaint() {
  std::lock_guard lock(paint_mutex);
  if (paint_request) paint_request();
}
namespace {


// ---------------------------------------------------------------- performance metrics
// The overlay's live readings (Full only): one worker reads the system counters the benchmark uses
// (benchmark_monitor.h, SystemSampler) and the native renderer's GPU time and memory twice a second while
// Full is shown, and stops when it is not. Drawing only copies the last reading.
struct MetricsView {
  benchmark::SystemSample sys;
  double vram = -1, budget = -1, gpu_ms = -1;  // MiB, MiB, ms (the native renderer)
  uint32_t api = 0, vendor = 0, in_w = 0, in_h = 0, out_w = 0, out_h = 0;
  std::string upscaler;
  double sample_ms = 0;  // the last reading's own time on the worker
  uint64_t samples = 0;
};

class MetricsSampler {
 public:
  // Called as the overlay draws: starts the worker when Full is on (it stops by itself when Full is not).
  void Want(bool on) {
    if (!on || running_.load()) return;
    if (worker_.joinable()) worker_.join();
    running_ = true;
    worker_ = std::thread([this] { Run(); });
  }
  MetricsView Get() const {
    std::lock_guard lock(mutex_);
    return view_;
  }

 private:
  static bool FullWanted() { return perf::ParseLevel(REXCVAR_GET(kk_performance_metrics)) == perf::Level::kFull; }

  void Run() {
#if defined(_WIN32)
    const auto module = GetModuleHandleW(L"rexgpu-native.dll");
    const auto read_native =
        module ? reinterpret_cast<uint32_t (*)(NrBenchmarkStats*)>(GetProcAddress(module, "nr_benchmark_stats")) : nullptr;
    // Turns on the renderer's copy of its frame GPU time and memory figures (a lock a frame, and the
    // memory reading it takes once a second anyway); the benchmark uses the same switch.
    if (const auto enable = module ? reinterpret_cast<void (*)(uint32_t)>(GetProcAddress(module, "nr_benchmark_enable"))
                                   : nullptr)
      enable(1);
#endif
    benchmark::SystemSampler sampler;
    MetricsView v;
#if defined(KK_DEV_TOOLS)
    double total_ms = 0;
#endif
    while (FullWanted()) {
      const auto start = std::chrono::steady_clock::now();
      const NrEffectStats e = nr::kk::EffectTimes();
      if (e.size) {
        v.api = e.api, v.vendor = e.vendor, v.in_w = e.input_width, v.in_h = e.input_height;
        v.out_w = e.output_width, v.out_h = e.output_height;
        v.upscaler = perf::UpscalerShort(e.upscaler);
      }
      v.sys = sampler.Sample(v.vendor);
#if defined(_WIN32)
      NrBenchmarkStats native;
      if (read_native && read_native(&native)) {
        v.gpu_ms = native.gpu_ms;
        if (native.memory_valid) v.vram = native.vram_used / 1048576.0, v.budget = native.vram_budget / 1048576.0;
      }
#endif
      v.sample_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      ++v.samples;
      {
        std::lock_guard lock(mutex_);
        view_ = v;
      }
#if defined(KK_DEV_TOOLS)
      // The worker's own time, logged every 20 readings (task 206 measurement).
      total_ms += v.sample_ms;
      if (v.samples % 20 == 0) {
        REXLOG_INFO("KK perf overlay: sampler {:.2f} ms a reading (mean of 20), {:.2f} ms the last", total_ms / 20,
                    v.sample_ms);
        total_ms = 0;
      }
#endif
      for (int i = 0; i < 10 && FullWanted(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    running_ = false;
  }

  std::atomic<bool> running_{false};
  std::thread worker_;
  mutable std::mutex mutex_;
  MetricsView view_;
};

// Colours: each row's label has its own (MangoHud's idea, our own colours); values are white with their
// units dimmed; the box is dark and see-through.
namespace pc {
constexpr ImU32 kBox = IM_COL32(8, 10, 16, 150);
constexpr ImU32 kValue = IM_COL32(236, 238, 242, 255);
constexpr ImU32 kUnit = IM_COL32(160, 166, 176, 255);
constexpr ImU32 kGpu = IM_COL32(88, 200, 120, 255);
constexpr ImU32 kVram = IM_COL32(190, 130, 230, 255);
constexpr ImU32 kCpu = IM_COL32(80, 160, 235, 255);
constexpr ImU32 kRam = IM_COL32(235, 125, 170, 255);
constexpr ImU32 kApi = IM_COL32(240, 120, 85, 255);
constexpr ImU32 kFps[3] = {IM_COL32(139, 213, 80, 255), IM_COL32(240, 200, 90, 255), IM_COL32(240, 110, 90, 255)};
constexpr ImU32 kGraph = IM_COL32(139, 213, 80, 255);
constexpr ImU32 kGraphBack = IM_COL32(0, 0, 0, 70);
constexpr ImU32 kGuide = IM_COL32(255, 255, 255, 40);
}  // namespace pc

class FpsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  bool NeedsContinuousPaint() override { return play::CoverScreen(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (freecam::HideHud()) return;
#if defined(KK_DEV_TOOLS)
    if (std::getenv("KK_DEV_UIPAINT_TEST")) {
      const double seconds = DevSecondsSinceMenu();
      static int phase = -1;
      const int next = seconds < 0 ? -1 : seconds < 12 ? 0 : seconds < 24 ? 1 : seconds < 36 ? 2 :
                       seconds < 48 ? 3 : seconds < 60 ? 4 : seconds < 72 ? 5 : 6;
      if (next >= 0 && next != phase) {
        phase = next;
        rex::cvar::SetFlagByName("ui_paint_on_demand", phase == 0 || phase == 2 || phase == 4 ? "false" : "true");
        rex::cvar::SetFlagByName("kk_frame_rate", phase < 2 || phase == 6 ? "30" : "0");
        rex::cvar::SetFlagByName("native_upscaler", phase == 4 || phase == 5 ? "dlss" : "off");
        rex::cvar::SetFlagByName("native_upscaler_quality", "quality");
        RequestUiPaint();
        REXLOG_INFO("UI repaint test phase {} at {:.3f} seconds", phase, seconds);
      }
    }
#endif
    // Black while the launcher's start request is carried out (the title and the game's menus never
    // show) and on the way back to the launcher.
    if (play::CoverScreen()) {
      ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));
      handoff::ContentDrawn();  // the game's black cover is up: the launcher before it may close
    }
    const perf::Level level = perf::ParseLevel(REXCVAR_GET(kk_performance_metrics));
    const bool benchmark_run = benchmark::Active();  // its own live overlay shows the same readings
    sampler_.Want(level == perf::Level::kFull && !benchmark_run);
    if (level == perf::Level::kOff || benchmark_run) return;
    DrawMetrics(io, level);
    DrawStutterLine(io);  // with the metrics, as with the old frame counter
  }

 private:
  void DrawMetrics(ImGuiIO& io, perf::Level level) {
    const auto stats = GetGuestFrameStats();
    if (stats.frame_count == 0) return;  // nothing to show yet (launcher, loading)
#if defined(KK_DEV_TOOLS)
    const auto draw_start = std::chrono::steady_clock::now();
#endif
    const float s = ImGui::GetFontSize() / 18.0f;
    const UiFonts& fonts = GetUiFonts();
    ImFont* bold = fonts.bold ? fonts.bold : ImGui::GetFont();
    ImFont* font = fonts.semibold ? fonts.semibold : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float size = 15.0f * s, unit = 12.0f * s, line = 19.0f * s, pad = 8.0f * s, gap = 6.0f * s;
    const ImVec2 origin(12 * s, 12 * s);
    const ImU32 fps_color = pc::kFps[perf::FpsBand(stats.fps)];
    char fps[16], ms[16];
    std::snprintf(fps, sizeof(fps), "%.0f", stats.fps);
    std::snprintf(ms, sizeof(ms), "%.1f", stats.frame_time_ms);
    auto text_w = [&](ImFont* f, float sz, const char* t) { return f->CalcTextSizeA(sz, FLT_MAX, 0, t).x; };

    // A row: the label in its colour, then (value, unit) cells, each right-aligned in its column.
    struct Cell {
      std::string value;
      const char* unit;
    };
    const float col_w = 66 * s;
    auto row = [&](float x0, float y, float label_w, const char* label, ImU32 color, std::initializer_list<Cell> cells) {
      dl->AddText(bold, size, ImVec2(x0, y), color, label);
      float x = x0 + label_w;
      for (const Cell& c : cells) {
        const float uw = c.unit && *c.unit ? text_w(font, unit, c.unit) + 3 * s : 0;
        const float vw = text_w(font, size, c.value.c_str());
        const float right = x + col_w;
        dl->AddText(font, size, ImVec2(right - uw - vw, y), pc::kValue, c.value.c_str());
        if (uw > 0) dl->AddText(font, unit, ImVec2(right - uw + 3 * s, y + (size - unit) * 0.8f), pc::kUnit, c.unit);
        x = right + gap;
      }
      return x;  // where the next column starts
    };

    if (level == perf::Level::kFps) {
      // FPS only: one row, the frame rate and the frame time.
      const float label_w = text_w(bold, size, "FPS") + 4 * s;
      const float w = label_w + col_w * 2 + gap;
      dl->AddRectFilled(origin, ImVec2(origin.x + w + pad * 2, origin.y + line + pad * 2 - 4 * s), pc::kBox, 4 * s);
      row(origin.x + pad, origin.y + pad - 2 * s, label_w, "FPS", fps_color, {{fps, ""}, {ms, "ms"}});
    } else {
      const MetricsView m = sampler_.Get();
      const std::string game = text::T("game");
      const float label_w = 58 * s, graph_h = 38 * s;
      const float w = label_w + col_w * 3 + gap * 2;
      const char* api = m.api == 1 ? "VULKAN" : m.api == 2 ? "D3D12" : "FPS";
      std::string upscale_line;
      if (m.in_w && m.out_w) {
        char sizes[64];
        std::snprintf(sizes, sizeof(sizes), "%ux%u > %ux%u", m.in_w, m.in_h, m.out_w, m.out_h);
        upscale_line = (m.upscaler.empty() || m.upscaler == "off" ? std::string(text::T("No upscaler")) : m.upscaler) +
                       "   " + sizes;
      }
      const float h = line * 5 + 4 * s + graph_h + (upscale_line.empty() ? 0 : line - 2 * s);
      dl->AddRectFilled(origin, ImVec2(origin.x + w + pad * 2, origin.y + h + pad * 2), pc::kBox, 4 * s);
      const float x0 = origin.x + pad;
      float y = origin.y + pad - 2 * s;
      auto note = [&](float x, float y_row, const std::string& t) {
        dl->AddText(font, unit, ImVec2(x, y_row + (size - unit) * 0.8f), pc::kUnit, t.c_str());
      };
      // GPU: the whole card's load; temperature and clock where the driver tells them (NVIDIA cards).
      char clock[16] = "-";
      if (m.sys.gpu_clock >= 0) std::snprintf(clock, sizeof(clock), "%.0f", m.sys.gpu_clock);
      if (m.sys.gpu_temperature >= 0 || m.sys.gpu_clock >= 0)
        row(x0, y, label_w, "GPU", pc::kGpu,
            {{perf::Whole(m.sys.gpu_load), "%"}, {perf::Whole(m.sys.gpu_temperature), "\xC2\xB0" "C"}, {clock, "MHz"}});
      else
        row(x0, y, label_w, "GPU", pc::kGpu, {{perf::Whole(m.sys.gpu_load), "%"}});
      y += line;
      // VRAM: in use and the budget; then the GPU's time for the last frame.
      char gpu_ms[16] = "-";
      if (m.gpu_ms >= 0) std::snprintf(gpu_ms, sizeof(gpu_ms), "%.1f", m.gpu_ms);
      row(x0, y, label_w, "VRAM", pc::kVram, {{perf::GiB(m.vram), "GiB"}, {"/ " + perf::GiB(m.budget), ""}, {gpu_ms, "ms"}});
      y += line;
      // CPU: every processor, then the game's share.
      note(row(x0, y, label_w, "CPU", pc::kCpu, {{perf::Whole(m.sys.cpu_load), "%"}, {perf::Whole(m.sys.cpu_game), "%"}}),
           y, game);
      y += line;
      // RAM: in use on the PC, then the game's.
      note(row(x0, y, label_w, "RAM", pc::kRam, {{perf::GiB(m.sys.ram_used), "GiB"}, {perf::GiB(m.sys.ram_game), "GiB"}}),
           y, game);
      y += line;
      // The graphics API, the frame rate and the frame time.
      row(x0, y, label_w, api, pc::kApi, {{fps, "FPS"}, {ms, "ms"}});
      y += line + 4 * s;
      // The frame time graph: the last frames, newest at the right.
      constexpr size_t kShown = 200;
      float times[kShown];
      const size_t n = GuestFrameTimes(times, kShown);
      const ImVec2 g0(x0, y), g1(x0 + w, y + graph_h);
      dl->AddRectFilled(g0, g1, pc::kGraphBack, 2 * s);
      if (n >= 2) {
        float slowest = 0;
        for (size_t i = 0; i < n; ++i) slowest = std::max(slowest, times[i]);
        const float top = perf::GraphTop(slowest);
        char label[16];
        std::snprintf(label, sizeof(label), "%.0f ms", top);
        dl->AddText(font, unit, ImVec2(g1.x - text_w(font, unit, label) - 3 * s, g0.y + 1 * s), pc::kUnit, label);
        // A faint guide at the current average frame time.
        const float guide = g1.y - std::min(float(stats.frame_time_ms) / top, 1.0f) * (graph_h - 2 * s);
        dl->AddLine(ImVec2(g0.x, guide), ImVec2(g1.x, guide), pc::kGuide, 1.0f);
        ImVec2 points[kShown];
        const float step = w / float(kShown - 1);
        for (size_t i = 0; i < n; ++i)
          points[i] = ImVec2(g1.x - step * float(n - 1 - i), g1.y - std::min(times[i] / top, 1.0f) * (graph_h - 2 * s));
        dl->AddPolyline(points, int(n), pc::kGraph, ImDrawFlags_None, 1.5f * s);
      }
      y += graph_h + 3 * s;
      if (!upscale_line.empty()) dl->AddText(font, unit + 1 * s, ImVec2(x0, y), pc::kUnit, upscale_line.c_str());
    }
#if defined(KK_DEV_TOOLS)
    // The overlay's own time on the UI thread, logged every 10 s (task 206 measurement).
    draw_us_ += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - draw_start).count();
    ++draws_;
    if (std::chrono::steady_clock::now() - draw_since_ > std::chrono::seconds(10)) {
      REXLOG_INFO("KK perf overlay: {} draws, {:.1f} us each ({})", draws_, draw_us_ / draws_, perf::LevelValue(level));
      draw_us_ = 0, draws_ = 0, draw_since_ = std::chrono::steady_clock::now();
    }
#endif
  }

  // The native renderer's pipeline stutter counter (--native_stutter_stats), at the top right.
  void DrawStutterLine(ImGuiIO& io) {
    const std::string stutter = nr::kk::StutterLine();
    if (stutter.empty()) return;
    const float s = ImGui::GetFontSize() / 18.0f;
    ImFont* font = GetUiFonts().semibold ? GetUiFonts().semibold : ImGui::GetFont();
    const float line_size = 13.0f * s;  // not "small": a macro in the Windows headers
    const ImVec2 st = font->CalcTextSizeA(line_size, FLT_MAX, 0, stutter.c_str());
    const ImVec2 q1(io.DisplaySize.x - 14 * s, 14 * s + st.y + 10 * s);
    const ImVec2 q0(q1.x - st.x - 20 * s, 14 * s);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(q0, q1, pc::kBox, 4 * s);
    dl->AddText(font, line_size, ImVec2(q0.x + 10 * s, q0.y + 5 * s), IM_COL32(220, 220, 230, 255), stutter.c_str());
  }

  MetricsSampler sampler_;
#if defined(KK_DEV_TOOLS)
  double draw_us_ = 0;
  int draws_ = 0;
  std::chrono::steady_clock::time_point draw_since_ = std::chrono::steady_clock::now();
#endif
};

}  // namespace

#if defined(KK_DEV_TOOLS)
void CreatePauseStandIn(rex::ui::ImGuiDrawer* drawer);  // pause_hooks.cpp (KK_DEV_PAUSE_STANDIN)
#endif

void CreateFpsOverlay(rex::ui::ImGuiDrawer* drawer) {
  new FpsOverlay(drawer);  // lives for the rest of the session
#if defined(KK_DEV_TOOLS)
  CreatePauseStandIn(drawer);
#endif
  // F2: Performance metrics Off, FPS only, Full (for this session; Settings saves the choice).
  rex::ui::RegisterBind("bind_kk_fps", "F2", "Switch performance metrics", [] {
    const perf::Level next = perf::NextLevel(perf::ParseLevel(REXCVAR_GET(kk_performance_metrics)));
    rex::cvar::SetFlagByName("kk_performance_metrics", perf::LevelValue(next));
#if defined(KK_DEV_TOOLS)
    REXLOG_INFO("KK dev F2: {}", perf::LevelValue(next));
#endif
    RequestUiPaint();
  });
}

}  // namespace kk
