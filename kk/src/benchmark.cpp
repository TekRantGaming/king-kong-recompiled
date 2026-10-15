#include "benchmark.h"
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
      << "<rect width=\"1000\" height=\"" << height << "\" fill=\"#111820\"/>";
  auto draw = [&](const std::vector<double>& samples, size_t row, const std::string& label) {
    const auto stats = Summarize(samples);
    const auto points = Graph(samples, 900);
    const double top = 30 + row * 200, scale = std::max(40.0, stats.worst_ms);
    svg << "<text x=\"30\" y=\"" << top << "\" fill=\"white\" font-family=\"sans-serif\">"
        << label << " / peak frame ms / scale " << scale << " / horizontal: measured elapsed time</text>"
        << "<polyline fill=\"none\" stroke=\"#5ed1ae\" points=\"";
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
      "native_ao_method", "native_ao_quality", "native_ao_samples", "ao_mode", "ao_strength", "native_gi",
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
        if (!txt_ok || !csv_ok || !drift_ok || !svg_ok) save_error = "Could not save benchmark results.";
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
    if (p == Phase::kRunning &&
        (DebugOverlayOpen() || play::PauseMenuOpen())) Interrupted();
    if (recording_route && p == Phase::kRunning) {
      ImGui::SetNextWindowPos(ImVec2(12, 50), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(.8f);
      ImGui::Begin("##route-recording", nullptr, ImGuiWindowFlags_NoDecoration |
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
      ImGui::Text("Recording route: %zu / %zu", chapter_index.load() + 1, recording.chapters.size());
      ImGui::Text("%.0f / %.0f s", game_seconds.load(), recording.chapters[chapter_index].duration);
      ImGui::End();
      return;
    }
    if (p == Phase::kLoading || p == Phase::kRunning) {
      ImGui::SetNextWindowPos(ImVec2(12, 50), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(.8f);
      ImGui::Begin("##benchmark-progress", nullptr, ImGuiWindowFlags_NoDecoration |
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
      ImGui::TextUnformatted(T("Benchmark"));
      if (p == Phase::kLoading) ImGui::TextUnformatted(T("Loading benchmark..."));
      else ImGui::Text("%s %zu / %zu, %.0f / %.0f s", T("Running benchmark..."),
          chapter_index.load() + 1, recording.chapters.size(), game_seconds.load(),
          recording.chapters[chapter_index].duration);
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
      if (drift_detected) ImGui::TextWrapped("%s", T("Route drift detected. Do not compare these results."));
      if (checkpoints_incomplete) ImGui::TextWrapped("%s", T("Some chapters have no verified position checkpoints."));
      ImGui::Text("%s: %.1f", T("Average FPS"), guest_stats.average_fps);
      ImGui::Text("%s: %.1f", T("1% low FPS"), guest_stats.low_fps);
      ImGui::Text("%s: %.2f ms", T("Worst frame"), guest_stats.worst_ms);
      ImGui::TextUnformatted(T("Frame time (ms)"));
      if (!graph.empty()) ImGui::PlotLines("##frame-times", graph.data(), int(graph.size()), 0, nullptr,
          0, std::max(40.f, float(guest_stats.worst_ms)), ImVec2(-1, 100));
      for (size_t c = 0; c < recording.chapters.size(); ++c) {
        ImGui::PushID(int(c));
        if (ImGui::TreeNode("##chapter", "%s %d", T("Chapter"), recording.chapters[c].entry + 1)) {
          const auto& s = chapter_stats[c];
          ImGui::Text("%s: %.1f   %s: %.1f   %s: %.2f ms", T("Average FPS"), s.average_fps,
              T("1% low FPS"), s.low_fps, T("Worst frame"), s.worst_ms);
          const auto& g = chapter_graphs[c];
          if (!g.empty()) ImGui::PlotLines("##chapter-frames", g.data(), int(g.size()), 0, nullptr,
              0, std::max(40.f, float(s.worst_ms)), ImVec2(-1, 70));
          ImGui::TreePop();
        }
        ImGui::PopID();
      }
      ImGui::TextWrapped("%s", T("Results are saved in your user data folder under benchmarks. Nothing is uploaded."));
      ImGui::TextWrapped("%s", result_dir.string().c_str());
      if (!save_error.empty()) ImGui::TextWrapped("%s", T(save_error).c_str());
      if (ImGui::Button(T("Copy results"))) ImGui::SetClipboardText(report.c_str());
      ImGui::SameLine();
      if (ImGui::Button(T("Quit game")) && quit_) quit_();
      // The dialog shows the translated summary. Detailed route/settings and
      // hardware diagnostics stay in results.txt (and the explicit copy action).
    }
    ImGui::End();
  }
 private:
  std::function<void()> quit_;
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
void Shutdown() { writer.Shutdown(); }

std::filesystem::path Prepare(const std::filesystem::path& root) {
  // Unique directory, never an existing player save. Failure aborts startup,
  // rather than falling through to a benchmark that writes into normal saves.
  const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  recording_route = REXCVAR_GET(kk_benchmark_record);
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
  // Linux observer implementation remains deferred under the standing hold.
  REXLOG_INFO("KK benchmark: host present observer {}", observer_available ? "available" : "unavailable");
}

void LevelLoaded() {
  if (phase != Phase::kLoading) { if (Active()) Interrupted(); return; }
  // The SDK reads the settings file after Prepare. Reapply these before the
  // first gameplay poll as well, without changing graphics or the frame cap.
  if (!recording_route && chapter_index == 0) ApplyReplayControls();
  if (chapter_index == 0) settings = Settings();
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
      else { std::lock_guard lock(mutex); phase = Phase::kLoading; advance_chapter = true; }
    }
    return;
  }
  const auto now = Clock::now();
  {
    std::lock_guard lock(mutex);
    if (last_guest != Clock::time_point{} && guest_times.size() < kMaxSamples) {
      const double ms = std::chrono::duration<double, std::milli>(now - last_guest).count();
      guest_times.push_back(ms); chapter_guest[chapter_index].push_back(ms);
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
    else { std::lock_guard lock(mutex); phase = Phase::kLoading; advance_chapter = true; }
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

void CreateBenchmarkDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> quit) {
  new Dialog(drawer, std::move(quit));
}
}  // namespace kk::benchmark
