#pragma once
// Task 229: evidence when the program ends without a word (a player's launcher closed after ~25 minutes with
// no crash file and nothing in the log). Pure logic, no system calls (tests: kk/tests/exit_evidence_tests.cpp).
//
// - SessionMarker: logs\session-<role>.txt, rewritten every 30 s while the process lives and marked "clean"
//   on a clean exit. Still "running" at the next start (and that process gone): the previous run ended
//   without a word; the next start logs when it was last alive and when it last drew a frame.
// - Windows' own record of the end (Application log: "Application Error" 1000, "Application Hang" 1002):
//   ParseEventData / DescribeEndEvent pick the exception code, faulting module NAME (never a path) and
//   offset for that process id.
// - FocusLossLog: the launcher's "window not active" lines, rate limited (a player's log had one pair every
//   5.5 s), and how long each loss lasted.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kk::exit_evidence {

struct SessionMarker {
  uint32_t pid = 0;
  std::string version;
  std::string role;     // "launcher" or "game"
  std::string state;    // "running", "clean: <how>", "ending: <why>"
  int64_t started = 0;  // Unix seconds
  int64_t alive = 0;    // last rewrite (every 30 s)
  int64_t frame = 0;    // last frame drawn (launcher draw or guest frame); 0: none yet
};

inline std::string FormatMarker(const SessionMarker& m) {
  std::string s;
  s += "pid=" + std::to_string(m.pid) + "\n";
  s += "version=" + m.version + "\n";
  s += "role=" + m.role + "\n";
  s += "state=" + m.state + "\n";
  s += "started=" + std::to_string(m.started) + "\n";
  s += "alive=" + std::to_string(m.alive) + "\n";
  s += "frame=" + std::to_string(m.frame) + "\n";
  return s;
}

inline bool ParseMarker(std::string_view text, SessionMarker& out) {
  SessionMarker m;
  bool have_pid = false, have_state = false;
  while (!text.empty()) {
    const size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string_view key = line.substr(0, eq), value = line.substr(eq + 1);
    auto number = [&](int64_t& to) {
      int64_t v = 0;
      bool any = false;
      for (const char c : value) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
        any = true;
      }
      if (any) to = v;
      return any;
    };
    int64_t n = 0;
    if (key == "pid" && number(n)) {
      m.pid = uint32_t(n);
      have_pid = true;
    } else if (key == "version") m.version = value;
    else if (key == "role") m.role = value;
    else if (key == "state") {
      m.state = value;
      have_state = true;
    } else if (key == "started") number(m.started);
    else if (key == "alive") number(m.alive);
    else if (key == "frame") number(m.frame);
  }
  if (!have_pid || !have_state || m.pid == 0) return false;
  out = m;
  return true;
}

// The previous run ended without a clean exit (and its process is gone).
inline bool EndedWithoutWord(const SessionMarker& m) { return m.state == "running"; }

// "12 min 5 s"
inline std::string Duration(int64_t seconds) {
  if (seconds < 0) seconds = 0;
  if (seconds < 60) return std::to_string(seconds) + " s";
  if (seconds < 3600) return std::to_string(seconds / 60) + " min " + std::to_string(seconds % 60) + " s";
  return std::to_string(seconds / 3600) + " h " + std::to_string((seconds % 3600) / 60) + " min";
}

// One log line about a previous run that ended without a word. `now`: Unix seconds of this start.
inline std::string DescribeUncleanEnd(const SessionMarker& m, int64_t now) {
  std::string s = "KK: the previous " + (m.role.empty() ? std::string("run") : m.role) + " (v" + m.version +
                  ", process " + std::to_string(m.pid) + ") ended without a clean exit or a crash report";
  if (m.started && m.alive >= m.started) s += "; it ran " + Duration(m.alive - m.started) + " or a little more";
  if (m.alive && now >= m.alive) s += ", last seen alive " + Duration(now - m.alive) + " before this start";
  if (m.frame && m.alive >= m.frame) {
    const int64_t quiet = m.alive - m.frame;
    s += quiet >= 60 ? ", and it had drawn no frame for " + Duration(quiet) + " before that (stuck, minimised or hidden)"
                     : ", still drawing frames then";
  } else if (!m.frame) {
    s += ", no frame drawn";
  }
  return s;
}

