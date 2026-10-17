#include "benchmark.h"
#include "handoff.h"
#include "freecam.h"
#include "benchmark_monitor.h"
#include "benchmark_results.h"
#include "graphics_panel_style.h"
#include "benchmark_data.h"
#include "benchmark_writer.h"
#include "benchmark_route.h"
#include "art.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <thread>
#include <stdexcept>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "diagnostics.h"
#include "debug_overlay.h"
#include "launcher_text.h"
#include "play_session.h"
#include "play_internal.h"
#include "hfr.h"
#include "settings.h"
#include "cheats.h"
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
REXCVAR_DEFINE_BOOL(kk_benchmark_record, false, "KK", "Record a controller benchmark route (not saved)");
REXCVAR_DEFINE_STRING(kk_benchmark_route, "", "KK", "Override benchmark-route.kkbr beside the game (not saved)");
REXCVAR_DEFINE_INT32(kk_benchmark_passes, 1, "KK",
    "How many times the benchmark plays the whole route; results are the median of the passes (1-5, not saved)");
REXCVAR_DEFINE_STRING(kk_benchmark_from, "", "KK",
    "Where the launcher opens the results: empty for About, settings for Settings > About (not saved)");
REXCVAR_DEFINE_DOUBLE(kk_benchmark_settle, kk::benchmark::kDefaultSettleSeconds, "KK",
    "Game seconds at the start of each chapter that are played but not measured (0-20, not saved)");

