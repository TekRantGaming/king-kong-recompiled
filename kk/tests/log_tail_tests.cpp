// Task 197 (Beta 7 uploads): the crash/hang report's log tail. The fixed ring of the last 200 log lines
// (order, wrap, long lines, writing it out as the crash handler does) and the redaction each line gets
// before it is kept (user names and personal folders, as for uploads, also when a line is cut). Also checks
// that a crash report with a full tail of long lines stays inside the upload limit (256 KB gzip).
// Usage: kk-log-tail-tests
#include "log_tail_logic.h"

#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace kk;
using kk::telemetry::Private;

namespace {
int failures = 0;
void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
void Same(const std::string& got, const std::string& want, const std::string& what) {
  Check(got == want, what + ": got \"" + got + "\", want \"" + want + "\"");
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

Private Personal() {
  Private p;
  p.names = {"PrivacyProbe", "DESKTOP-PROBE7"};
  p.homes = {"C:\\Users\\PrivacyProbe", "/home/probe"};
  return p;
}

void Ring() {
  auto tail = std::make_unique<LogTail<>>();
  Check(tail->Count() == 0 && tail->Text().empty(), "empty at start");
  tail->Add("first");
  tail->Add("second");
  Same(tail->Text(), "first\nsecond\n", "oldest first");
  for (int i = 0; i < 450; ++i) tail->Add("line " + std::to_string(i));
  Check(tail->Count() == 200, "keeps 200 lines");
  std::vector<std::string> lines;
  tail->ForEach([&](const char* t, size_t n) { lines.emplace_back(t, n); });
  Check(lines.size() == 200 && lines.front() == "line 250" && lines.back() == "line 449",
        "after wrapping: the newest 200, oldest first");
  tail->Add(std::string(2000, 'x'));
  lines.clear();
  tail->ForEach([&](const char* t, size_t n) { lines.emplace_back(t, n); });
  Check(lines.back().size() == 511 && lines.front() == "line 251", "a long line is cut to 511 bytes");

  // A small ring wraps the same way.
  LogTail<3, 8> small;
  for (const char* s : {"a", "b", "c", "d", "0123456789"}) small.Add(s);
  Same(small.Text(), "c\nd\n0123456\n", "3-line ring, 8-byte slots");

  // Many writers at once: nothing lost from the count, every kept line whole.
  auto shared = std::make_unique<LogTail<>>();
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w)
    writers.emplace_back([&shared, w] {
      for (int i = 0; i < 1000; ++i) shared->Add("writer " + std::to_string(w) + " line " + std::to_string(i));
    });
  for (auto& t : writers) t.join();
  lines.clear();
  shared->ForEach([&](const char* t, size_t n) { lines.emplace_back(t, n); });
  bool whole = lines.size() == 200;
  for (const auto& l : lines) whole &= l.rfind("writer ", 0) == 0 && Has(l, " line ");
  Check(whole, "four writers: 200 whole lines kept");

  // A held lock (a crash while a line is being copied) does not stop the crash handler reading.
  struct Held : LogTail<4, 16> {
    void Hold() { Lock(); }
    void Release() { Unlock(); }
  };
  auto held = std::make_unique<Held>();
  held->Add("kept");
  held->Hold();
  std::string seen;
  const size_t read = held->ForEach([&](const char* t, size_t n) { seen.assign(t, n); }, 8);
  held->Release();
  Check(read == 1 && seen == "kept", "a held lock: the crash handler still reads the ring");
  held->Add("after");
  Same(held->Text(), "kept\nafter\n", "the lock works again after release");
}

void Redaction() {
  const Private p = Personal();
  Same(RedactLogLine("[12:00:01] [info] KK: settings C:\\Users\\PrivacyProbe\\Documents\\king_kong\\x.toml\r\n", p, 511),
       "[12:00:01] [info] KK: settings %USERPROFILE%\\Documents\\king_kong\\x.toml", "own Windows profile");
  Same(RedactLogLine("open D:\\Users\\SomeoneElse\\a.txt", p, 511), "open %USERPROFILE%\\a.txt", "another profile");
  Same(RedactLogLine("cache /home/probe/.local/share/kk and /home/other/x", p, 511),
       "cache ~/.local/share/kk and ~/x", "Linux homes");
  Same(RedactLogLine("host DESKTOP-PROBE7 user privacyprobe", p, 511), "host <user> user <user>", "names in any case");
  Same(RedactLogLine("a\nb", p, 511), "a b", "no line breaks inside a line");
  Same(RedactLogLine("short", p, 3), "sho", "cut to the size");

  // A name cut in half at the end of a long line never survives (the margin is dropped).
  for (size_t at = 480; at < 520; ++at) {
    std::string line(at, '.');
    line += "PrivacyProbe and more text after it to make the line long enough to cut";
    const std::string got = RedactLogLine(line, p, 500);
    Check(got.size() <= 500 && !Has(got, "Privacy") && !Has(got, "Probe") && !Has(got, "Priv"),
          "a cut name never survives (at " + std::to_string(at) + ")");
  }
  for (size_t at = 470; at < 520; ++at) {
    std::string line(at, '.');
    line += "C:\\Users\\PrivacyProbe\\Desktop\\file.txt plus more text so the line is long";
    const std::string got = RedactLogLine(line, p, 500);
    Check(!Has(got, "Users\\Priv") && !Has(got, "Privacy"), "a cut profile path never survives (at " + std::to_string(at) + ")");
  }
}

// The crash handler's way of writing the tail (diagnostics.cpp WriteLogTail) into a real file.
void WriteOut() {
  auto tail = std::make_unique<LogTail<>>();
  const Private p = Personal();
  tail->Add(RedactLogLine("[error] KK dev: forced crash; test path C:\\Users\\PrivacyProbe\\private.txt", p, 511));
  FILE* f = std::tmpfile();
  Check(f != nullptr, "temporary file");
  if (!f) return;
  tail->ForEach([f](const char* t, size_t n) {
    std::fputs("  ", f);
    std::fwrite(t, 1, n, f);
    std::fputs("\n", f);
  });
  std::rewind(f);
  char buffer[1024] = {};
  const size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, f);
  std::fclose(f);
  Same(std::string(buffer, got), "  [error] KK dev: forced crash; test path %USERPROFILE%\\private.txt\n",
       "written as the crash handler does");
}