// ------------------------------------------------------- Windows event log ---
// The <Data> elements of an event's XML (EvtRender), in order: {Name attribute or "", value}.
inline std::vector<std::pair<std::string, std::string>> ParseEventData(std::string_view xml) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t at = 0;
  for (;;) {
    const size_t open = xml.find("<Data", at);
    if (open == std::string_view::npos) break;
    const char after = open + 5 < xml.size() ? xml[open + 5] : '\0';
    if (after != '>' && after != ' ' && after != '/') {  // <DataItem> and the like
      at = open + 5;
      continue;
    }
    const size_t tag_end = xml.find('>', open);
    if (tag_end == std::string_view::npos) break;
    const std::string_view tag = xml.substr(open, tag_end - open);
    std::string name;
    if (const size_t n = tag.find("Name=\""); n != std::string_view::npos) {
      const size_t q = tag.find('"', n + 6);
      if (q != std::string_view::npos) name = tag.substr(n + 6, q - n - 6);
    }
    if (tag_end > 0 && xml[tag_end - 1] == '/') {  // <Data Name="x"/>
      out.emplace_back(name, "");
      at = tag_end + 1;
      continue;
    }
    const size_t close = xml.find("</Data>", tag_end);
    if (close == std::string_view::npos) break;
    out.emplace_back(name, std::string(xml.substr(tag_end + 1, close - tag_end - 1)));
    at = close + 7;
  }
  return out;
}

// The text between <tag ...> and </tag> (first one), or the value of attribute `attr` of that tag.
inline std::string XmlValue(std::string_view xml, std::string_view tag, std::string_view attr = {}) {
  const std::string open = "<" + std::string(tag);
  size_t p = xml.find(open);
  while (p != std::string_view::npos) {
    const char c = p + open.size() < xml.size() ? xml[p + open.size()] : '\0';
    if (c == '>' || c == ' ' || c == '/') break;
    p = xml.find(open, p + 1);
  }
  if (p == std::string_view::npos) return {};
  const size_t end = xml.find('>', p);
  if (end == std::string_view::npos) return {};
  if (!attr.empty()) {
    const std::string_view head = xml.substr(p, end - p);
    const std::string key = std::string(attr) + "='";
    const std::string key2 = std::string(attr) + "=\"";
    for (const std::string& k : {key, key2}) {
      const size_t a = head.find(k);
      if (a == std::string_view::npos) continue;
      const size_t q = head.find(k.back(), a + k.size());
      if (q != std::string_view::npos) return std::string(head.substr(a + k.size(), q - a - k.size()));
    }
    return {};
  }
  const size_t close = xml.find("</" + std::string(tag) + ">", end);
  if (close == std::string_view::npos) return {};
  return std::string(xml.substr(end + 1, close - end - 1));
}

// "C:\\x\\owclient.dll" -> "owclient.dll" (never log a path: it can hold the player's name).
inline std::string BaseName(std::string_view path) {
  const size_t slash = path.find_last_of("\\/");
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

inline std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

// "0x5784", "5784" (hex, as the Application log writes it) or decimal "22392" -> pid; 0 if not a number.
inline uint32_t ParsePid(std::string_view s, bool hex_default) {
  bool hex = hex_default;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s.remove_prefix(2);
    hex = true;
  }
  uint64_t v = 0;
  if (s.empty()) return 0;
  for (const char c : s) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return 0;
    v = v * (hex ? 16 : 10) + uint64_t(d);
    if (v > 0xFFFFFFFFull) return 0;
  }
  return uint32_t(v);
}