namespace kk::benchmark {
namespace {
using Clock = std::chrono::steady_clock;
enum class Phase { kOff, kLoading, kRunning, kSaving, kComplete };
std::atomic<Phase> phase{Phase::kOff};
std::atomic<double> game_seconds{0};
std::atomic<bool> interrupted{false};
std::atomic<bool> cancelled{false};
std::mutex mutex;
std::filesystem::path result_dir;
Clock::time_point last_guest{}, last_host{};
std::string settings, save_error;
Monitor monitor;
MonitorView final_monitor;
bool monitoring = true;
std::atomic<int64_t> frame_start_ns{0};
bool observer_available = false;
ResultWriter writer;
bool recording_route = false;
Recording recording;
std::atomic<size_t> chapter_index{0};
std::atomic<int> pass_index{0};
int passes = 1;
bool from_settings = false;  // started from Settings' About tab: the results open there (kk_benchmark_from)
double settle_seconds = kDefaultSettleSeconds;
// Set once the first swap at or after the settle time has been seen: the next
// interval is the first one measured in this chapter.
std::atomic<bool> measuring{false};
uint64_t game_frame = 0;
bool advance_chapter = false, returned = false;
double checkpoint_at = 0;
RouteReplay replay;
std::string route_id;
// Measured intervals per pass and chapter.
std::array<std::array<std::vector<double>, kRouteChapters>, kMaxPasses> pass_guest, pass_host;
// Chapters played to their end (a cancelled run keeps the ones it finished).
std::array<std::array<bool, kRouteChapters>, kMaxPasses> chapter_done{};
// Every guest swap, settling included, for frames.csv.
struct Swap { uint8_t pass, chapter; bool measured; float seconds; double ms; };
std::vector<Swap> swaps;
struct Drift {
  size_t compared = 0, unavailable = 0, mismatches = 0, missed_pulses = 0, missing_inputs = 0;
  double max_distance = 0, max_checkpoint_lag = 0, max_input_age = 0;
};
std::array<Drift, 4> chapter_drift;
std::atomic<bool> drift_detected{false};
std::atomic<bool> checkpoints_incomplete{false};
size_t next_checkpoint = 0;
std::vector<double> checkpoint_seconds;
double input_seconds = 0, previous_position_seconds = -1;
std::array<float, 3> previous_position{};
const uint8_t* replay_base = nullptr;
std::ostringstream drift_csv;
// The player's own controller during the run (the game reads the route instead).
std::mutex cancel_mutex;
CancelPress cancel_press;

const char* ChapterName(size_t c) {
  static const char* kNames[] = {"Necropolis", "V-Rex", "Kong", "Skull Island coast"};
  return c < 4 ? kNames[c] : "Chapter";
}

Recording ReadRecording(const std::filesystem::path& path, std::string* id = nullptr) {
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec || bytes > kMaxRecordingBytes) throw std::runtime_error("Missing or oversized benchmark route");
  std::ifstream file(path, std::ios::binary);
  std::string data(static_cast<size_t>(bytes), '\0');
  if (!file.read(data.data(), std::streamsize(data.size()))) throw std::runtime_error("Cannot read benchmark route");
  auto decoded = DecodeRecording(data);
  if (id) *id = RecordingId(data);
  return decoded;
}

void ApplyReplayControls() {
  // These affect the path after the captured input hook. Keep graphics/caps
  // available for comparisons; these process-local overrides are never saved.
  std::istringstream lines(recording.settings);
  for (std::string line; std::getline(lines, line);) {
    const auto split = line.find(" = ");
    if (split == std::string::npos) continue;
    const auto key = line.substr(0, split), value = line.substr(split + 3);
    const bool control = key == "kk_camera_sensitivity" || key == "kk_camera_modern" ||
        key == "kk_fov" || key == "kk_hfr_fixes" || key == "kk_cheats" || key == "kk_camera_shake";
    bool cheat = false;
    for (const auto& c : kCheats) cheat |= key == std::string("kk_cheat_") + c.id;
    if (control || cheat) rex::cvar::SetFlagByName(key, value);
  }
  rex::cvar::SetFlagByName("mnk_mode", "false");
}

std::string GraphSvg(const std::array<std::vector<double>, 4>& chapters) {
  std::ostringstream svg;
  svg.imbue(std::locale::classic());
  const size_t height = 200 * (recording.chapters.size() + 1);
  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"" << height
      << "\" viewBox=\"0 0 1000 " << height << "\">"
      << "<rect width=\"1000\" height=\"" << height << "\" fill=\"#080a0d\"/>";
  auto draw = [&](const std::vector<double>& samples, size_t row, const std::string& label) {
    const auto stats = Summarize(samples);
    const auto points = Graph(samples, 900);
    const double top = 30 + row * 200, scale = std::max(40.0, stats.worst_ms);
    svg << "<text x=\"30\" y=\"" << top << "\" fill=\"white\" font-family=\"sans-serif\">"
        << label << " / peak frame ms / scale " << scale << " / horizontal: measured elapsed time</text>"
        << "<polyline fill=\"none\" stroke=\"#c9a05e\" points=\"";
    for (size_t i = 0; i < points.size(); ++i)
      svg << 30 + 940.0 * i / std::max<size_t>(1, points.size() - 1) << ',' << top + 150 - 140 * points[i] / scale << ' ';
    svg << "\"/>";
  };
  std::vector<double> total;
  for (size_t c = 0; c < recording.chapters.size(); ++c)
    total.insert(total.end(), chapters[c].begin(), chapters[c].end());
  draw(total, 0, "total, every pass (loading and settling excluded)");
  for (size_t c = 0; c < recording.chapters.size(); ++c) {
    draw(chapters[c], c + 1, "Chapter entry " + std::to_string(recording.chapters[c].entry) + ", every pass");
  }
  return svg.str() + "</svg>\n";
}

void FinishRecording() {
  if (phase != Phase::kRunning && phase != Phase::kLoading) return;
  writer.Start([] {
    try {
      recording.complete = !interrupted;
      // A successful write alone does not prove that every chapter captured a
      // controller through its ending. Never publish an unusable route as complete.
      if (recording.complete) {
        try { DecodeRecording(EncodeRecording(recording)); }
        catch (const std::exception&) { recording.complete = false; interrupted = true; }
      }
      const bool ok = WriteResult(result_dir / "route.kkbr", EncodeRecording(recording));
      if (!ok) { std::lock_guard lock(mutex); save_error = "Could not save benchmark results."; }
      REXLOG_INFO("KK benchmark recording: saved {} ({} reads, {} chapters, complete {})",
          ok, recording.samples.size(), recording.chapters.size(), recording.complete);
    } catch (const std::exception& e) {
      REXLOG_ERROR("KK benchmark recording: {}", e.what());
      std::lock_guard lock(mutex); save_error = "Could not save benchmark results.";
    }
    phase = Phase::kComplete;
    RequestUiPaint();
  }, [] { phase = Phase::kSaving; });
}

std::string Settings() {
  std::string out = "Version: " KK_VERSION "\nRoute: recorded-controller-v1\n";
  out += DiagnosticsHardware();
  out += "\nSettings (requested; renderer fallbacks are recorded in diagnostics):\n";
  for (const char* key : {"native_backend", "kk_frame_rate", "fullscreen", "kk_fullscreen_mode",
      "window_width", "window_height", "video_mode_width", "video_mode_height", "monitor",
      "d3d12_allow_variable_refresh_rate_and_tearing", "vulkan_allow_present_mode_immediate",
      "vulkan_allow_present_mode_mailbox", "vulkan_allow_present_mode_fifo_relaxed", "ui_paint_on_demand",
      "kk_original_look", "kk_render_quality", "native_render_scale", "resolution_scale",
      "draw_resolution_scale_x", "draw_resolution_scale_y", "native_upscaler", "native_upscaler_quality",
      "native_upscaler_output", "native_frame_size",
      "native_ao_method", "native_ao_quality", "native_ao_samples", "native_ao_normal_smoothing", "ao_mode", "ao_strength",
      "native_gi", "native_gi_distance",
      "native_cel_shading", "native_cel_outline", "native_cel_darkness", "native_cel_bands", "native_cel_strength",
      "native_shadow_quality", "native_water_reflections", "native_volumetrics", "kk_fov",
      "kk_camera_sensitivity", "kk_camera_modern", "kk_hfr_fixes", "kk_cheats", "present_effect",
      "kk_deadzone", "kk_toggle_aim", "kk_invert_ls_x", "kk_invert_ls_y", "kk_invert_rs_x", "kk_invert_rs_y",
      "mnk_mode", "kk_camera_shake", "kk_fog", "kk_motion_blur", "kk_big_blur"})
    out += std::string(key) + " = " + rex::cvar::GetFlagByName(key) + '\n';
  for (size_t i = 0; i < kk::kPadCount; ++i) {
    const std::string key = std::string("kk_map_") + kk::GetPadInfo(static_cast<kk::Pad>(i)).id;
    out += key + " = " + rex::cvar::GetFlagByName(key) + '\n';
  }
  for (const auto& c : kCheats) {
    const std::string key = std::string("kk_cheat_") + c.id;
    out += key + " = " + rex::cvar::GetFlagByName(key) + '\n';
  }
  return RedactDiagnostics(std::move(out));
}

void ObserveReplayPosition(double seconds, const uint8_t* base) {
  std::array<float, 3> position;
  for (size_t i = 0; i < 3; ++i) position[i] = hfr::LoadF(base, 0x82D64250 + uint32_t(i) * 4);
  while (next_checkpoint < recording.checkpoints.size()) {
    const auto& c = recording.checkpoints[next_checkpoint];
    const double target = checkpoint_seconds[next_checkpoint];
    if (c.chapter != chapter_index || (std::isfinite(target) && target > seconds + 1e-9)) break;
    auto& d = chapter_drift[chapter_index];
    const double lag = std::isfinite(target) ? std::max(0.0, seconds - target) : 0;
    const bool available = c.valid && c.seconds >= 1 && recording.chapters[chapter_index].entry != 10 &&
        std::isfinite(target) && previous_position_seconds >= 0 && previous_position_seconds <= target;
    const auto compared = PositionAt(previous_position, previous_position_seconds, position, seconds, target);
    const double distance = available ? PositionDistance(compared, c.position) : 0;
    if (available) {
      ++d.compared;
      d.max_distance = std::max(d.max_distance, distance);
      if (distance > 1 || lag > .1) { ++d.mismatches; drift_detected = true; }
    } else {
      ++d.unavailable;
      if (c.valid && c.seconds >= 1) checkpoints_incomplete = true;
    }
    d.max_checkpoint_lag = std::max(d.max_checkpoint_lag, lag);
    drift_csv << pass_index.load() + 1 << ',' << chapter_index.load() + 1 << ',' << c.seconds << ',' << seconds << ','
        << distance << ',' << lag << ',' << available << ',' << target;
    for (float v : c.position) drift_csv << ',' << v;
    for (float v : compared) drift_csv << ',' << v;
    drift_csv << '\n';
    ++next_checkpoint;
  }
  previous_position = position;
  previous_position_seconds = seconds;
}

void EndReplayChapter() {
  // Different frame rates can stop on opposite sides of the recording's last
  // checkpoint. Account for those checks explicitly rather than dropping them.
  while (next_checkpoint < recording.checkpoints.size() &&
      recording.checkpoints[next_checkpoint].chapter == chapter_index) {
    const auto& c = recording.checkpoints[next_checkpoint++];
    ++chapter_drift[chapter_index].unavailable;
    if (c.valid && c.seconds >= 1 && recording.chapters[chapter_index].entry != 10)
      checkpoints_incomplete = true;
    drift_csv << pass_index.load() + 1 << ',' << chapter_index.load() + 1 << ',' << c.seconds << ',' << game_seconds
        << ",0," << game_seconds.load() - c.seconds << ",0," << checkpoint_seconds[next_checkpoint - 1]
        << ",0,0,0,0,0,0\n";
  }
}

std::string PresetLabel() {
  const auto preset = rex::cvar::GetFlagByName("kk_render_quality");
  for (const auto& item : RenderPresets()) if (item.id == preset) return item.label;
  return "Custom";
}

const char* StatusKey() {
  return cancelled ? "cancelled" : interrupted ? "interrupted" : drift_detected ? "drift" :
      checkpoints_incomplete ? "unverified" : "complete";
}

void Finish() {
  // Called by the game thread after the last measured swap. IO and sorting are
  // outside the measured interval and off the game/presenter threads.
  EndReplayChapter();
  measuring = false;
  writer.Start([] {
    try {
      monitor.Chapter(-1);
      monitor.Stop();
      final_monitor = monitor.View();
      std::vector<Swap> all;
      {
        std::lock_guard lock(mutex);
        all.swap(swaps);
      }
      const size_t chapters = recording.chapters.size();
      // Per pass and chapter, then the whole pass; a pass counts only when
      // every chapter of it was measured.
      std::vector<PassScope> rows;
      int complete_passes = 0;
      for (int p = 0; p < passes; ++p) {
        bool whole = true;
        for (size_t c = 0; c < chapters; ++c) whole = whole && chapter_done[p][c] && !pass_guest[p][c].empty();
        if (whole) ++complete_passes;
        std::vector<double> total;
        double cpu_sum = 0, cpu_w = 0, gpu_sum = 0, gpu_w = 0;
        for (size_t c = 0; c < chapters; ++c) {
          if (!chapter_done[p][c] || pass_guest[p][c].empty()) continue;
          PassScope r; r.pass = p + 1; r.chapter = int(c + 1);
          r.frames = Summarize(pass_guest[p][c]);
          const auto& bg = final_monitor.background[size_t(p) * kRouteChapters + c];
          r.other_cpu = bg[0].Mean(); r.other_gpu = bg[1].Mean(); r.gpu_clock = bg[2].Mean();
          r.gpu_temp = bg[3].Mean(); r.gpu_ms = bg[4].Mean(); r.game_ms = bg[5].Mean(); r.render_ms = bg[6].Mean();
          if (bg[0].weight > 0) { cpu_sum += bg[0].sum; cpu_w += bg[0].weight; }
          if (bg[1].weight > 0) { gpu_sum += bg[1].sum; gpu_w += bg[1].weight; }
          rows.push_back(r);
          total.insert(total.end(), pass_guest[p][c].begin(), pass_guest[p][c].end());
        }
        if (!whole) continue;  // the whole route only for a pass that played every chapter
        PassScope t; t.pass = p + 1; t.chapter = 0; t.frames = Summarize(total);
        t.other_cpu = cpu_w > 0 ? cpu_sum / cpu_w : -1; t.other_gpu = gpu_w > 0 ? gpu_sum / gpu_w : -1;
        rows.push_back(t);
      }
      std::string text = "King Kong recorded-route benchmark v3\n";
      text += std::string("Status: ") + StatusKey() + (cancelled || interrupted || drift_detected ? " (do not compare)\n" : "\n");
      text += "Passes: " + std::to_string(complete_passes) + " complete of " + std::to_string(passes) +
          " requested. Results are the median of the complete passes.\n";
      text += "Settle: the first " + std::to_string(settle_seconds) +
          " game seconds of each chapter are played but not measured.\n";
      text += settings;
      text += "Recording build: " + recording.build + "\nRecording ID (FNV-1a): " + route_id + '\n';
      text += "No confirmed game RNG seed API is exposed. AI, particles, collisions and load timing can vary.\n";
      text += "Replay holds the latest pad at each pre-step game time, without changing simulation dt.\n";
      text += "Jack position checkpoints: 1 unit tolerance, first second excluded. Kong position unavailable.\n";
      text += "Spread = (highest - lowest) / median of the passes' average FPS. Confidence: High up to 3%, "
              "Medium up to 6%, Low above. One pass has no confidence figure.\n";
      text += "Background: other programs' CPU above " + std::to_string(int(kBusyOtherCpu)) +
          "% of all processors, or their use of the game's GPU above " + std::to_string(int(kBusyOtherGpu)) +
          "%, marks a pass as busy. GPU use = the busiest 3D/compute engine of the adapter the game renders on, "
          "summed over other programs; video encode/decode (recording, streaming, video playback), copy engines "
          "and other adapters are not counted.\n";
      for (int scope = 0; scope <= int(chapters); ++scope) {
        const auto summary = SummarizePasses(rows, scope);
        text += "\n" + (scope ? "Chapter " + std::to_string(scope) + " entry " +
            std::to_string(recording.chapters[size_t(scope - 1)].entry) + " (" + ChapterName(size_t(scope - 1)) + ")"
            : std::string("Total")) + ", median of " + std::to_string(summary.passes) + " passes\n" +
            Describe(summary.median);
        char line[512];
        std::snprintf(line, sizeof(line), "Pass spread: %.2f%% (%.1f to %.1f FPS), confidence %s, busy passes %zu\n",
            summary.spread, summary.min_fps, summary.max_fps, ConfidenceLabel(summary.confidence), summary.busy_passes);
        text += line;
        for (const auto& r : rows) if (r.chapter == scope) {
          std::snprintf(line, sizeof(line), "  Pass %d: %.2f FPS average, %.2f 1%% low, worst %.1f ms, other CPU %.1f%%, other GPU %.1f%%%s;"
              " GPU %.0f MHz %.0f C, GPU frame %.2f ms, game thread %.2f ms, render thread %.2f ms\n",
              r.pass, r.frames.average_fps, r.frames.low_fps, r.frames.worst_ms, r.other_cpu, r.other_gpu,
              r.Busy() ? " (busy)" : "", r.gpu_clock, r.gpu_temp, r.gpu_ms, r.game_ms, r.render_ms);
          text += line;
        }
        if (scope) {
          const auto& d = chapter_drift[size_t(scope - 1)];
          text += "Checkpoints compared/unavailable/mismatched (every pass): " + std::to_string(d.compared) + "/" +
              std::to_string(d.unavailable) + "/" + std::to_string(d.mismatches) +
              "\nMax position distance: " + std::to_string(d.max_distance) +
              "\nMax checkpoint time lag seconds: " + std::to_string(d.max_checkpoint_lag) +
              "\nMax input age seconds: " + std::to_string(d.max_input_age) +
              "\nPolls with skipped button/trigger transitions: " + std::to_string(d.missed_pulses) +
              "\nPolls missing recorded inputs after initial 0.25 seconds: " + std::to_string(d.missing_inputs) + '\n';
        }
      }
      text += "\nGame swap intervals include caps and stalls; loading and settling are excluded.\n"
              "1% low = reciprocal of the mean of the slowest ceil(N/100) intervals.\n"
              "Compare the same route, settings, cap, API, cache and power state. AI can differ.\n";
      // Machine-readable aggregate (median of the passes), the format earlier runs used.
      std::ostringstream summary;
      summary.imbue(std::locale::classic());
      summary << "scope,count,seconds,average_fps,low1_fps,low01_fps,p50_ms,p95_ms,p99_ms,worst_ms,stutters";
      for (const auto* name : kMetricNames) summary << ',' << name << ",peak " << name;
      summary << ",final_fps\n";
      for (size_t c = 0; c <= chapters; ++c) {
        const Stats s = SummarizePasses(rows, int(c)).median;
        summary << c << ',' << s.count << ',' << std::setprecision(12) << s.seconds << ',' << s.average_fps << ','
            << s.low_fps << ',' << s.low01_fps << ',' << s.p50_ms << ',' << s.p95_ms << ',' << s.p99_ms << ',' << s.worst_ms << ',' << s.stutters;
        const auto& m = final_monitor.metrics[c];
        text += "\n" + std::string(c ? "Chapter " + std::to_string(c) : "Total") + " / system counters, every pass / mean; peak:\n";
        for (size_t i = 0; i < kMetricCount; ++i) {
          summary << ',' << m[i].Mean() << ',' << m[i].peak;
          text += std::string(kMetricNames[i]) + ": " + std::to_string(m[i].Mean()) + "; " + std::to_string(m[i].peak) + "\n";
        }
        text += std::string("Bottleneck indication: ") + Bound(m, s.count ? s.seconds * 1000 / s.count : -1) + "\n";
        summary << ',' << s.final_fps << '\n';
      }
      text += "Monitor worker ms: " + std::to_string(final_monitor.worker_ms) + ", samples: " + std::to_string(final_monitor.samples) + ", dropped frames: " + std::to_string(final_monitor.dropped) + "\n";
      const std::string api = final_monitor.api == 1 ? "Vulkan" : final_monitor.api == 2 ? "DirectX 12" : "Unavailable";
      const std::string render = std::to_string(final_monitor.width) + "x" + std::to_string(final_monitor.height) +
          " > " + std::to_string(final_monitor.output_width) + "x" + std::to_string(final_monitor.output_height);
      text += "API: " + api + "\nRender resolution: " + render + "\nUpscaler: " + final_monitor.upscaler + "\n";
      text += "\nRecording settings:\n" + recording.settings;
      text = RedactDiagnostics(std::move(text));
      // Every swap, settling included: pass, chapter, game seconds at the swap.
      std::ostringstream csv;
      csv.imbue(std::locale::classic());
      csv << "pass,chapter,game_seconds,frame_ms,measured\n" << std::fixed;
      for (const auto& s : all)
        csv << int(s.pass) + 1 << ',' << int(s.chapter) + 1 << ',' << std::setprecision(4) << s.seconds << ','
            << std::setprecision(4) << s.ms << ',' << s.measured << '\n';
      // For the launcher's results page.
      std::string status = std::string("status=") + StatusKey() + "\npasses=" + std::to_string(passes) +
          "\ncomplete_passes=" + std::to_string(complete_passes) + "\nsettle=" + std::to_string(settle_seconds) +
          "\napi=" + api + "\nrender=" + render + "\nupscaler=" + final_monitor.upscaler +
          "\npreset=" + PresetLabel() + "\ncap=" + rex::cvar::GetFlagByName("kk_frame_rate") + '\n';
      for (size_t c = 0; c < chapters; ++c) status += "chapter" + std::to_string(c + 1) + '=' + ChapterName(c) + '\n';
      std::array<std::vector<double>, 4> graph_chapters;
      for (int p = 0; p < passes; ++p)
        for (size_t c = 0; c < chapters; ++c)
          graph_chapters[c].insert(graph_chapters[c].end(), pass_guest[p][c].begin(), pass_guest[p][c].end());
      const bool ok = WriteResult(result_dir / "summary.csv", summary.str()) &&
          WriteResult(result_dir / "passes.csv", PassesCsv(rows)) &&
          WriteResult(result_dir / "frames.csv", csv.str()) &&
          WriteResult(result_dir / "drift.csv", drift_csv.str()) &&
          WriteResult(result_dir / "frame-times.svg", GraphSvg(graph_chapters)) &&
          WriteResult(result_dir / "results.txt", text) &&
          WriteResult(result_dir / "status.txt", status);  // last: marks a complete set
      if (!ok) { std::lock_guard lock(mutex); save_error = "Could not save benchmark results."; }
      const auto total = SummarizePasses(rows, 0);
      REXLOG_INFO("KK benchmark: complete, status {}, passes {}, average {:.3f}, 1% low {:.3f}, worst {:.3f} ms, spread {:.2f}%, saved {}",
                  StatusKey(), complete_passes, total.median.average_fps, total.median.low_fps, total.median.worst_ms,
                  total.spread, ok);
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
  kk::handoff::OnPresent();  // this observer replaces the handoff's (handoff.h)
  if (recording_route) return;
  if (phase != Phase::kRunning) return;
  const auto now = Clock::now();
  std::lock_guard lock(mutex);
  if (phase != Phase::kRunning) return;
  auto& host = pass_host[size_t(pass_index.load())][chapter_index];
  if (measuring && last_host != Clock::time_point{} && host.size() < kMaxSamples)
    host.push_back(std::chrono::duration<double, std::milli>(now - last_host).count());
  last_host = now;
}

void Cancel(const char* how) {
  const auto p = phase.load();
  if (p != Phase::kRunning && p != Phase::kLoading) return;
  if (!cancelled.exchange(true)) REXLOG_INFO("KK benchmark: cancelled by the player ({})", how);
  interrupted = true;
}

// The live panel (the F8 panel's style). It takes no focus: the run is
// cancelled with B, Back or Start on the controller, Esc, or its Cancel button.
class Dialog final : public rex::ui::ImGuiDialog {
 public:
  explicit Dialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  bool NeedsContinuousPaint() override { return phase == Phase::kSaving || phase == Phase::kComplete; }
 protected:
  void OnDraw(ImGuiIO& io) override {
    if (freecam::HideHud()) return;
    using text::T;
    const auto p = phase.load();
    if (p == Phase::kOff) return;
    if (p == Phase::kRunning && (DebugOverlayOpen() || play::PauseMenuOpen())) Interrupted();
    const bool live = p == Phase::kRunning || p == Phase::kLoading;
    if (live && !recording_route && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) Cancel("Esc");
    const auto draw_start = Clock::now();
#if defined(KK_DEV_TOOLS)
    // Offline/game A/B aid only: remove the entire live monitor from baseline.
    if (live && !monitoring) return;
#endif
    const float scale = std::clamp(ImGui::GetFontSize() / 18.f, .75f, 2.f);
    const MonitorView v = monitor.View();
    ImGui::SetNextWindowPos(live ? ImVec2(16 * scale, 16 * scale) : ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2),
        ImGuiCond_Always, live ? ImVec2(0, 0) : ImVec2(.5f, .5f));
    const float width = std::min((live ? 510.f : 420.f) * scale, io.DisplaySize.x - 32);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, std::max(120.f, io.DisplaySize.y - 32 * scale)));
    ui::PushGraphicsPanelStyle(.90f);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ui::color::kAccent);
    const auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | (live ? 0 : ImGuiWindowFlags_NoInputs);
    ImGui::Begin((std::string(T("Benchmark")) + "###benchmark-panel").c_str(), nullptr, flags);
    if (live) {
      if (recording_route) Heading("Recording route"); else Heading("Live performance");
      if (p == Phase::kLoading) ImGui::TextUnformatted(T("Loading benchmark..."));
      else ImGui::Text("%s %zu / %zu | %.0f / %.0f s", T("Chapter"), chapter_index.load() + 1,
          recording.chapters.size(), game_seconds.load(), recording.chapters[chapter_index].duration);
      if (!recording_route && passes > 1)
        ImGui::TextUnformatted(text::F("Pass {0} of {1}", {std::to_string(pass_index.load() + 1), std::to_string(passes)}).c_str());
      if (!recording_route && p == Phase::kRunning && !measuring)
        ImGui::TextDisabled("%s", T("Settling, not measured yet"));
      if (!recording_route) {
        if (cancelled) ImGui::TextUnformatted(T("Cancelling..."));
        else {
          if (ImGui::Button(T("Cancel"))) Cancel("mouse");
          ImGui::SameLine();
          ImGui::TextDisabled("%s", T("B, Back or Start on the controller, or Esc, also cancels."));
        }
        ImGui::Separator();
      }
      if (!recording_route && monitoring) {
        ImGui::Text("%s: %.1f | %s: %.1f", T("Current FPS"), v.current_fps, T("Average FPS"), v.frames.average_fps);
        ImGui::Text("%s: %.1f | %s: %.1f", T("1% low FPS"), v.frames.low_fps, T("0.1% low FPS"), v.frames.low01_fps);
        ImGui::Text("p50 / p95 / p99: %.2f / %.2f / %.2f ms | %s: %zu", v.frames.p50_ms, v.frames.p95_ms, v.frames.p99_ms, T("Stutters"), v.frames.stutters);
        MetricReadout(v);
        ImGui::TextColored(ImColor(ui::color::kAccent), "%s: %s", T("Bottleneck indication"),
            T(Bound(v.metrics[std::min<size_t>(4, chapter_index.load() + 1)],
                v.current_fps > 0 ? 1000 / v.current_fps : -1)));
        ImGui::TextUnformatted(T("Frame time (ms)"));
        if (v.recent_count) ImGui::PlotLines("##live-graph", v.recent.data(), int(v.recent_count), 0, nullptr, 0,
            std::max(40.f, *std::max_element(v.recent.begin(), v.recent.begin() + v.recent_count)), ImVec2(-1, 64 * scale));
        Configuration(v);
      }
    } else {
      ImGui::TextUnformatted(T(p == Phase::kSaving ? "Saving results..." : "Returning to the launcher..."));
      std::lock_guard lock(mutex);
      if (!save_error.empty()) ImGui::TextWrapped("%s", T(save_error).c_str());
    }
    ImGui::End(); ImGui::PopStyleColor(); ui::PopGraphicsPanelStyle();
    draw_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - draw_start).count(); ++draw_count_;
    if (!live && !logged_cost_) {
      logged_cost_ = true;
      REXLOG_INFO("KK benchmark UI CPU: {} draws, {:.6f} ms/draw (includes UI construction, not GPU)", draw_count_, draw_ms_ / draw_count_);
    }
  }
 private:
  static void Heading(const char* label) {
    ImGui::TextColored(ImColor(ui::color::kAccent), "%s", text::T(label)); ImGui::Separator();
  }
  static std::string Value(double v) {
    if (v < 0 || !std::isfinite(v)) return text::T("Unavailable");
    char buf[64]; std::snprintf(buf, sizeof(buf), "%.2f", v); return buf;
  }
  static void MetricReadout(const MonitorView& v) {
    if (ImGui::BeginTable("##metrics", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
      for (size_t i = 0; i < kMetricCount; ++i) {
        const double value = v.current[i];
        if (i >= kClock && value < 0) continue;
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(text::T(kMetricNames[i]));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(Value(value).c_str());
      }
      ImGui::EndTable();
    }
  }
  static void Configuration(const MonitorView& v) {
    using text::T;
    ImGui::TextWrapped("%s: %ux%u > %ux%u | %s", T("Render resolution"), v.width, v.height, v.output_width, v.output_height,
        v.upscaler.empty() ? T("Unavailable") : v.upscaler == "off" ? T("Off") : v.upscaler.c_str());
    const auto preset = rex::cvar::GetFlagByName("kk_render_quality");
    const char* preset_label = "Custom";
    for (const auto& item : RenderPresets()) if (item.id == preset) preset_label = item.label;
    ImGui::Text("%s: %s | API: %s", T("Preset"), T(preset_label), v.api == 1 ? "Vulkan" : v.api == 2 ? "DirectX 12" : T("Unavailable"));
  }
  double draw_ms_ = 0; uint64_t draw_count_ = 0; bool logged_cost_ = false;
};

}  // namespace