// A crash report with a full tail of 200 long, varied lines: its upload stays inside 256 KB gzip.
void UploadSize() {
  auto tail = std::make_unique<LogTail<>>();
  uint32_t seed = 12345;
  for (int i = 0; i < 300; ++i) {
    std::string line = "[2026-10-17 12:00:00.000] [info] ";
    for (int k = 0; k < 600; ++k) {
      seed = seed * 1103515245u + 12345u;
      line += char(33 + (seed >> 16) % 90);
    }
    tail->Add(RedactLogLine(line, Personal(), 511));
  }
  std::string report = "King Kong crashed.\nException 0xE0430001\nStack:\n  king_kong.exe + 0x1234\n\n";
  report += std::string(32768, 'd') + "\n";
  report += "Last log lines (oldest first; user names and personal folders removed):\n";
  tail->ForEach([&](const char* t, size_t n) { report.append("  ").append(t, n) += '\n'; });
  const auto gz = telemetry::CrashGzip(telemetry::RedactForUpload(report, Personal()));
  Check(!gz.empty() && gz.size() <= telemetry::kMaxCrashGzip, "report with a full tail fits the upload limit");
  Check(report.size() > 100000, "the tail is all there (" + std::to_string(report.size()) + " bytes)");
}
}  // namespace

int main() {
  Ring();
  Redaction();
  WriteOut();
  UploadSize();
  if (failures) {
    std::printf("%d log tail check(s) failed\n", failures);
    return 1;
  }
  std::printf("log tail: all checks passed\n");
  return 0;
}
