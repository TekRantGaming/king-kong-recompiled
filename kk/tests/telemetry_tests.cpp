// Task 194 (Beta 7 uploads, codex/state/telemetry-b7.md): redaction, gzip, the size limits, per-chapter
// statistics and the rule that nothing is sent while the Online player count switch is off. No game, no
// network: a fake sender. Writes gzip samples and a crash report's upload body into <out folder>, which
// tests/telemetry_gzip_tests.py then decompresses with Python's own gzip.
// Usage: kk-telemetry-tests <out folder>
#include "telemetry_logic.h"
#include "telemetry_upload.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <chrono>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace kk::telemetry;

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
void WriteFile(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << s;
}
std::string ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
std::string Header(const Request& r, const std::string& name) {
  for (const auto& h : r.headers)
    if (h.rfind(name + ": ", 0) == 0) return h.substr(name.size() + 2);
  return {};
}

void Redaction() {
  Private me;
  me.names = {"TekRa", "DESKTOP-K9"};
  me.homes = {"C:\\Users\\TekRa"};
  Same(RedactForUpload("log C:\\Users\\TekRa\\Documents\\king_kong\\x.log", me),
       "log %USERPROFILE%\\Documents\\king_kong\\x.log", "own profile");
  Same(RedactForUpload("c:/users/tekra/AppData", me), "%USERPROFILE%/AppData", "own profile, other case and slash");
  Same(RedactForUpload("D:\\Users\\SomeoneElse\\file.txt", {}), "%USERPROFILE%\\file.txt", "another profile");
  Same(RedactForUpload("/home/craig/.local/share/KingKong", {}), "~/.local/share/KingKong", "Linux home");
  Same(RedactForUpload("/var/home/deck/Games", {}), "~/Games", "Bazzite home");
  Same(RedactForUpload("Z:\\home\\deck\\Games", {}), "~\\Games", "Proton home");
  Same(RedactForUpload("/Users/al/Library", {}), "~/Library", "macOS home");
  Same(RedactForUpload("user TekRa on DESKTOP-K9 (tekra)", me), "user <user> on <user> (<user>)", "names anywhere");
  Same(RedactForUpload("C:\\Program Files\\KK\\king_kong.exe + 0x1A2B", me),
       "C:\\Program Files\\KK\\king_kong.exe + 0x1A2B", "other paths stay");
  Same(RedactForUpload("C:\\Users\\", me), "C:\\Users\\", "the Users folder alone");
  Private short_name;
  short_name.names = {"Al"};
  Same(RedactForUpload("Also Al went, al.", short_name), "Also <user> went, <user>.", "short names as words");
  Private elsewhere;
  elsewhere.names = {"craig"};
  elsewhere.homes = {"D:\\Profiles\\craig\\"};
  Same(RedactForUpload("D:\\Profiles\\craig\\save.dat", elsewhere), "%USERPROFILE%\\save.dat", "home not under Users");
  Private linux_home;
  linux_home.homes = {"/mnt/data/me"};
  Same(RedactForUpload("/mnt/data/me/kk.log and /mnt/data/meow", linux_home), "~/kk.log and /mnt/data/meow",
       "POSIX home, whole names only");
}

void Json() {
  Same(JsonString("a\"b\\c\n"), "\"a\\\"b\\\\c\\u000a\"", "escaping");
  Same(JsonString(std::string(200, 'x')).size() == 130 ? "ok" : "long", "ok", "strings cut to 128");
  // A cut never splits a UTF-8 character ("é" is two bytes).
  std::string accents;
  for (int i = 0; i < 100; ++i) accents += "\xC3\xA9";
  const std::string cut = JsonString(accents);
  Check(cut.size() == 2 + 128 && static_cast<unsigned char>(cut[cut.size() - 2]) == 0xA9, "UTF-8 safe cut");
  Same(JsonNumber(12.345, 1), "12.3", "number");
  Same(JsonNumber(std::nan("")), "null", "not a number");
}