bool Requested() { return REXCVAR_GET(kk_benchmark) || REXCVAR_GET(kk_benchmark_record); }
bool Active() { return phase != Phase::kOff; }
bool RecordingRoute() { return recording_route; }
std::filesystem::path RouteFile() {
  const auto override_path = REXCVAR_GET(kk_benchmark_route);
  return override_path.empty() ? rex::filesystem::GetExecutableFolder() / "benchmark-route.kkbr"
                               : std::filesystem::path(override_path);
}
bool RouteAvailable() {
  try { ReadRecording(RouteFile()); return true; }
  catch (const std::exception& e) { REXLOG_WARN("KK benchmark route: {}", e.what()); return false; }
}
void Shutdown() { writer.Shutdown(); monitor.Stop(); }

std::filesystem::path Prepare(const std::filesystem::path& root) {
  // Unique directory, never an existing player save. Failure aborts startup,
  // rather than falling through to a benchmark that writes into normal saves.
  const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  recording_route = REXCVAR_GET(kk_benchmark_record);
  passes = std::clamp(REXCVAR_GET(kk_benchmark_passes), 1, kMaxPasses);
  from_settings = std::string(REXCVAR_GET(kk_benchmark_from)) == "settings";
  settle_seconds = std::clamp(REXCVAR_GET(kk_benchmark_settle), 0.0, 20.0);
  if (!std::isfinite(settle_seconds)) settle_seconds = kDefaultSettleSeconds;
#if defined(KK_DEV_TOOLS)
  monitoring = std::getenv("KK_DEV_BENCH_MONITOR_OFF") == nullptr;
#endif
  if (!recording_route) {
    recording = ReadRecording(RouteFile(), &route_id);
    replay.Attach(recording);
    checkpoint_seconds = CheckpointStartSeconds(recording);
    ApplyReplayControls();
    for (size_t chapter = 0; chapter < recording.chapters.size(); ++chapter) {
      bool available = false;
      for (const auto& c : recording.checkpoints)
        available |= c.chapter == chapter && c.valid && c.seconds >= 1 && recording.chapters[chapter].entry != 10;
      if (!available) checkpoints_incomplete = true;
      // Never settle longer than half a chapter.
      settle_seconds = std::min(settle_seconds, recording.chapters[chapter].duration / 2);
    }
    drift_csv.imbue(std::locale::classic());
    drift_csv << "pass,chapter,recorded_seconds,replay_seconds,position_distance,time_lag_seconds,available,position_seconds,recorded_x,recorded_y,recorded_z,replay_x,replay_y,replay_z\n";
  }
  result_dir = root / "benchmarks" / ((recording_route ? "record-" : "run-") + std::to_string(stamp));
  std::filesystem::create_directories(result_dir);
  const auto private_root = result_dir / "user";
  if (!std::filesystem::create_directory(private_root))
    throw std::runtime_error("Could not create private benchmark user data");
  phase = Phase::kLoading;
  for (const char* name : {"kk_benchmark", "kk_benchmark_record", "kk_benchmark_route", "kk_benchmark_passes",
                           "kk_benchmark_settle", "kk_benchmark_from"})
    rex::cvar::ResetToDefault(name);
  if (recording_route) {
    double duration = 45;
#if defined(KK_DEV_TOOLS)
    if (const char* s = std::getenv("KK_DEV_BENCH_SECONDS"); s && *s)
      duration = std::clamp(std::atof(s), 1.0, 120.0);
#endif
    recording.chapters = DefaultRoute(duration);
    recording.build = KK_VERSION " / " KK_BENCH_BUILD " / " __DATE__ " " __TIME__;
    recording.samples.reserve(262144);
  }
#if defined(_WIN32)
  // Other programs at normal priority (a build, a download) take turns with the
  // game less often while it is measured. This process exits after the run.
  if (!recording_route && SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS))
    REXLOG_INFO("KK benchmark: process priority above normal for the run");
