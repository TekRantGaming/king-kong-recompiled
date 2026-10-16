// This file is UTF-8.
#include "launcher_benchmark.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "launcher_text.h"

namespace kk::bench_ui {
namespace {
using text::F;
using text::T;

std::string ReadSmall(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > 64 * 1024 * 1024) return {};
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

std::map<std::string, std::string> ReadInfo(const std::string& text) {
  std::map<std::string, std::string> out;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (const auto eq = line.find('='); eq != std::string::npos && eq > 0) out[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return out;
}

// Finished runs (with status.txt), newest first.
std::vector<std::filesystem::path> Runs(const std::filesystem::path& user_dir) {
  std::vector<std::filesystem::path> runs;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(user_dir / "benchmarks", ec), end; !ec && it != end; it.increment(ec)) {
    const auto name = it->path().filename().string();
    std::error_code file_ec;
    if (name.starts_with("run-") && std::filesystem::is_regular_file(it->path() / "status.txt", file_ec))
      runs.push_back(it->path());
  }
  // run-<microseconds>: compare by length, then text, so the newest comes first.
  std::sort(runs.begin(), runs.end(), [](const auto& a, const auto& b) {
    const auto x = a.filename().string(), y = b.filename().string();
    return x.size() != y.size() ? x.size() > y.size() : x > y;
  });
  return runs;
}

std::string Num(double v, int decimals = 1) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  // The launcher's languages other than English write a decimal comma.
  std::string s = buf;
  if (text::Current() != text::Lang::kEnglish)
    for (char& c : s) if (c == '.') c = ',';
  return s;
}

const char* ChapterTitle(int chapter) {
  switch (chapter) {
    case 1: return "Necropolis";
    case 2: return "V-Rex";
    case 3: return "Kong";
    case 4: return "Skull Island coast";
    default: return "Whole route";
  }
}
}  // namespace

Page StartPage(bool have_results) {
  Page p;
  p.title = "Benchmark";
  p.body = "Plays a recorded route through four chapters with your graphics settings and private saves. "
           "A standard run plays it three times (about 11 minutes) and reports the middle result of each "
           "chapter with how close the passes were. A quick run plays it once (about 4 minutes). Close other "
           "programs first. To cancel during the run, press B, Back or Start on the controller, or Esc.";
  p.buttons = {"Standard run", "Quick run"};
  if (have_results) p.buttons.push_back("Last results");
  p.buttons.push_back("Back");
  p.cancel = int(p.buttons.size()) - 1;
  p.width = 680;
  return p;
}

StartChoice StartChoiceFor(int button, bool have_results) {
  if (button == 0) return StartChoice::kStandard;
  if (button == 1) return StartChoice::kQuick;
  if (button == 2 && have_results) return StartChoice::kResults;
  return StartChoice::kBack;
}

bool HasResults(const std::filesystem::path& user_dir) { return !Runs(user_dir).empty(); }

Result LoadNewest(const std::filesystem::path& user_dir) {
  Result r;
  const auto runs = Runs(user_dir);
  if (runs.empty()) return r;
  r.dir = runs.front();
  r.info = ReadInfo(ReadSmall(r.dir / "status.txt"));
  r.rows = benchmark::ReadPassesCsv(ReadSmall(r.dir / "passes.csv"));
  r.report = ReadSmall(r.dir / "results.txt");
  r.found = !r.info.empty();
  for (size_t i = 1; i < runs.size(); ++i) {
    const auto info = ReadInfo(ReadSmall(runs[i] / "status.txt"));
    if (info.count("status") && info.at("status") != "cancelled" && info.at("status") != "interrupted") {
      r.previous_fps = benchmark::SummarizePasses(benchmark::ReadPassesCsv(ReadSmall(runs[i] / "passes.csv")), 0)
                           .median.average_fps;
      break;
    }
  }
  return r;
}

Page ResultsPage(const Result& r) {
  Page p;
  p.title = "Benchmark results";
  p.buttons = {"Close", "Run again", "Open results folder", "Copy results"};
  p.cancel = 0;
  p.width = 820;
  if (!r.found) {
    p.body = "No results were saved. The run may have been closed before it finished.";
    p.buttons = {"Close", "Run again"};
    return p;
  }
  const auto get = [&](const char* key) { const auto it = r.info.find(key); return it == r.info.end() ? std::string() : it->second; };
  const std::string status = get("status");
  if (status == "cancelled") p.body = "You cancelled the run. Chapters it finished are shown, but do not compare them.";
  else if (status == "interrupted") p.body = "The run was interrupted (paused, F8 or the game left play). Do not compare these results.";
  else if (status == "drift") p.body = "The replay drifted from the recorded route. Compare these results with care.";
  else p.body = "Finished. Each figure is the middle result of the passes.";

  std::ostringstream out;
  size_t busy = 0;
  for (const auto& row : r.rows) busy += row.chapter == 0 && row.Busy();
  if (busy) out << T("Other programs were busy during some passes. Close them and run it again for steadier results.") << "\n\n";
  for (int scope = 0; scope <= int(benchmark::kRouteChapters); ++scope) {
    const auto s = benchmark::SummarizePasses(r.rows, scope);
    if (!s.passes) continue;
    out << "### " << T(scope ? ChapterTitle(scope) : "Whole route") << "\n";
    out << "- " << F("Average {0} FPS, 1% low {1} FPS, worst frame {2} ms",
                     {Num(s.median.average_fps), Num(s.median.low_fps), Num(s.median.worst_ms, 0)}) << "\n";
    if (s.passes > 1)
      out << "- " << F("{0} passes, {1} to {2} FPS (spread {3}%). Confidence: {4}",
                       {std::to_string(s.passes), Num(s.min_fps), Num(s.max_fps), Num(s.spread),
                        T(benchmark::ConfidenceLabel(s.confidence))}) << "\n";
    else
      out << "- " << T("One pass: run a standard run to see how steady the result is.") << "\n";
    if (scope == 0 && r.previous_fps > 0)
      out << "- " << F("Previous run: {0} FPS ({1}%)",
                       {Num(r.previous_fps), (s.median.average_fps >= r.previous_fps ? "+" : "") +
                                                 Num((s.median.average_fps / r.previous_fps - 1) * 100)}) << "\n";
  }
  out << "### " << T("Settings") << "\n";
  out << "- " << F("{0}, render {1}, upscaler {2}, preset {3}, frame cap {4}",
                   {get("api"), get("render"), get("upscaler"), get("preset"),
                    get("cap") == "0" ? std::string(T("Off")) : get("cap")}) << "\n";
  out << "- " << F("The first {0} seconds of each chapter are played but not measured.", {Num(std::atof(get("settle").c_str()), 0)})
      << "\n";
  out << "- " << T("Results stay on this PC.") << "\n";
  p.reader = out.str();
  return p;
}

ResultChoice ResultChoiceFor(int button) {
  switch (button) {
    case 1: return ResultChoice::kRunAgain;
    case 2: return ResultChoice::kOpenFolder;
    case 3: return ResultChoice::kCopy;
    default: return ResultChoice::kClose;
  }
}

}  // namespace kk::bench_ui