void Stats() {
  ChapterStats c;
  c.entry = 10;
  for (int i = 0; i < 990; ++i) c.AddFrame(1000.0 / 60);
  for (int i = 0; i < 10; ++i) c.AddFrame(100.0);
  Check(c.frames == 1000 && c.over50 == 10, "frame and stutter count");
  Check(std::abs(c.worst_ms - 100) < 1e-9, "worst frame");
  Check(std::abs(c.OnePercentLowFps() - 10.0) < 0.1, "1% low = the slowest 10 frames: " + std::to_string(c.OnePercentLowFps()));
  const double avg = 1000.0 * 1000 / (990 * 1000.0 / 60 + 1000);
  Check(std::abs(c.AverageFps() - avg) < 0.01, "average FPS");
  ChapterStats d;
  d.entry = 10;
  d.AddFrame(300);  // over the histogram
  c.Merge(d);
  Check(c.frames == 1001 && c.worst_ms == 300 && c.long_frames == 1, "merge and long frames");
  const std::string json = c.Json();
  Check(json.find("\"entry\":10") != std::string::npos && json.find("\"stutters50ms\":11") != std::string::npos,
        "chapter JSON: " + json);
}

void Perf(const fs::path& out) {
  PerfSummary s;
  s.install_id = "0123abcd-0000-4000-8000-0000000000ff";
  s.version = "2.0.0-beta.7";
  s.api = "vulkan";
  s.gpu = std::string(300, 'G');
  s.settings = {{"preset", JsonString("High")}, {"frameCap", "60"}};
  Same(PerfJson(s), "", "no chapter, nothing to send");
  ChapterStats brief;
  brief.entry = 3;
  for (int i = 0; i < 100; ++i) brief.AddFrame(16.7);  // 1.7 s
  s.chapters.push_back(brief);
  Same(PerfJson(s), "", "a visit under 5 s is left out");
  for (int e = 0; e < 120; ++e) {
    ChapterStats c;
    c.entry = e % 42;
    for (int i = 0; i < 400 + e; ++i) c.AddFrame(16.7);
    c.gpu_ms_sum = 10, c.gpu_samples = 1;
    s.chapters.push_back(c);
  }
  const std::string json = PerfJson(s);
  size_t chapters = 0;
  for (size_t at = 0; (at = json.find("\"entry\":", at)) != std::string::npos; ++at) ++chapters;
  Check(!json.empty() && json.size() <= kMaxPerfBytes, "summary within 16 KB: " + std::to_string(json.size()));
  Check(chapters <= kMaxChapters && chapters > 0, "at most 64 chapters: " + std::to_string(chapters));
  Check(json.find(std::string(129, 'G')) == std::string::npos && json.find(std::string(128, 'G')) != std::string::npos,
        "GPU name cut to 128");
  Check(json.find("\"settings\":{\"preset\":\"High\",\"frameCap\":60}") != std::string::npos, "settings");
  Check(json.find("\\") == std::string::npos && json.find(":\\") == std::string::npos, "no paths in the summary");
  WriteFile(out / "perf.json", json);
}

void GzipSamples(const fs::path& out) {
  const std::string check = "123456789";
  Check(Crc32(reinterpret_cast<const uint8_t*>(check.data()), check.size()) == 0xCBF43926u, "CRC-32");
  const auto gz = Gzip(check);
  Check(gz.size() > 18 && gz[0] == 0x1F && gz[1] == 0x8B && gz[2] == 8, "gzip header");
  Check(gz[gz.size() - 4] == 9 && gz[gz.size() - 3] == 0, "gzip size field");
  std::mt19937 rng(194);
  std::vector<std::pair<std::string, std::string>> samples;
  samples.push_back({"empty", ""});
  samples.push_back({"short", check});
  std::string text;
  for (int i = 0; i < 3000; ++i) text += "Frame times: 59.9 FPS average, 41.0 FPS 1% low, worst 120 ms, line " + std::to_string(i) + "\n";
  samples.push_back({"text", text});
  std::string records = "KGPC";
  for (int i = 0; i < 200000; ++i) records += char(rng() % 7 == 0 ? rng() : (i * 31) & 0x3F);
  samples.push_back({"records", records});
  std::string noise;
  for (int i = 0; i < 70000; ++i) noise += char(rng());
  samples.push_back({"noise", noise});
  std::string run(100000, 'a');
  samples.push_back({"run", run});
  for (const auto& [name, data] : samples) {
    const auto z = Gzip(data);
    WriteFile(out / (name + ".orig"), data);
    WriteFile(out / (name + ".gz"), std::string(z.begin(), z.end()));
  }
  Check(Gzip(text).size() < text.size() / 4, "text compresses");
  Check(Gzip(run).size() < 1000, "a run compresses");
  // The crash limit: a text whose gzip is over 256 KB is cut until it fits.
  const auto big = CrashGzip(noise + noise + noise + noise + noise);  // 350 KB of noise
  Check(!big.empty() && big.size() <= kMaxCrashGzip, "crash gzip within 256 KB: " + std::to_string(big.size()));
}