#endif
  REXLOG_INFO("KK benchmark: {} passes, {} settle seconds per chapter", passes, settle_seconds);
  play::SetStartRequest({play::StartKind::kChapter, 0,
      recording.chapters.front().entry, "Benchmark"});
  swaps.reserve(size_t(passes) * 65536);
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
  if (!recording_route && monitoring) monitor.Start();
  // Linux observer implementation remains deferred under the standing hold.
  REXLOG_INFO("KK benchmark: host present observer {}", observer_available ? "available" : "unavailable");
}

void LevelLoaded() {
  if (phase != Phase::kLoading) { if (Active()) Interrupted(); return; }
  // The SDK reads the settings file after Prepare. Reapply these before the
  // first gameplay poll as well, without changing graphics or the frame cap.
  if (!recording_route && chapter_index == 0 && pass_index == 0) ApplyReplayControls();
  if (chapter_index == 0 && pass_index == 0) { settings = Settings();  }
  game_seconds = 0;
  game_frame = 0;
  input_seconds = 0;
  previous_position_seconds = -1;
  replay_base = nullptr;
  checkpoint_at = 0;
  measuring = false;
  {
    std::lock_guard lock(mutex); last_guest = last_host = {};
  }
  if (recording_route) {
    if (chapter_index == 0) recording.settings = settings;
    game_frame = 0;
    checkpoint_at = 0;
    phase = Phase::kRunning;
    REXLOG_INFO("KK benchmark recording: chapter {} entry {}, duration {} game seconds",
        chapter_index.load() + 1, recording.chapters[chapter_index].entry, recording.chapters[chapter_index].duration);
    return;
  }
  monitor.Chapter(-1);
  replay.Reset(chapter_index);
  while (next_checkpoint < recording.checkpoints.size() &&
      recording.checkpoints[next_checkpoint].chapter < chapter_index) {
    ++chapter_drift[recording.checkpoints[next_checkpoint].chapter].unavailable;
    ++next_checkpoint;
  }
  phase = Phase::kRunning;
  REXLOG_INFO("KK benchmark replay: pass {} chapter {} entry {}, duration {} game seconds",
      pass_index.load() + 1, chapter_index.load() + 1, recording.chapters[chapter_index].entry,
      recording.chapters[chapter_index].duration);
}

