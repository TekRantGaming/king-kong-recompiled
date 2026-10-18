// Task 229: the evidence left when the program ends without a word (exit_evidence_logic.h): the session
// marker, Windows' Application log events (crash / hang) for our process, and the rate-limited focus lines.
#include "exit_evidence_logic.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
}  // namespace

int main() {
  using namespace kk::exit_evidence;

  // Marker round trip.
  SessionMarker m;
  m.pid = 22392;
  m.version = "2.0.0-beta.9";
  m.role = "launcher";
  m.state = "running";
  m.started = 1000;
  m.alive = 1000 + 25 * 60;
  m.frame = 1000 + 25 * 60 - 10;
  SessionMarker back;
  Check(ParseMarker(FormatMarker(m), back), "marker parses");
  Check(back.pid == m.pid && back.version == m.version && back.role == m.role && back.state == m.state &&
            back.started == m.started && back.alive == m.alive && back.frame == m.frame,
        "marker round trip");
  Check(EndedWithoutWord(back), "running marker = ended without a word");
  back.state = "clean: quit";
  Check(!EndedWithoutWord(back), "clean marker");
  // CRLF and junk lines; a torn file (no state) is ignored.
  Check(ParseMarker("pid=5\r\nstate=running\r\nnonsense\r\n", back) && back.pid == 5, "CRLF marker");
  Check(!ParseMarker("pid=5\nversion=x\n", back), "no state: not a marker");
  Check(!ParseMarker("pid=abc\nstate=running\n", back), "bad pid: not a marker");
  Check(!ParseMarker("", back), "empty file");

  // The log line.
  std::string line = DescribeUncleanEnd(m, m.alive + 300);
  Check(Has(line, "previous launcher (v2.0.0-beta.9, process 22392) ended without a clean exit"), "line: who");
  Check(Has(line, "ran 25 min 0 s"), "line: how long");
  Check(Has(line, "last seen alive 5 min 0 s before this start"), "line: last alive");
  Check(Has(line, "still drawing frames"), "line: drawing");
  m.frame = m.alive - 600;
  line = DescribeUncleanEnd(m, m.alive + 300);
  Check(Has(line, "drawn no frame for 10 min 0 s"), "line: stuck or hidden");
  m.frame = 0;
  Check(Has(DescribeUncleanEnd(m, m.alive), "no frame drawn"), "line: no frame");
  Check(Duration(3725) == "1 h 2 min", "duration hours");

  // Application Error (1000), Windows 11 form with names. Paths must never come out.
  const std::string crash =
      "<Event><System><Provider Name='Application Error' Guid='{a}'/><EventID>1000</EventID></System>"
      "<EventData><Data Name='AppName'>king_kong.exe</Data><Data Name='AppVersion'>0.0.0.0</Data>"
      "<Data Name='AppTimeStamp'>6712a0b0</Data><Data Name='ModuleName'>owclient.dll</Data>"
      "<Data Name='ModuleVersion'>0.311.0.6</Data><Data Name='ModuleTimeStamp'>1</Data>"
      "<Data Name='ExceptionCode'>c0000409</Data><Data Name='FaultingOffset'>00000000001a2b3c</Data>"
      "<Data Name='ProcessId'>0x5778</Data><Data Name='ProcessCreationTime'>0x1</Data>"
      "<Data Name='AppPath'>C:\\Users\\Someone\\KingKong\\king_kong.exe</Data>"
      "<Data Name='ModulePath'>C:\\Program Files (x86)\\Overwolf\\owclient.dll</Data></EventData></Event>";
  std::string d = DescribeEndEvent(crash, 0x5778, "king_kong.exe");
  Check(Has(d, "exception 0xc0000409 in owclient.dll at offset 0x00000000001a2b3c"), "1000: code, module, offset");
  Check(Has(d, "fail-fast"), "1000: fail-fast explained");
  Check(!Has(d, "Users") && !Has(d, "Program Files"), "1000: no paths");
  Check(DescribeEndEvent(crash, 0x5779, "king_kong.exe").empty(), "1000: other process ignored");
  Check(DescribeEndEvent(crash, 0x5778, "other.exe").empty(), "1000: other program ignored");
  // Older form: no names, same order; module given as a path.
  const std::string old =
      "<Event><System><Provider Name=\"Application Error\"/></System><EventData><Data>King_Kong.exe</Data>"
      "<Data>1</Data><Data>2</Data><Data>C:\\x\\nvoglv64.dll</Data><Data>4</Data><Data>5</Data>"
      "<Data>c0000005</Data><Data>0000abcd</Data><Data>5778</Data></EventData></Event>";
  d = DescribeEndEvent(old, 0x5778, "king_kong.exe");
  Check(Has(d, "0xc0000005 in nvoglv64.dll") && Has(d, "access violation") && !Has(d, "C:\\"), "1000 old form");
  // Application Hang (1002): ended by Windows / the player after it stopped responding.
  const std::string hang =
      "<Event><System><Provider Name='Application Hang'/><EventID Qualifiers='0'>1002</EventID></System>"
      "<EventData><Data>king_kong.exe</Data><Data>0.0.0.0</Data><Data>5778</Data><Data>01db</Data>"
      "<Data>4294967295</Data><Data>C:\\Users\\Someone\\king_kong.exe</Data></EventData></Event>";
  d = DescribeEndEvent(hang, 0x5778, "king_kong.exe");
  Check(Has(d, "stopped responding") && !Has(d, "Someone"), "1002 hang");
  Check(DescribeEndEvent("<Event><System><Provider Name='Other'/></System></Event>", 1, "king_kong.exe").empty(),
        "other provider ignored");
  Check(ParseEventData("<EventData><Data Name='a'/><DataItem>x</DataItem><Data>b</Data></EventData>").size() == 2,
        "empty Data and DataItem");
  Check(ParsePid("0x1F", false) == 31 && ParsePid("1F", true) == 31 && ParsePid("31", false) == 31 &&
            ParsePid("zz", true) == 0 && ParsePid("", true) == 0,
        "pid parsing");

  // Focus losses: the first 12 changes logged, then a summary once a minute.
  FocusLossLog f;
  int logged = 0;
  int64_t t = 0, len = 0;
  for (int i = 0; i < 40; ++i) {  // a 33 ms loss every 5.5 s, as in the player's log
    if (f.Lost(t)) ++logged;
    if (f.Gained(t + 33, len)) ++logged;
    Check(len == 33, "loss length");
    t += 5500;
    const std::string s = f.Summary(t);
    if (!s.empty()) {
      Check(Has(s, "each 33-33 ms"), "summary lengths");
      ++logged;
    }
  }
  Check(logged >= 12 && logged <= 16, "rate limited (12 lines + about 3 summaries in 220 s)");
  FocusLossLog g;
  Check(g.Gained(10, len) && len == -1, "gain without loss");

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("exit evidence: all checks passed\n");
  return 0;
}