void Uploads(const fs::path& out) {
  const fs::path root = out / "uploads";
  std::error_code ec;
  fs::remove_all(root, ec);
  std::vector<Request> sent;
  int answer = 200;
  Context c;
  c.switch_on = false;
  c.install_id = "0123abcd-0000-4000-8000-0000000000ff";
  c.version = "2.0.0-beta.7";
  c.api = "d3d12";
  c.state_dir = root / "user" / "telemetry";
  c.report_dirs = {root / "user" / "crashes", root / "logs"};
  c.pipelines_file = root / "cache" / "native-pipelines" / "555307D3.pipelines";
  c.personal.names = {"TekRa"};
  c.personal.homes = {"C:\\Users\\TekRa"};
  c.send = [&](const Request& r) {
    sent.push_back(r);
    return answer;
  };
  WriteFile(c.pipelines_file, "KGPC" + std::string(4000, 'p'));
  WriteFile(root / "user" / "crashes" / "crash-20261017-100000-000-1.txt",
            "King Kong crashed.\nException 0xC0000005 at 0000\nStack:\n  king_kong.exe + 0x1234\n"
            "King Kong PC port diagnostics\nVersion: 2.0.0-beta.7\nlog C:\\Users\\TekRa\\Documents\\king_kong\\x.log "
            "by TekRa\n");
  WriteFile(root / "user" / "crashes" / "crash-20261001-100000-000-1.txt", "King Kong crashed.\nVersion: 2.0.0-beta.6\n");
  WriteFile(root / "user" / "crashes" / "crash-20261017-100000-000-1.dmp", "MDMP");
  WriteFile(root / "logs" / "hang-20261017-090000.txt",
            "King Kong stopped drawing frames for 20 seconds (a freeze).\r\nVersion: 2.0.0-beta.7\r\nThreads:\r\n");
  WriteFile(c.state_dir / "perf-pending.json", "{\"installId\":\"x\"}");
  fs::last_write_time(root / "logs" / "hang-20261017-090000.txt",
                      fs::file_time_type::clock::now() - std::chrono::hours(1));

  // Switch off: nothing at all is sent, and nothing is written.
  Check(!Allowed(c), "switch off: not allowed");
  Check(SendPipelines(c) == 0 && SendPerf(c, "{\"a\":1}") == 0 && SendPendingPerf(c) == 0 && SendCrashReports(c, 5) == 0,
        "switch off: every upload says nothing sent");
  Check(sent.empty(), "switch off: the sender was never called");
  Check(!fs::exists(c.state_dir / "pipelines-sent.txt") && !fs::exists(c.state_dir / "reports-sent.txt") &&
            ReadFile(c.state_dir / "perf-pending.json") == "{\"installId\":\"x\"}",
        "switch off: nothing remembered or changed");
  // Nor without a proper install ID or API.
  Context bad = c;
  bad.switch_on = true;
  bad.install_id = "not-a-uuid";
  Check(!Allowed(bad), "a bad install ID: not allowed");
  bad.install_id = c.install_id;
  bad.api = "opengl";
  Check(!Allowed(bad), "an unknown API: not allowed");

  c.switch_on = true;
  Check(Allowed(c), "switch on: allowed");
  // Pipelines: gzip, the three headers; not again while unchanged; again after a 429.
  answer = 429;
  Check(SendPipelines(c) == 429 && sent.size() == 1, "pipelines sent (429)");
  answer = 200;
  Check(SendPipelines(c) == 200 && sent.size() == 2, "pipelines sent again after a 429");
  const Request& p = sent.back();
  Check(p.path == "/v1/pipelines" && p.content_type == "application/gzip", "pipelines request");
  Check(Header(p, "X-KK-Install") == c.install_id && Header(p, "X-KK-Version") == "2.0.0-beta.7" &&
            Header(p, "X-KK-Api") == "d3d12",
        "pipelines headers");
  Check(p.body.size() > 18 && uint8_t(p.body[0]) == 0x1F && uint8_t(p.body[1]) == 0x8B, "pipelines body is gzip");
  Check(SendPipelines(c) == kPipelinesUnchanged && sent.size() == 2, "unchanged pipelines are not sent again");
  c.force = true;
  Check(SendPipelines(c) == 200 && sent.size() == 3, "forced (developer aid) pipelines sent");
  c.force = false;
  WriteFile(c.pipelines_file, "KGPC" + std::string(5000, 'q'));
  Check(SendPipelines(c) == 200 && sent.size() == 4, "changed pipelines sent");
  WriteFile(c.pipelines_file, "NOPE" + std::string(5000, 'q'));
  Check(SendPipelines(c) == kNotPipelineFile && sent.size() == 4, "not a record file: not sent");
  Context missing = c;
  missing.pipelines_file = root / "nothing.pipelines";
  Check(SendPipelines(missing) == kNoPipelineFile && sent.size() == 4, "no record file: not sent");

  // Crash and hang reports: this version's, newest first, one per call; marked sent on 200.
  const auto pending = PendingReports(c);
  Check(pending.size() == 2, "two reports of this version waiting: " + std::to_string(pending.size()));
  answer = 0;
  Check(SendCrashReports(c, 1) == 1 && PendingReports(c).size() == 2, "no answer: the report waits");
  answer = 200;
  sent.clear();
  Check(SendCrashReports(c, 1) == 1 && sent.size() == 1, "one report per start");
  Check(sent[0].path == "/v1/crash" && Header(sent[0], "X-KK-Kind") == "crash" &&
            Header(sent[0], "X-KK-Install") == c.install_id && Header(sent[0], "X-KK-Api") == "d3d12",
        "crash request headers");
  WriteFile(out / "crash-body.gz", sent[0].body);
  Check(PendingReports(c).size() == 1, "the crash report is remembered as sent");
  Check(SendCrashReports(c, 1) == 1 && Header(sent.back(), "X-KK-Kind") == "hang", "then the hang report");
  Check(PendingReports(c).empty() && SendCrashReports(c, 5) == 0, "nothing left to send");

  // The summary: kept until the server takes it.
  sent.clear();
  fs::remove(c.state_dir / "perf-pending.json", ec);
  answer = 0;
  Check(SendPerf(c, "{\"installId\":\"y\"}") == 0 && sent.size() == 1 && fs::exists(c.state_dir / "perf-pending.json"),
        "no answer: the summary is kept");
  Check(sent[0].path == "/v1/perf" && sent[0].content_type == "application/json", "summary request");
  answer = 200;
  Check(SendPendingPerf(c) == 200 && !fs::exists(c.state_dir / "perf-pending.json") &&
            fs::exists(c.state_dir / "perf-last.json"),
        "sent at the next start, then kept as perf-last.json");
  Check(SendPerf(c, std::string(kMaxPerfBytes + 1, 'x')) == 0, "a summary over 16 KB is never sent");
}
}  // namespace

int main(int argc, char** argv) {
  const fs::path out = argc > 1 ? argv[1] : "telemetry-test";
  fs::create_directories(out);
  Redaction();
  Json();
  Stats();
  Perf(out);
  GzipSamples(out);
  Uploads(out);
  if (failures) return 1;
  std::printf("telemetry: all checks passed\n");
  return 0;
}