void Frame(double dt, double scaled_dt, const uint8_t* base) {
  const auto p = phase.load();
  if (p != Phase::kRunning) return;
  if (recording_route) {
    if (interrupted) { FinishRecording(); return; }
    if (!std::isfinite(dt) || dt <= 0 || !std::isfinite(scaled_dt) || scaled_dt < 0) {
      interrupted = true; FinishRecording(); return;
    }
    // The same unscaled game delta used by the old benchmark, not a wall clock
    // or a frame count. Preserve both deltas and the title's absolute clock.
    game_seconds = game_seconds.load() + dt;
    ++game_frame;
    if (game_seconds >= checkpoint_at) {
      Checkpoint c; c.chapter = chapter_index; c.seconds = game_seconds;
      // Jack's static position block, documented by the native renderer timing
      // investigation. Kong has a different actor; never claim this tracks him.
      c.valid = recording.chapters[chapter_index].entry != 10;
      for (size_t i = 0; i < 3; ++i) {
        c.position[i] = hfr::LoadF(base, 0x82D64250 + uint32_t(i) * 4);
        c.valid = c.valid && std::isfinite(c.position[i]);
      }
      if (!c.valid) c.position = {};
      recording.checkpoints.push_back(c);
      checkpoint_at += 1;
    }
    return;
  }
  if (interrupted) { Finish(); return; }
  if (!std::isfinite(dt) || dt <= 0) { interrupted = true; Finish(); return; }
  if (monitoring) monitor.GameThread();
  frame_start_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(), std::memory_order_release);
  input_seconds = game_seconds;
  game_seconds = game_seconds.load() + dt;
  ++game_frame;
  replay_base = base;
  ObserveReplayPosition(input_seconds, base);
}

