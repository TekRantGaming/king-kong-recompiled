#include "benchmark.h"
#include "benchmark_monitor.h"
#include "benchmark_history.h"
#include "graphics_panel_style.h"
#include "benchmark_data.h"
#include "benchmark_writer.h"
#include "benchmark_route.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
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

namespace kk::benchmark {
namespace {
using Clock = std::chrono::steady_clock;
enum class Phase { kOff, kLoading, kRunning, kSaving, kComplete };
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
Monitor monitor;
MonitorView final_monitor;
std::string summary_csv, previous_note;
std::array<Stats, 5> previous_stats;
std::array<Values, 5> previous_means{MissingValues(), MissingValues(), MissingValues(), MissingValues(), MissingValues()};
bool previous_has_summary = false;
bool monitoring = true;
bool have_previous = false;
std::atomic<int64_t> frame_start_ns{0};
bool observer_available = false;
ResultWriter writer;
bool recording_route = false;
Recording recording;
std::atomic<size_t> chapter_index{0};
uint64_t game_frame = 0;
bool advance_chapter = false, returned = false;
double checkpoint_at = 0;
RouteReplay replay;
std::string route_id;
std::array<std::vector<double>, 4> chapter_guest, chapter_host;
std::array<Stats, 4> chapter_stats;
std::array<std::vector<float>, 4> chapter_graphs;
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
  draw(total, 0, "total (loading excluded)");
  for (size_t c = 0; c < recording.chapters.size(); ++c) {
    draw(chapters[c], c + 1, "Chapter entry " + std::to_string(recording.chapters[c].entry));
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
      "native_ao_method", "native_ao_quality", "native_ao_samples", "ao_mode", "ao_strength", "native_gi", "native_gi_distance",
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
    drift_csv << chapter_index.load() + 1 << ',' << c.seconds << ',' << seconds << ','
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
    drift_csv << chapter_index.load() + 1 << ',' << c.seconds << ',' << game_seconds
        << ",0," << game_seconds.load() - c.seconds << ",0," << checkpoint_seconds[next_checkpoint - 1]
        << ",0,0,0,0,0,0\n";
  }
}

void Finish() {
  // Called by the game thread after the last measured swap. IO and sorting are
  // outside the measured interval and off the game/presenter threads.
  EndReplayChapter();
  writer.Start([] {
    try {
      monitor.Chapter(-1);
      monitor.Stop();
      final_monitor = monitor.View();
      std::vector<double> guest, host;
      {
        std::lock_guard lock(mutex);
        guest.swap(guest_times);
        host.swap(host_times);
      }
      const Stats gs = Summarize(guest), hs = Summarize(host);
      std::string text = "King Kong recorded-route benchmark v2\n";
      text += interrupted ? "Status: interrupted (do not compare)\n" : drift_detected ?
          "Status: complete with route drift (do not compare)\n" : checkpoints_incomplete ?
          "Status: complete (route verification incomplete)\n" : "Status: complete\n";
      text += settings;
      text += "Recording build: " + recording.build + "\nRecording ID (FNV-1a): " + route_id + '\n';
      text += "No confirmed game RNG seed API is exposed. AI, particles, collisions and load timing can vary.\n";
      text += "Replay holds the latest pad at each pre-step game time, without changing simulation dt.\n";
      text += "Jack position checkpoints: 1 unit tolerance, first second excluded. Kong position unavailable.\n";
      text += "Clock word in recording is the raw guest value at 0x82CCAE1C; alignment uses summed raw dt.\n";
      text += "Version-1 checkpoint times subtract their frame delta; live positions are sampled at that time between adjacent observations.\n";
      text += "\nGame swap intervals (includes cap and stalls):\n" + Describe(gs);
      text += observer_available ? "\nSuccessful host present return intervals (not scanout):\n" + Describe(hs)
                                : "\nHost present timing: unavailable (SDK patch 0041 required)\n";
      text += "\n1% low = reciprocal of the mean of the slowest ceil(N/100) intervals.\n"
              "Graph = peak frame time per elapsed-time bucket. CSV contains every interval.\n"
              "Compare the same route, settings, cap, API, cache and power state. AI can differ.\n";
      text = RedactDiagnostics(std::move(text));
      std::string csv = "series,frame,elapsed_seconds,frame_ms\n" + Csv(guest, "game-total") + Csv(host, "host-total");
      std::array<Stats, 4> per_stats;
      std::array<std::vector<float>, 4> per_graphs;
      for (size_t c = 0; c < recording.chapters.size(); ++c) {
        per_stats[c] = Summarize(chapter_guest[c]); per_graphs[c] = Graph(chapter_guest[c]);
        const auto& d = chapter_drift[c];
        text += "\nChapter " + std::to_string(c + 1) + " entry " + std::to_string(recording.chapters[c].entry) +
            ", requested game seconds " + std::to_string(recording.chapters[c].duration) + "\n" + Describe(per_stats[c]);
        text += "Host present returns:\n" + Describe(Summarize(chapter_host[c]));
        text += "Checkpoints compared/unavailable/mismatched: " + std::to_string(d.compared) + "/" +
            std::to_string(d.unavailable) + "/" + std::to_string(d.mismatches) +
            "\nMax position distance: " + std::to_string(d.max_distance) +
            "\nMax checkpoint time lag seconds: " + std::to_string(d.max_checkpoint_lag) +
            "\nMax input age seconds: " + std::to_string(d.max_input_age) +
            "\nPolls with skipped button/trigger transitions: " + std::to_string(d.missed_pulses) +
            "\nPolls missing recorded inputs after initial 0.25 seconds: " + std::to_string(d.missing_inputs) + '\n';
        const auto game_series = "game-chapter-" + std::to_string(c + 1), host_series = "host-chapter-" + std::to_string(c + 1);
        csv += Csv(chapter_guest[c], game_series.c_str()) + Csv(chapter_host[c], host_series.c_str());
      }
      // Machine-readable aggregate sidecar also drives previous-run comparison.
      std::ostringstream summary;
      summary.imbue(std::locale::classic());
      summary << "scope,count,seconds,average_fps,low1_fps,low01_fps,p50_ms,p95_ms,p99_ms,worst_ms,stutters";
      for (const auto* name : kMetricNames) summary << ',' << name << ",peak " << name;
      summary << ",final_fps\n";
      for (size_t c = 0; c <= recording.chapters.size(); ++c) {
        const Stats& s = c ? per_stats[c - 1] : gs;
        summary << c << ',' << s.count << ',' << std::setprecision(12) << s.seconds << ',' << s.average_fps << ','
            << s.low_fps << ',' << s.low01_fps << ',' << s.p50_ms << ',' << s.p95_ms << ',' << s.p99_ms << ',' << s.worst_ms << ',' << s.stutters;
        const auto& m = final_monitor.metrics[c];
        text += "\n" + std::string(text::T(c ? "Chapter" : "Total")) + (c ? " " + std::to_string(c) : "") + " / " + text::T("System counters") + " / " + text::T("Mean") + "; " + text::T("Peak") + ":\n";
        for (size_t i = 0; i < kMetricCount; ++i) {
          summary << ',' << m[i].Mean() << ',' << m[i].peak;
          text += std::string(text::T(kMetricNames[i])) + ": " + std::to_string(m[i].Mean()) + "; " + std::to_string(m[i].peak) + "\n";
        }
        text += std::string(text::T("Bottleneck indication")) + ": " + text::T(Bound(m, s.count ? s.seconds * 1000 / s.count : -1)) + "\n";
        summary << ',' << s.final_fps << '\n';
      }
      summary_csv = summary.str();
      for (const auto* note : {
          "Counters are sampled at 4 Hz; loading is excluded. Means are weighted by measured time. Unavailable counters are not zero.",
          "Bottleneck indication compares GPU time with the slower CPU thread. Caps and waits can affect it.",
          "Latency is the age of the latest game tick at guest swap. It is not paired-frame, display or input latency."})
        text += std::string(text::T(note)) + "\n";
      text += "Monitor worker ms: " + std::to_string(final_monitor.worker_ms) + ", samples: " + std::to_string(final_monitor.samples) + ", dropped frames: " + std::to_string(final_monitor.dropped) + "\n";
      text += "API: " + std::string(final_monitor.api == 1 ? "Vulkan" : final_monitor.api == 2 ? "DirectX 12" : text::T("Unavailable")) +
          "\n" + text::T("Render resolution") + ": " + std::to_string(final_monitor.width) + "x" + std::to_string(final_monitor.height) +
          " > " + std::to_string(final_monitor.output_width) + "x" + std::to_string(final_monitor.output_height) + "\n" + text::T("Upscaler") + ": " + final_monitor.upscaler + "\n";
      if (have_previous) {
        text += "\n" + std::string(text::T("Previous run")) + ": " + previous_note + "\n" +
            text::T("Previous settings or route may differ. Check the exported reports before comparing.") + "\n";
        for (size_t c = 0; c <= recording.chapters.size(); ++c) {
          text += std::string(text::T(c ? "Chapter" : "Total")) + " " + std::to_string(c) + "\n" + Describe(previous_stats[c]);
          for (size_t i = 0; i < kMetricCount; ++i)
            text += std::string(text::T(kMetricNames[i])) + ": " + std::to_string(previous_means[c][i]) + "\n";
        }
      }
      const bool summary_ok = WriteResult(result_dir / "summary.csv", summary_csv);
      text += "\nRecording settings:\n" + recording.settings;
      text = RedactDiagnostics(std::move(text));
      const bool txt_ok = WriteResult(result_dir / "results.txt", text);
      const bool csv_ok = WriteResult(result_dir / "frames.csv", csv);
      const bool drift_ok = WriteResult(result_dir / "drift.csv", drift_csv.str());
      const bool svg_ok = WriteResult(result_dir / "frame-times.svg", GraphSvg(chapter_guest));
      {
        std::lock_guard lock(mutex);
        report = std::move(text);
        guest_stats = gs;
        host_stats = hs;
        graph = Graph(guest);
        chapter_stats = per_stats; chapter_graphs = std::move(per_graphs);
        if (!txt_ok || !csv_ok || !drift_ok || !svg_ok || !summary_ok) save_error = "Could not save benchmark results.";
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
  if (recording_route) return;
  if (phase != Phase::kRunning) return;
  const auto now = Clock::now();
  std::lock_guard lock(mutex);
  if (phase != Phase::kRunning) return;
  if (last_host != Clock::time_point{} && host_times.size() < kMaxSamples)
  {
    const double ms = std::chrono::duration<double, std::milli>(now - last_host).count();
    host_times.push_back(ms); chapter_host[chapter_index].push_back(ms);
  }
  else if (host_times.size() >= kMaxSamples) interrupted = true;
  last_host = now;
}


std::string Value(double v, const char* format = "%.2f") {
  if (v < 0 || !std::isfinite(v)) return text::T("Unavailable");
  char buf[64]; std::snprintf(buf, sizeof(buf), format, v); return buf;
}
void Heading(const char* label) {
  ImGui::TextColored(ImColor(ui::color::kAccent), "%s", text::T(label)); ImGui::Separator();
}
void FrameReadout(const Stats& s, const Stats* previous = nullptr) {
  using text::T;
  if (ImGui::BeginTable("##frames", previous ? 3 : 2, ImGuiTableFlags_SizingStretchProp)) {
    auto row = [&](const char* label, double value, double old) {
      ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(T(label));
      ImGui::TableNextColumn(); ImGui::TextUnformatted(Value(value).c_str());
      if (previous) { ImGui::TableNextColumn(); ImGui::TextDisabled("%s: %s", T("Previous"), Value(old).c_str()); }
    };
    row("Final FPS", s.final_fps, previous && previous->final_fps > 0 ? previous->final_fps : -1);
    row("Average FPS", s.average_fps, previous ? previous->average_fps : -1);
    row("1% low FPS", s.low_fps, previous ? previous->low_fps : -1);
    row("0.1% low FPS", s.low01_fps, previous && previous->low01_fps > 0 ? previous->low01_fps : -1);
    row("Frame p50 (ms)", s.p50_ms, previous ? previous->p50_ms : -1);
    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("p95 / p99");
    ImGui::TableNextColumn(); ImGui::Text("%.2f / %.2f", s.p95_ms, s.p99_ms);
    if (previous) { ImGui::TableNextColumn(); ImGui::TextDisabled("%s / %s", Value(previous->p95_ms > 0 ? previous->p95_ms : -1).c_str(), Value(previous->p99_ms).c_str()); }
    row("Worst frame (ms)", s.worst_ms, previous ? previous->worst_ms : -1);
    row("Stutters (>50 ms)", double(s.stutters), previous && previous_has_summary ? double(previous->stutters) : -1);
    ImGui::EndTable();
  }
}
void MetricReadout(const MonitorView& v, size_t scope, bool live) {
  using text::T;
  if (ImGui::BeginTable("##metrics", live ? 2 : have_previous ? 4 : 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
    if (!live) {
      ImGui::TableSetupColumn(T("Metric")); ImGui::TableSetupColumn(T("Mean")); ImGui::TableSetupColumn(T("Peak")); if (have_previous) ImGui::TableSetupColumn(T("Previous")); ImGui::TableHeadersRow();
    }
    for (size_t i = 0; i < kMetricCount; ++i) {
      const double value = live ? v.current[i] : v.metrics[scope][i].Mean();
      if (i >= kClock && value < 0 && (!have_previous || previous_means[scope][i] < 0)) continue;
      ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(T(kMetricNames[i]));
      ImGui::TableNextColumn(); ImGui::TextUnformatted(Value(value).c_str());
      if (!live) { ImGui::TableNextColumn(); ImGui::TextUnformatted(Value(v.metrics[scope][i].peak).c_str());
        if (have_previous) { ImGui::TableNextColumn(); ImGui::TextDisabled("%s", Value(previous_means[scope][i]).c_str()); }
      }
    }
    ImGui::EndTable();
  }
}
void Configuration(const MonitorView& v) {
  using text::T;
  ImGui::TextWrapped("%s: %ux%u > %ux%u | %s", T("Render resolution"), v.width, v.height, v.output_width, v.output_height,
      v.upscaler.empty() ? T("Unavailable") : v.upscaler == "off" ? T("Off") : v.upscaler.c_str());
  const auto preset = rex::cvar::GetFlagByName("kk_render_quality");
  const char* preset_label = "Custom";
  for (const auto& item : RenderPresets()) if (item.id == preset) preset_label = item.label;
  ImGui::Text("%s: %s | API: %s", T("Preset"), T(preset_label), v.api == 1 ? "Vulkan" : v.api == 2 ? "DirectX 12" : T("Unavailable"));
}
void SystemInfo() {
  using text::T;
  std::istringstream lines(DiagnosticsHardware()); std::string line;
  while (std::getline(lines, line)) {
    if (line.starts_with("CPU: ")) {
      auto cpu = line.substr(5); if (const auto n = cpu.rfind(" ("); n != std::string::npos) cpu.resize(n);
      ImGui::TextWrapped("CPU: %s", cpu.c_str());
    } else if (line.starts_with("Windows: ")) ImGui::Text("%s: Windows %s", T("Operating system"), line.substr(9).c_str());
    else if (line.starts_with("RAM: ")) ImGui::Text("RAM: %s", line.substr(5).c_str());
    else if (line.starts_with("  ")) {
      // Hardware model names are invariant; field labels use the translation table.
      const auto split = line.find("; vendor ");
      ImGui::TextWrapped("GPU: %s", line.substr(2, split == std::string::npos ? split : split - 2).c_str());
      const auto driver = line.find("; driver ");
      if (driver != std::string::npos) ImGui::Text("%s: %s", T("Driver"), line.substr(driver + 9).c_str());
    }
  }
  ImGui::TextDisabled("%s", T("GPU inventory; the active API and render resolution are shown above."));
}
class StartDialog final : public rex::ui::ImGuiDialog {
 public:
  StartDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> start) : ImGuiDialog(drawer), start_(std::move(start)) {}
 protected:
  void OnDraw(ImGuiIO& io) override {
    using text::T;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(560.f, io.DisplaySize.x - 32), 0), ImGuiCond_Always);
    ui::PushGraphicsPanelStyle(.96f);
    ImGui::Begin((std::string(T("Benchmark")) + "###benchmark-start").c_str(), nullptr,
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    Heading("Recorded route");
    ImGui::TextWrapped("%s", T("Four chapters, three game minutes plus loading. Uses your graphics settings and private saves."));
    ImGui::Spacing();
    ImGui::TextWrapped("%s", T("The monitor shows measured frame times and system counters. Do not pause or open F8 during the run."));
    ImGui::TextWrapped("%s", T("Route drift can affect comparisons. Results stay on this PC."));
    ImGui::Separator();
#if defined(KK_DEV_TOOLS)
    if (std::getenv("KK_DEV_BENCH_UI") && Clock::now() - shown_ > std::chrono::seconds(4)) { Close(); }
#endif
    bool start = ImGui::Button(T("Start benchmark")); ImGui::SameLine();
    bool close = ImGui::Button(T("Back"));
    ImGui::End(); ui::PopGraphicsPanelStyle();
    if (start) { auto callback = std::move(start_); Close(); if (callback) callback(); }
    else if (close) Close();
  }
 private: std::function<void()> start_;
  Clock::time_point shown_ = Clock::now();
};
class Dialog final : public rex::ui::ImGuiDialog {
 public:
  Dialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> quit)
      : ImGuiDialog(drawer), quit_(std::move(quit)) {}
  bool NeedsContinuousPaint() override { return phase == Phase::kSaving || phase == Phase::kComplete; }
 protected:
  void OnDraw(ImGuiIO& io) override {
    using text::T;
    const auto p = phase.load();
    if (p == Phase::kOff) return;
    if (p == Phase::kRunning && (DebugOverlayOpen() || play::PauseMenuOpen())) Interrupted();
    const auto draw_start = Clock::now();
    int test_stage = -1, test_tab = -1;
#if defined(KK_DEV_TOOLS)
    if (std::getenv("KK_DEV_BENCH_UI")) {
      static const auto began = Clock::now();
      if (p == Phase::kComplete) {
        if (complete_at_ == Clock::time_point{}) complete_at_ = Clock::now();
        test_stage = int(std::chrono::duration<double>(Clock::now() - complete_at_).count() / 3);
        if (test_stage < 9) {
          const char* langs[] = {"1", "3", "5", "4", "6"};
          rex::cvar::SetFlagByName("user_language", test_stage < 5 ? langs[test_stage] : "1");
          test_tab = test_stage < 5 ? 0 : test_stage - 4;
        }
      } else if (p == Phase::kRunning && chapter_index == 0) {
        const auto seconds = game_seconds.load();
        const int stage = int(seconds / 4);
        const char* langs[] = {"1", "3", "5", "4", "6"};
        rex::cvar::SetFlagByName("user_language", stage < 5 ? langs[stage] : "1");
        if (stage < 5 && stage != live_stage_) { live_stage_ = stage; REXLOG_INFO("KK benchmark UI shot: live-{}", stage); }
      }
      if (test_stage >= 0 && test_stage < 9 && test_stage != result_stage_) {
        result_stage_ = test_stage; REXLOG_INFO("KK benchmark UI shot: result-{}", test_stage);
      }
    }
#endif
    const float scale = std::clamp(ImGui::GetFontSize() / 18.f, .75f, 2.f);
    const bool live = p == Phase::kRunning || p == Phase::kLoading;
#if defined(KK_DEV_TOOLS)
    // Offline/game A/B aid only: remove the entire live monitor from baseline.
    if (live && !monitoring) return;
#endif
    const MonitorView v = p == Phase::kComplete ? final_monitor : monitor.View();
    ImGui::SetNextWindowPos(live ? ImVec2(16 * scale, 16 * scale) : ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2),
        ImGuiCond_Always, live ? ImVec2(0, 0) : ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min((live ? 510.f : 960.f) * scale, io.DisplaySize.x - 32),
        std::min((live ? 660.f : 700.f) * scale, io.DisplaySize.y - 32)), ImGuiCond_Always);
    ui::PushGraphicsPanelStyle(.90f);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, ui::color::kAccent);
    ImGui::PushStyleColor(ImGuiCol_Tab, IM_COL32(40, 36, 30, 220));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, IM_COL32(72, 59, 38, 240));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, IM_COL32(90, 72, 44, 230));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ui::color::kAccent);
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, IM_COL32(40, 36, 30, 220));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, IM_COL32(90, 72, 44, 230));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, ui::color::kAccent);
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, IM_COL32(40, 36, 30, 220));
    const auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
        (live ? ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav : 0);
    ImGui::Begin((std::string(T(live ? "Benchmark" : "Benchmark results")) + "###benchmark-panel").c_str(), nullptr, flags);
    if (live) {
      if (recording_route) Heading("Recording route"); else Heading("Live performance");
      if (p == Phase::kLoading) ImGui::TextUnformatted(T("Loading benchmark..."));
      else ImGui::Text("%s %zu / %zu | %.0f / %.0f s", T("Chapter"), chapter_index.load() + 1,
          recording.chapters.size(), game_seconds.load(), recording.chapters[chapter_index].duration);
      if (!recording_route && monitoring) {
        ImGui::Text("%s: %.1f | %s: %.1f", T("Current FPS"), v.current_fps, T("Average FPS"), v.frames.average_fps);
        ImGui::Text("%s: %.1f | %s: %.1f", T("1% low FPS"), v.frames.low_fps, T("0.1% low FPS"), v.frames.low01_fps);
        ImGui::Text("p50 / p95 / p99: %.2f / %.2f / %.2f ms | %s: %zu", v.frames.p50_ms, v.frames.p95_ms, v.frames.p99_ms, T("Stutters"), v.frames.stutters);
        MetricReadout(v, 0, true);
        ImGui::TextColored(ImColor(ui::color::kAccent), "%s: %s", T("Bottleneck indication"),
            T(Bound(v.metrics[std::min<size_t>(4, chapter_index.load() + 1)],
                v.current_fps > 0 ? 1000 / v.current_fps : -1)));
        ImGui::TextUnformatted(T("Frame time (ms)"));
        if (v.recent_count) ImGui::PlotLines("##live-graph", v.recent.data(), int(v.recent_count), 0, nullptr, 0,
            std::max(40.f, *std::max_element(v.recent.begin(), v.recent.begin() + v.recent_count)), ImVec2(-1, 64 * scale));
        Configuration(v);
        ImGui::TextWrapped("%s", T("Latency is the age of the latest game tick at guest swap. It is not paired-frame, display or input latency."));
      }
    } else if (p == Phase::kSaving) ImGui::TextUnformatted(T("Saving results..."));
    else {
      std::lock_guard lock(mutex);
      const float footer = ImGui::GetFrameHeightWithSpacing() * 2.6f;
      ImGui::BeginChild("##result-content", ImVec2(0, -footer));
      if (interrupted) ImGui::TextWrapped("%s", T("Benchmark interrupted. Run it again before comparing results."));
      if (drift_detected) ImGui::TextColored(ImColor(ui::color::kWarn), "%s", T("Route drift detected. Do not compare these results."));
      if (checkpoints_incomplete) ImGui::TextWrapped("%s", T("Some chapters have no verified position checkpoints."));
      if (ImGui::BeginTabBar("##chapters")) {
        for (size_t c = 0; c <= recording.chapters.size(); ++c) {
          const std::string title = c ? std::string(T("Chapter")) + " " + std::to_string(recording.chapters[c - 1].entry + 1) : T("Total");
          if (ImGui::BeginTabItem(title.c_str(), nullptr, test_tab == int(c) ? ImGuiTabItemFlags_SetSelected : 0)) {
            const auto& s = c ? chapter_stats[c - 1] : guest_stats;
            const auto& g = c ? chapter_graphs[c - 1] : graph;
            ImGui::TextColored(ImColor(ui::color::kAccent), "%s: %s", T("Bottleneck indication"), T(Bound(v.metrics[c], s.count ? s.seconds * 1000 / s.count : -1)));
            FrameReadout(s, have_previous ? &previous_stats[c] : nullptr);
            if (!g.empty()) ImGui::PlotLines("##result-graph", g.data(), int(g.size()), 0, T("Frame time (ms)"),
                0, std::max(40.f, float(s.worst_ms)), ImVec2(-1, 90 * scale));
            Heading("System counters"); MetricReadout(v, c, false);
            if (have_previous) {
              ImGui::Text("%s: %s", T("Previous run"), previous_note.c_str());
              const auto before = previous_stats[c].average_fps;
              if (before > 0) ImGui::Text("%s: %+.2f%%", T("Average FPS change"), (s.average_fps / before - 1) * 100);
              ImGui::TextWrapped("%s", T("Previous settings or route may differ. Check the exported reports before comparing."));
            } else ImGui::TextUnformatted(T("No previous local run found."));
            ImGui::EndTabItem();
          }
        }
        ImGui::EndTabBar();
      }
      Heading("System information"); Configuration(v); SystemInfo();
      ImGui::TextWrapped("%s", T("Counters are sampled at 4 Hz; loading is excluded. Means are weighted by measured time. Unavailable counters are not zero."));
      ImGui::TextWrapped("%s", T("Bottleneck indication compares GPU time with the slower CPU thread. Caps and waits can affect it."));
      ImGui::TextWrapped("%s", T("Latency is the age of the latest game tick at guest swap. It is not paired-frame, display or input latency."));
      ImGui::TextWrapped("%s", T("Results are saved in your user data folder under benchmarks. Nothing is uploaded."));
      ImGui::TextWrapped("%s", result_dir.string().c_str());
#if defined(KK_DEV_TOOLS)
      if (test_stage >= 0 && test_stage < 5) {
        const double seconds = std::chrono::duration<double>(Clock::now() - complete_at_).count();
        ImGui::SetScrollY(std::fmod(seconds, 3.0) > 1.5 ? ImGui::GetScrollMaxY() : 0);
      }
#endif
      ImGui::EndChild();
      if (!save_error.empty()) ImGui::TextWrapped("%s", T(save_error).c_str());
      if (ImGui::Button(T("Save results"))) {
        if (WriteResult(result_dir / "results.txt", report) && WriteResult(result_dir / "summary.csv", summary_csv)) save_error.clear();
        else save_error = "Could not save benchmark results.";
      }
      ImGui::SameLine(); if (ImGui::Button(T("Copy results"))) ImGui::SetClipboardText(report.c_str());
      ImGui::SameLine(); if (ImGui::Button(T("Copy CSV"))) ImGui::SetClipboardText(summary_csv.c_str());
      ImGui::SameLine(); if (ImGui::Button(T("Quit game")) && quit_) quit_();
    }
    ImGui::End(); ImGui::PopStyleColor(9); ui::PopGraphicsPanelStyle();
    draw_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - draw_start).count(); ++draw_count_;
    if (!live && !logged_cost_) {
      logged_cost_ = true;
      REXLOG_INFO("KK benchmark UI CPU: {} draws, {:.6f} ms/draw (includes UI construction, not GPU)", draw_count_, draw_ms_ / draw_count_);
    }
  }
 private:
  std::function<void()> quit_;
  Clock::time_point complete_at_{}; int live_stage_ = -1, result_stage_ = -1;
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
    }
    drift_csv.imbue(std::locale::classic());
    drift_csv << "chapter,recorded_seconds,replay_seconds,position_distance,time_lag_seconds,available,position_seconds,recorded_x,recorded_y,recorded_z,replay_x,replay_y,replay_z\n";
  }
  // Only this user root: no global Documents/save access. Older v2 reports
  // remain readable through their existing Samples/Measured/Average fields.
  if (!recording_route) {
    std::filesystem::path newest;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root / "benchmarks", ec), end; !ec && it != end; it.increment(ec)) {
      const auto name = it->path().filename().string();
      if (name.starts_with("run-") && name > newest.filename().string() &&
          std::filesystem::is_regular_file(it->path() / "results.txt", ec)) newest = it->path();
    }
    if (!newest.empty()) {
      auto read_small = [](const std::filesystem::path& path) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > 1048576) return std::string{};
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
      };
      auto old = ReadSummary(read_small(newest / "summary.csv"));
      previous_has_summary = old.Valid();
      if (!old.Valid()) old = ReadLegacySummary(read_small(newest / "results.txt"));
      previous_stats = old.frames; previous_means = old.means;
      have_previous = old.Valid();
      previous_note = newest.filename().string();
    }
  }
  result_dir = root / "benchmarks" / ((recording_route ? "record-" : "run-") + std::to_string(stamp));
  std::filesystem::create_directories(result_dir);
  const auto private_root = result_dir / "user";
  if (!std::filesystem::create_directory(private_root))
    throw std::runtime_error("Could not create private benchmark user data");
  phase = Phase::kLoading;
  rex::cvar::ResetToDefault("kk_benchmark");
  rex::cvar::ResetToDefault("kk_benchmark_record");
  rex::cvar::ResetToDefault("kk_benchmark_route");
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
  play::SetStartRequest({play::StartKind::kChapter, 0,
      recording.chapters.front().entry, "Benchmark"});
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
  if (!recording_route && monitoring) monitor.Start();
  // Linux observer implementation remains deferred under the standing hold.
  REXLOG_INFO("KK benchmark: host present observer {}", observer_available ? "available" : "unavailable");
}

