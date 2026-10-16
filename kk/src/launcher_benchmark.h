// The benchmark's pages in the launcher: the start pop-up (a standard or a
// quick run) and the results page shown when a run comes back to the launcher.
// Both are the launcher's own pop-ups (launcher_ui.h Modal), so controller,
// keyboard and mouse all work the same way as everywhere else in it. This file
// builds their content; launcher.cpp opens them.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "benchmark_results.h"

namespace kk::bench_ui {

constexpr int kStandardPasses = 3;
constexpr int kQuickPasses = 1;

// What a pop-up shows: title and body are English (the pop-up translates them);
// `reader` is already in the launcher's language.
struct Page {
  std::string title, body, reader;
  std::vector<std::string> buttons;
  int cancel = -1;  // the button B / Esc picks
  float width = 620;
};

enum class StartChoice { kStandard, kQuick, kResults, kBack };
Page StartPage(bool have_results);
StartChoice StartChoiceFor(int button, bool have_results);

// A finished run, read back from user/benchmarks/run-<time>.
struct Result {
  bool found = false;
  std::filesystem::path dir;
  std::map<std::string, std::string> info;  // status.txt
  std::vector<benchmark::PassScope> rows;   // passes.csv
  std::string report;                       // results.txt (Copy results)
  double previous_fps = 0;                  // the run before, if it was a finished one
};
// The newest run under <user>/benchmarks (and the one before it, to compare).
Result LoadNewest(const std::filesystem::path& user_dir);
bool HasResults(const std::filesystem::path& user_dir);

enum class ResultChoice { kClose, kRunAgain, kOpenFolder, kCopy };
Page ResultsPage(const Result& result);
ResultChoice ResultChoiceFor(int button);

}  // namespace kk::bench_ui