#if defined(KK_DEV_TOOLS)
// KK_DEV_BENCH_SHOTS="<chapter>:<from>-<to>[,...]" (task 179): every guest frame of the route between those
// game seconds of that chapter (1-based) saved as bench_c<chapter>_<ms> in KK_DEV_SHOTS_DIR, so runs can
// be compared frame by frame at the same game time.
void BenchShots(size_t chapter, double seconds) {
  struct Window { size_t chapter; double from, to; };
  static const std::vector<Window> windows = [] {
    std::vector<Window> w;
    const char* v = std::getenv("KK_DEV_BENCH_SHOTS");
    for (std::string all = v ? v : "", item; !all.empty();) {
      const size_t comma = all.find(',');
      item = all.substr(0, comma);
      all = comma == std::string::npos ? "" : all.substr(comma + 1);
      Window x{};
      if (std::sscanf(item.c_str(), "%zu:%lf-%lf", &x.chapter, &x.from, &x.to) == 3) w.push_back(x);
    }
    return w;
  }();
  for (const Window& w : windows) {
    if (w.chapter != chapter + 1 || seconds < w.from || seconds > w.to) continue;
    const char* dir = std::getenv("KK_DEV_SHOTS_DIR");
    art::CaptureFrame(std::filesystem::path(dir && *dir ? dir : ".") /
                      ("bench_c" + std::to_string(chapter + 1) + "_" + std::to_string(int(seconds * 1000 + 0.5)) +
                       art::kDevCaptureExtension));
    return;
  }
}
#endif

