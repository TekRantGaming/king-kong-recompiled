// The port's changelog (CHANGELOG.md at the repository root, built into the
// program): shown by the launcher on the About page and, after an update, in
// a "What's new" pop-up.

#pragma once

#include <string>
#include <vector>

namespace kk {

struct ChangelogEntry {
  std::string version;  // "1.6.0"
  std::string date;     // "7 October 2026"
  std::string body;     // the version's notes (markdown: ### headings, - bullets)
};

// Every version, newest first.
const std::vector<ChangelogEntry>& Changelog();

// Compares "1.6.0"-style versions: <0, 0 or >0.
int CompareVersions(const std::string& a, const std::string& b);

}  // namespace kk
