#include "changelog.h"
#include "update.h"

#include <cstdlib>
#include <sstream>

namespace kk {

extern const char* const kChangelogText;  // generated from CHANGELOG.md by the build

int CompareVersions(const std::string& a, const std::string& b) {
  // The updater's order (update.h): "2.0.0-beta.1" comes after 1.9.11 and before 2.0.0-beta.2 and 2.0.0.
  // Splitting on '.' alone read it as 2.0.0.1, after 2.0.0.
  if (IsNewerVersion(a, b)) return 1;
  if (IsNewerVersion(b, a)) return -1;
  return 0;
}

const std::vector<ChangelogEntry>& Changelog() {
  static const std::vector<ChangelogEntry> entries = [] {
    std::vector<ChangelogEntry> out;
    std::stringstream in(kChangelogText);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("## v", 0) == 0) {  // "## v1.6.0 (7 October 2026)"
        ChangelogEntry e;
        const std::string rest = line.substr(4);
        const size_t paren = rest.find(" (");
        e.version = rest.substr(0, paren);
        if (paren != std::string::npos) e.date = rest.substr(paren + 2, rest.size() - paren - 3);
        out.push_back(std::move(e));
      } else if (!out.empty()) {
        out.back().body += line + "\n";
      }
    }
    for (auto& e : out) {  // trim blank lines around each version's notes
      while (!e.body.empty() && e.body.front() == '\n') e.body.erase(0, 1);
      while (!e.body.empty() && e.body.back() == '\n') e.body.pop_back();
    }
    return out;
  }();
  return entries;
}

}  // namespace kk