void GuestSwap() {
  if (phase != Phase::kRunning) return;
  if (recording_route) {
    if (interrupted) { FinishRecording(); return; }
    if (game_seconds >= recording.chapters[chapter_index].duration) {
      if (chapter_index + 1 == recording.chapters.size()) FinishRecording();
      else { monitor.Chapter(-1); std::lock_guard lock(mutex); phase = Phase::kLoading; advance_chapter = true; }
    }
    return;
  }
  const auto now = Clock::now();
  const double seconds = game_seconds.load();
  const size_t chapter = chapter_index.load();
  const int pass = pass_index.load();
#if defined(KK_DEV_TOOLS)
  BenchShots(chapter, seconds);
#endif
  {
    std::lock_guard lock(mutex);
    const bool was_measuring = measuring.load();
    if (last_guest != Clock::time_point{} && swaps.size() < kMaxSamples) {
      const double ms = std::chrono::duration<double, std::milli>(now - last_guest).count();
      swaps.push_back({uint8_t(pass), uint8_t(chapter), was_measuring, float(seconds), ms});
      if (was_measuring) {
        pass_guest[size_t(pass)][chapter].push_back(ms);
        // Game logic and guest swaps may run on different title threads. This is
        // age of the latest observed tick, not an invented pairing of two frames.
        const auto tick_ns = frame_start_ns.load(std::memory_order_acquire);
        const auto swap_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        if (monitoring) monitor.Record(ms, tick_ns > 0 && swap_ns >= tick_ns ? (swap_ns - tick_ns) / 1000000.0 : -1);
      }
    } else if (swaps.size() >= kMaxSamples) interrupted = true;
    last_guest = now;
    // The settle time is over: the next interval is the chapter's first measured one.
    if (!was_measuring && seconds >= settle_seconds) {
      measuring = true;
      monitor.Chapter(int(chapter), pass);
    }
  }
  if (interrupted) Finish();
  else if (seconds >= recording.chapters[chapter].duration) {
    // The final swap follows the simulation, so its state covers the last
    // recorded pre-step checkpoint without inventing an extra game frame.
    if (replay_base) ObserveReplayPosition(seconds, replay_base);
    EndReplayChapter();
    chapter_done[size_t(pass)][chapter] = true;
    measuring = false;
    monitor.Chapter(-1);
    if (chapter + 1 == recording.chapters.size() && pass + 1 >= passes) Finish();
    else { std::lock_guard lock(mutex); phase = Phase::kLoading; advance_chapter = true; }
  }
}