void LevelLoaded() {
  if (phase != Phase::kLoading) { if (Active()) Interrupted(); return; }
  // The SDK reads the settings file after Prepare. Reapply these before the
  // first gameplay poll as well, without changing graphics or the frame cap.
  if (!recording_route && chapter_index == 0) ApplyReplayControls();
  if (chapter_index == 0) { settings = Settings();  }
  game_seconds = 0;
  game_frame = 0;
  input_seconds = 0;
  previous_position_seconds = -1;
  replay_base = nullptr;
  checkpoint_at = 0;
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
  monitor.Chapter(int(chapter_index.load()));
  replay.Reset(chapter_index);
  while (next_checkpoint < recording.checkpoints.size() &&
      recording.checkpoints[next_checkpoint].chapter < chapter_index) {
    ++chapter_drift[recording.checkpoints[next_checkpoint].chapter].unavailable;
    ++next_checkpoint;
  }
  phase = Phase::kRunning;
  REXLOG_INFO("KK benchmark replay: chapter {} entry {}, duration {} game seconds",
      chapter_index.load() + 1, recording.chapters[chapter_index].entry, recording.chapters[chapter_index].duration);
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
  {
    std::lock_guard lock(mutex);
    if (last_guest != Clock::time_point{} && guest_times.size() < kMaxSamples) {
      const double ms = std::chrono::duration<double, std::milli>(now - last_guest).count();
      guest_times.push_back(ms); chapter_guest[chapter_index].push_back(ms);
      // Game logic and guest swaps may run on different title threads. This is
      // age of the latest observed tick, not an invented pairing of two frames.
      const auto tick_ns = frame_start_ns.load(std::memory_order_acquire);
      const auto swap_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
      if (monitoring) monitor.Record(ms, tick_ns > 0 && swap_ns >= tick_ns ? (swap_ns - tick_ns) / 1000000.0 : -1);
    }
    else if (guest_times.size() >= kMaxSamples) interrupted = true;
    last_guest = now;
  }
  if (interrupted) Finish();
  else if (game_seconds >= recording.chapters[chapter_index].duration) {
    // The final swap follows the simulation, so its state covers the last
    // recorded pre-step checkpoint without inventing an extra game frame.
    if (replay_base) ObserveReplayPosition(game_seconds, replay_base);
    EndReplayChapter();
    if (chapter_index + 1 == recording.chapters.size()) Finish();
    else { monitor.Chapter(-1); std::lock_guard lock(mutex); phase = Phase::kLoading; advance_chapter = true; }
  }
}

bool Pad(uint32_t user, uint8_t* state, uint32_t& status) {
  if (!Active() || recording_route) return false;
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
    ++chapter_index;
    return recording.chapters[chapter_index].entry;
  }
  if (recording_route && phase == Phase::kComplete && !returned) {
    std::lock_guard lock(mutex);
    if (save_error.empty()) {
      returned = true;
      play::ReturnToLauncher("benchmark route recording saved", "about");
    }
  }
  return -1;
}

void Interrupted() {
  const auto p = phase.load();
  if (p == Phase::kRunning || p == Phase::kLoading) interrupted = true;
}

void CreateBenchmarkStartDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> start) {
  new StartDialog(drawer, std::move(start));
}

void CreateBenchmarkDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> quit) {
  new Dialog(drawer, std::move(quit));
#if defined(KK_DEV_TOOLS)
  if (std::getenv("KK_DEV_BENCH_UI")) {
    new StartDialog(drawer, [] {});
    REXLOG_INFO("KK benchmark UI shot: start");
  }
#endif
}
}  // namespace kk::benchmark