// What Windows recorded about the end of process `pid` of `exe` (lower case, e.g. "king_kong.exe"), from
// one event's XML; empty when the event is about another process. Only the code, module name and offset.
inline std::string DescribeEndEvent(std::string_view xml, uint32_t pid, std::string_view exe) {
  const std::string provider = XmlValue(xml, "Provider", "Name");
  const auto data = ParseEventData(xml);
  auto named = [&](std::string_view name) -> std::string {
    for (const auto& [n, v] : data)
      if (n == name) return v;
    return {};
  };
  auto at = [&](size_t i) -> std::string { return i < data.size() ? data[i].second : std::string(); };
  if (provider == "Application Error") {
    // Event 1000. Named fields on Windows 10/11; the same order without names on older ones.
    const bool names = !named("AppName").empty();
    const std::string app = names ? named("AppName") : at(0);
    const std::string pid_text = names ? named("ProcessId") : at(8);
    if (Lower(app) != exe || ParsePid(pid_text, true) != pid) return {};
    const std::string module = BaseName(names ? named("ModuleName") : at(3));
    const std::string code = names ? named("ExceptionCode") : at(6);
    const std::string offset = names ? named("FaultingOffset") : at(7);
    std::string s = "Windows recorded a crash: exception 0x" + code + " in " + module + " at offset 0x" + offset;
    const std::string c = Lower(code);
    if (c == "c0000409") s += " (fail fast: stack buffer overrun, abort() or an invalid argument)";
    else if (c == "c0000374") s += " (heap corruption)";
    else if (c == "c00000fd") s += " (stack overflow)";
    else if (c == "c0000005") s += " (access violation)";
    else if (c == "40000015") s += " (abort())";
    return s;
  }
  if (provider == "Application Hang") {
    // Event 1002: AppName, AppVersion, ProcessId (hex), StartTime, TerminationTime, ... (no names).
    const std::string app = !named("AppName").empty() ? named("AppName") : at(0);
    const std::string pid_text = !named("ProcessId").empty() ? named("ProcessId") : at(2);
    if (Lower(app) != exe || ParsePid(pid_text, true) != pid) return {};
    return "Windows recorded that the window stopped responding and the program was closed (Application Hang)";
  }
  return {};
}

// --------------------------------------------------------- focus losses ---
// Decides which "launcher controller input" changes are logged: every change for the first `kFirst`, then
// one summary a minute (count, shortest and longest loss). Times in milliseconds.
struct FocusLossLog {
  static constexpr int kFirst = 12;
  static constexpr int64_t kSummaryEveryMs = 60000;
  int changes = 0;        // changes seen
  int64_t lost_at = -1;   // when the current loss began
  int quiet_losses = 0;   // losses not logged since the last summary
  int64_t quiet_min = 0, quiet_max = 0;
  int64_t summary_at = -1;

  // The window became not active at `now`. True: log this one.
  bool Lost(int64_t now) {
    lost_at = now;
    ++changes;
    return changes <= kFirst;
  }
  // Active again at `now`. Returns the loss's length (ms, -1 if none was open) in `length` and true when this
  // change is logged (with the length).
  bool Gained(int64_t now, int64_t& length) {
    length = lost_at >= 0 ? now - lost_at : -1;
    lost_at = -1;
    ++changes;
    if (changes <= kFirst) return true;
    if (length >= 0) {
      quiet_min = quiet_losses ? std::min(quiet_min, length) : length;
      quiet_max = quiet_losses ? std::max(quiet_max, length) : length;
      ++quiet_losses;
    }
    return false;
  }
  // Once a minute after the first kFirst changes: a summary to log (empty: nothing).
  std::string Summary(int64_t now) {
    if (changes <= kFirst) return {};
    if (summary_at < 0) summary_at = now;
    if (now - summary_at < kSummaryEveryMs || quiet_losses == 0) return {};
    char buf[200];
    std::snprintf(buf, sizeof buf,
                  "KK: launcher window lost the foreground %d more times in the last %lld s (each %lld-%lld ms)",
                  quiet_losses, (long long)((now - summary_at) / 1000), (long long)quiet_min, (long long)quiet_max);
    summary_at = now;
    quiet_losses = 0;
    return buf;
  }
};

}  // namespace kk::exit_evidence