bool Pad(uint32_t user, uint8_t* state, uint32_t& status) {
  if (!Active() || recording_route) return false;
  // The player's own controller (what the game would have read): only a cancel.
  bool physical = user == 0 && status == 0;
  uint16_t buttons = physical ? uint16_t(state[4] << 8 | state[5]) : 0;
#if defined(KK_DEV_TOOLS)
  // Test aid: KK_DEV_BENCH_CANCEL_PAD=<game seconds> presses B on the "player's
  // controller" for half a second at that time of the first chapter.
  static const double dev_cancel_at = [] {
    const char* v = std::getenv("KK_DEV_BENCH_CANCEL_PAD");
    return v && *v ? std::atof(v) : -1.0;
  }();
  if (user == 0 && dev_cancel_at >= 0) {
    physical = true;
    const double t = game_seconds.load();
    buttons = phase == Phase::kRunning && pass_index == 0 && chapter_index == 0 && t >= dev_cancel_at &&
        t < dev_cancel_at + .5 ? 0x2000 : 0;
  }
#endif
  if (physical) {
    bool cancel;
    { std::lock_guard lock(cancel_mutex); cancel = cancel_press.Update(buttons); }
    if (cancel) Cancel("controller");
  }
  std::memset(state, 0, 16);
  status = user == 0 ? 0 : 1167;
  if (phase == Phase::kRunning) {
    uint32_t missed = 0;
    if (const auto* sample = replay.Read(user, input_seconds, missed, game_frame == 0)) {
      status = sample->status;
      std::memcpy(state, sample->state.data(), 16);
      auto& d = chapter_drift[chapter_index];
      d.max_input_age = std::max(d.max_input_age, input_seconds - SampleStartSeconds(*sample));
      if (missed) { ++d.missed_pulses; drift_detected = true; }
      if (input_seconds - SampleStartSeconds(*sample) > .25) drift_detected = true;
    } else if (user < 4 && game_seconds > .25) {
      ++chapter_drift[chapter_index].missing_inputs;
      drift_detected = true;
    }
  }
  return true;
}

void PadRead(uint32_t user, const uint8_t* state, uint32_t status, const uint8_t* base) {
  if (!recording_route || phase != Phase::kRunning) return;
  if (recording.samples.size() >= kMaxSamples) { interrupted = true; return; }
  PadSample sample;
  sample.chapter = chapter_index; sample.frame = game_frame;
  sample.seconds = game_seconds;
  // Read metadata with the pad, including polls after LevelLoaded but before
  // the first frame hook. Cached deltas/clocks could still belong to the last chapter.
  sample.raw_dt = hfr::LoadF(base, 0x82C4EDBC);
  sample.scaled_dt = hfr::LoadF(base, hfr::kFrameTime);
  sample.game_ms = hfr::Load32(base, 0x82CCAE1C);
  sample.user = user; sample.status = status;
  if (state && status == 0) std::memcpy(sample.state.data(), state, 16);
  recording.samples.push_back(sample);
}

int NextChapter() {
  if (!Active()) return -1;
  if (interrupted && phase == Phase::kLoading) {
    if (recording_route) FinishRecording(); else Finish();
  }
  if (phase == Phase::kLoading && advance_chapter) {
    advance_chapter = false;
    if (chapter_index + 1 == recording.chapters.size()) {
      // The next pass plays the whole route again from its first chapter.
      chapter_index = 0;
      ++pass_index;
      next_checkpoint = 0;
#if defined(_WIN32) && defined(KK_DEV_TOOLS)
      // Test aid: KK_DEV_BENCH_PRIORITY_AB=1 measures even passes at normal
      // priority, odd passes above normal, to compare them under the same load.
      if (std::getenv("KK_DEV_BENCH_PRIORITY_AB")) {
        const bool boost = pass_index % 2 == 0;
        SetPriorityClass(GetCurrentProcess(), boost ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
        REXLOG_INFO("KK benchmark: pass {} priority {}", pass_index.load() + 1, boost ? "above normal" : "normal");
      }
#endif
    } else {
      ++chapter_index;
    }
    return recording.chapters[chapter_index].entry;
  }
  if (phase == Phase::kComplete && !returned) {
    // Results (or the recorded route) are saved: the launcher shows them. A
    // route that could not be saved keeps its message on screen instead.
    if (recording_route) { std::lock_guard lock(mutex); if (!save_error.empty()) return -1; }
    returned = true;
    play::ReturnToLauncher(recording_route ? "benchmark route recording saved" : "benchmark finished",
                           recording_route ? "about" : from_settings ? "benchmark-settings" : "benchmark");
  }
  return -1;
}

void Interrupted() {
  const auto p = phase.load();
  if (p == Phase::kRunning || p == Phase::kLoading) interrupted = true;
}

void CreateBenchmarkDialog(rex::ui::ImGuiDrawer* drawer) {
  new Dialog(drawer);
}
}  // namespace kk::benchmark
