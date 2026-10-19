// Task 194 (Beta 7 uploads, codex/state/telemetry-b7.md): redaction, gzip, the size limits, per-chapter
// statistics (task 211: the worst-frame detail) and the rule that nothing is sent while the Online player count switch is off. No game, no
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
            "King Kong crashed.\nException 0xC0000005 at 0000\nGraphics API: vulkan\nStack:\n  king_kong.exe + 0x1234\n"
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
            Header(sent[0], "X-KK-Install") == c.install_id && Header(sent[0], "X-KK-Api") == "vulkan",
        "crash request headers (task 270: the crashed session's API, not the uploader's d3d12)");
  WriteFile(out / "crash-body.gz", sent[0].body);
  Check(PendingReports(c).size() == 1, "the crash report is remembered as sent");
  Check(SendCrashReports(c, 1) == 1 && Header(sent.back(), "X-KK-Kind") == "hang", "then the hang report");
  Check(Header(sent.back(), "X-KK-Api") == "d3d12", "a report that names no API: the uploader's");
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

// Task 211: the worst-frame detail: which frames are kept, and what they look like in the summary.
WorstFrame Frame(double ms, bool loading = false) {
  WorstFrame f;
  f.ms = ms;
  f.loading = loading;
  return f;
}
std::string MsList(const std::vector<WorstFrame>& list) {
  std::string s;
  for (const auto& f : list) s += (s.empty() ? "" : ",") + JsonNumber(f.ms, 0) + (f.loading ? "L" : "");
  return s;
}
WorstFrame FullFrame() {
  WorstFrame f;
  f.ms = 11234.56, f.at_s = 12.34, f.cpu_ms = 3.21, f.swap_ms = 11200.04, f.cap_wait_ms = 0;
  f.native = true, f.render_frames = 3, f.render_cpu_ms = 11190.0, f.gpu_ms = 4.567, f.gpu_wait_ms = 1.0;
  f.submit_ms = 0.5, f.worker_jobs = 12, f.pipe_waits = 2, f.pipe_wait_ms = 11150.3, f.pipes_made = 40;
  f.compile_ms = 250.0, f.tex_uploads = 7, f.upload_mb = 1.5, f.tex_ms = 2.0, f.pack_textures = 1, f.pack_mb = 0.75;
  f.texture_pack = true, f.reads = 4, f.read_mb = 0.25, f.read_ms = 3.0, f.read_max_ms = 2.5, f.focus = 0;
  f.frame_cap = 60, f.vsync = 0, f.hfr = 1, f.upscaler = "dlss";
  return f;
}

void WorstFrames(const fs::path& out) {
  // The threshold: only chapter frames of 250 ms and more.
  Check(!WorstFrameWanted(-1, 11200), "menus are not recorded");
  Check(!WorstFrameWanted(3, 249.9), "249.9 ms is under the threshold");
  Check(WorstFrameWanted(3, 250.0) && WorstFrameWanted(0, 11200), "250 ms and over are recorded");
  {
    // Task 217: the 11 s "load freezes" were PS2 cutscenes between story chapters; the frame that spans the
    // level loader's wait for one is left out, the frames before and after it are not.
    CutsceneFrames c;
    Check(!c.Spans(0, false), "no cutscene: a normal frame");
    Check(c.Spans(0, true), "a frame while the loader waits for a cutscene");
    Check(c.Spans(1, false), "the frame in which the wait ended");
    Check(!c.Spans(1, false), "the next frame counts again");
    Check(c.Spans(3, false), "two waits since the last frame");
    Check(!c.Spans(3, false), "and then normal frames");
  }
  Check(!WorstFrameWanted(3, std::nan("")) && !WorstFrameWanted(3, INFINITY), "not finite: not recorded");
  std::vector<WorstFrame> list;
  AddWorstFrame(list, Frame(249.9));
  AddWorstFrame(list, Frame(std::nan("")));
  Check(list.empty(), "under the threshold is never kept");

  // The slowest 3, slowest first.
  for (double ms : {300.0, 1000.0, 260.0, 11200.0, 500.0}) AddWorstFrame(list, Frame(ms));
  Same(MsList(list), "11200,1000,500", "top 3 gameplay frames");
  // At most one loading frame: a slower load replaces it, a faster one is not kept.
  AddWorstFrame(list, Frame(5000, true));
  Same(MsList(list), "11200,5000L,1000", "a loading frame joins");
  AddWorstFrame(list, Frame(4000, true));
  Same(MsList(list), "11200,5000L,1000", "a faster second load is not kept");
  AddWorstFrame(list, Frame(6000, true));
  Same(MsList(list), "11200,6000L,1000", "a slower load replaces the loading frame");
  AddWorstFrame(list, Frame(7000));
  AddWorstFrame(list, Frame(8000));
  Same(MsList(list), "11200,8000,7000", "gameplay frames push the load out when they are slower");
  AddWorstFrame(list, Frame(400));
  Same(MsList(list), "11200,8000,7000", "a faster frame is not kept when the list is full");

  // Merging visits keeps the same rule.
  ChapterStats a, b;
  a.entry = b.entry = 2;
  AddWorstFrame(a.worst, Frame(900));
  AddWorstFrame(a.worst, Frame(3000, true));
  AddWorstFrame(b.worst, Frame(2500, true));
  AddWorstFrame(b.worst, Frame(1200));
  AddWorstFrame(b.worst, Frame(700));
  a.Merge(b);
  Same(MsList(a.worst), "3000L,1200,900", "merged visits: top 3, one load");

  // The serialised format, exactly.
  Same(FullFrame().Json(),
       "{\"ms\":11234.6,\"at\":12.3,\"phase\":\"play\",\"cpuMs\":3.2,\"swapMs\":11200.0,\"capMs\":0.0,\"gameMs\":34.5,"
       "\"rFrames\":3,\"rCpuMs\":11190.0,\"gpuMs\":4.57,\"gpuWaitMs\":1.0,\"submitMs\":0.5,\"rLoading\":false,"
       "\"jobs\":12,\"pipeWaits\":2,\"pipeWaitMs\":11150.3,\"pipesMade\":40,\"drawsSkipped\":0,\"compileMs\":250.0,"
       "\"texUploads\":7,\"uploadMB\":1.50,\"texMs\":2.0,\"prepMs\":0.0,\"packTex\":1,\"packMB\":0.75,\"pack\":true,"
       "\"reads\":4,\"readMB\":0.25,\"readMs\":3.0,\"readMaxMs\":2.5,\"videoReads\":0,\"focus\":\"focused\","
       "\"focusChanged\":false,\"paused\":false,\"sleepMs\":0,\"download\":false,\"cap\":60,\"vsync\":false,"
       "\"hfr\":true,\"upscaler\":\"dlss\"}",
       "full worst-frame JSON");
  // What is not known is left out (no renderer, no window state, no settings read).
  Same(Frame(300, true).Json(),
       "{\"ms\":300.0,\"phase\":\"load\",\"pack\":false,\"reads\":0,\"readMB\":0.00,\"readMs\":0.0,\"readMaxMs\":0.0,"
       "\"videoReads\":0,\"paused\":false,\"sleepMs\":0,\"download\":false}",
       "minimal worst-frame JSON");
  WorstFrame odd = Frame(300);
  odd.focus = 2, odd.focus_changed = true, odd.sleep_ms = 9000.4, odd.upscaler = "C:\\Users\\x/../\"fsr 3\"";
  const std::string oj = odd.Json();
  Check(oj.find("\"focus\":\"minimised\",\"focusChanged\":true") != std::string::npos, "minimised: " + oj);
  Check(oj.find("\"sleepMs\":9000") != std::string::npos, "asleep: " + oj);
  Check(oj.find("\"upscaler\":\"CUsersxfsr3\"") != std::string::npos && oj.find('\\') == std::string::npos,
        "the upscaler is a plain word, never a path or free text: " + oj);

  // In the chapter's JSON (and left out on request).
  ChapterStats c;
  c.entry = 1;
  for (int i = 0; i < 600; ++i) c.AddFrame(16.7);
  c.AddFrame(11234.56);
  AddWorstFrame(c.worst, FullFrame());
  const std::string cj = c.Json();
  Check(cj.find(",\"worst\":[{\"ms\":11234.6,") != std::string::npos && cj.back() == '}', "chapter JSON: " + cj);
  Check(c.Json(false).find("worst\"") == std::string::npos, "chapter JSON without the detail");
  Check(ChapterStats{}.Json().find("worst\"") == std::string::npos, "no slow frame, no worst list");

  // The 16 KB limit: the detail goes from the shortest chapters first, every chapter stays.
  PerfSummary s;
  s.install_id = "0123abcd-0000-4000-8000-0000000000ff";
  s.version = "2.0.0-beta.9";
  s.api = "vulkan";
  for (int e = 0; e < 20; ++e) {
    ChapterStats ch;
    ch.entry = e;
    for (int i = 0; i < 1000 - e * 10; ++i) ch.AddFrame(16.7);
    for (int k = 0; k < 3; ++k) {
      WorstFrame f = FullFrame();
      f.ms = 300 + 100 * k + e;
      AddWorstFrame(ch.worst, f);
    }
    s.chapters.push_back(ch);
  }
  const std::string json = PerfJson(s);
  size_t chapters = 0, details = 0;
  for (size_t at = 0; (at = json.find("\"entry\":", at)) != std::string::npos; ++at) ++chapters;
  for (size_t at = 0; (at = json.find("\"worst\":", at)) != std::string::npos; ++at) ++details;
  Check(!json.empty() && json.size() <= kMaxPerfBytes, "summary with details within 16 KB: " + std::to_string(json.size()));
  Check(chapters == 20, "every chapter kept, only detail dropped: " + std::to_string(chapters));
  Check(details > 0 && details < 20, "some chapters keep their detail: " + std::to_string(details));
  Check(json.find("{\"entry\":0,") != std::string::npos &&
            json.find("\"worst\":") < json.find("{\"entry\":1,"),
        "the longest chapter keeps its detail");
  Check(json.find(":\\") == std::string::npos && json.find("Users") == std::string::npos, "no paths in the detail");
  WriteFile(out / "perf-worst.json", json);
}

// Task 270: crash report API, hitch causes, video memory, the precise CPU clock.
void Task270(const fs::path& out) {
  // The crashed session's API.
  Same(ReportApi("King Kong crashed.\nGraphics API: d3d12\nStack:\n"), "d3d12", "report API line");
  Same(ReportApi("x\r\nGraphics API: vulkan\r\n"), "vulkan", "report API line (CRLF)");
  Same(ReportApi("Settings:\n  native_backend = d3d12\n"), "d3d12", "the backend setting when no API line");
  Same(ReportApi("Graphics API: vulkan\n  native_backend = d3d12\n"), "vulkan", "the API line wins");
  Same(ReportApi("  native_backend = auto\nmy_native_backend = d3d12\n"), "", "auto / another key: not known");
  Same(ReportApi("Graphics API: "), "", "cut short");

  // Hitch causes.
  Check(!HitchWanted(3, 50.0) && HitchWanted(3, 50.1) && HitchWanted(3, 249.9) && !HitchWanted(3, 250.0),
        "hitches are over 50 and under 250 ms");
  Check(!HitchWanted(-1, 100) && !HitchWanted(3, std::nan("")), "menus and non-finite frames are not hitches");
  const auto hitch = [](double ms) {
    WorstFrame f;
    f.ms = ms;
    f.swap_ms = 2, f.cap_wait_ms = 0, f.focus = 0, f.native = true;
    return f;
  };
  const auto cause = [](const WorstFrame& f) { return std::string(HitchName(ClassifyHitch(f))); };
  WorstFrame f = hitch(100);
  Same(cause(f), "game", "the game thread's own time");
  f.pipe_wait_ms = 30;
  Same(cause(f), "pipeline", "a pipeline wait of a quarter of the frame");
  f.tex_ms = 20, f.prepare_ms = 20;
  Same(cause(f), "texture", "the largest measured wait wins");
  f.read_max_ms = 60;
  Same(cause(f), "read", "a long file read");
  f.gpu_wait_ms = 70;
  Same(cause(f), "gpu", "waiting for the GPU");
  f.submit_ms = 80;
  Same(cause(f), "present", "present/submit");
  f.focus_changed = true;
  Same(cause(f), "focus", "focus change comes first");
  f = hitch(100);
  f.focus = 1;
  Same(cause(f), "focus", "in the background");
  f = hitch(100);
  f.sleep_ms = 500;
  Same(cause(f), "focus", "the PC slept");
  f = hitch(100);
  f.pipe_wait_ms = 20;  // under a quarter: not the cause; the game thread is
  Same(cause(f), "game", "small waits do not count");
  f = hitch(100);
  f.swap_ms = 70;
  Same(cause(f), "present", "the swap call waiting for the renderer");
  f.pipes_made = 3, f.compile_ms = 40;
  Same(cause(f), "compile", "background pipeline creation before the swap wait");
  f = hitch(100);
  f.native = false, f.pipe_wait_ms = 90, f.swap_ms = 60;
  Same(cause(f), "present", "no renderer figures: renderer causes are not used");
  f = hitch(100);
  f.swap_ms = -1, f.cap_wait_ms = -1;
  Same(cause(f), "unknown", "nothing measured");

  // Per-chapter counters, merged, in the JSON only when not zero.
  ChapterStats a, b;
  a.entry = b.entry = 9;
  for (int i = 0; i < 600; ++i) a.AddFrame(1000.0 / 60);
  a.AddHitch(Hitch::kPipeline), a.AddHitch(Hitch::kPipeline), a.AddHitch(Hitch::kGame);
  b.AddFrame(16), b.AddHitch(Hitch::kPipeline), b.AddHitch(Hitch::kUnknown);
  a.Merge(b);
  Check(a.hitches[size_t(Hitch::kPipeline)] == 3 && a.hitches[size_t(Hitch::kUnknown)] == 1, "hitch counts merge");
  std::string json = a.Json();
  Check(json.find(",\"hitch\":{\"pipeline\":3,\"game\":1,\"unknown\":1}") != std::string::npos, "hitch JSON: " + json);
  Check(ChapterStats{}.Json().find("hitch") == std::string::npos, "no hitches: no hitch field");

  // Video memory: the chapter's peak (with the budget then) and the value at the worst frame.
  ChapterStats v;
  v.entry = 2;
  v.AddFrame(16), v.AddVram(3000, 8000, true);
  v.AddFrame(120), v.AddVram(5000, 7900, true);  // the worst frame so far
  v.AddFrame(16), v.AddVram(7000, 7800, false);  // the peak
  v.AddFrame(16), v.AddVram(-1, -1, false);      // not known: nothing changes
  Check(v.vram_peak_mib == 7000 && v.vram_peak_budget_mib == 7800, "VRAM peak and its budget");
  Check(v.vram_worst_mib == 5000 && v.vram_worst_budget_mib == 7900, "VRAM at the worst frame");
  ChapterStats w;  // a later part of the chapter with a slower frame whose VRAM is not known
  w.entry = 2;
  w.AddFrame(200), w.AddVram(-1, -1, true);
  ChapterStats v2 = v;
  v2.Merge(w);
  Check(v2.vram_worst_mib == -1 && v2.vram_peak_mib == 7000, "a slower frame without VRAM figures: not known");
  ChapterStats x;
  x.entry = 2;
  x.AddFrame(20), x.AddVram(7500, 7600, true);
  v.Merge(x);
  Check(v.vram_peak_mib == 7500 && v.vram_peak_budget_mib == 7600 && v.vram_worst_mib == 5000,
        "merge: higher peak taken, worst frame kept");
  json = v.Json();
  Check(json.find("\"vramPeakMiB\":7500,\"vramPeakBudgetMiB\":7600,\"vramWorstMiB\":5000,\"vramWorstBudgetMiB\":7900") !=
            std::string::npos,
        "VRAM JSON: " + json);
  WorstFrame wf;
  wf.ms = 300, wf.vram_mib = 6100, wf.budget_mib = 6144, wf.cause = "texture";
  json = wf.Json();
  Check(json.find("\"vramMiB\":6100,\"budgetMiB\":6144") != std::string::npos && json.find("\"cause\":\"texture\"") != std::string::npos,
        "worst frame VRAM and cause: " + json);
  wf.cause = "C:\\Users\\x";
  Check(wf.Json().find('\\') == std::string::npos && wf.Json().find("\"cause\":\"CUsersx\"") != std::string::npos,
        "the cause is a plain word");

  // The summary: session peak and whether pack textures were loaded; absent when not known.
  PerfSummary s;
  s.install_id = "0123abcd-0000-4000-8000-0000000000ff", s.version = "2.0.0-beta.11", s.api = "vulkan";
  ChapterStats ch = a;
  ch.total_ms = 60000;
  s.chapters.push_back(ch);
  json = PerfJson(s);
  Check(json.find("vramPeak") == std::string::npos, "no session VRAM when not known");
  Check(json.find("texturePackUsed") == std::string::npos, "pack use not known: left out");
  s.vram_peak_mib = 7900, s.vram_peak_budget_mib = 8000, s.texture_pack_used = 1;
  json = PerfJson(s);
  Check(json.find("\"startupFallback\":false,\"vramPeakMiB\":7900,\"vramPeakBudgetMiB\":8000,\"texturePackUsed\":true}") !=
            std::string::npos,
        "session VRAM and pack use: " + json.substr(json.size() > 120 ? json.size() - 120 : 0));
  WriteFile(out / "perf-task270.json", json);

  // A full summary still fits 16 KB: 64 chapters with every new field.
  PerfSummary big = s;
  big.chapters.clear();
  for (int i = 0; i < 64; ++i) {
    ChapterStats c = ch;
    c.entry = i;
    for (size_t k = 0; k < kHitchCauses; ++k) c.hitches[k] = 1000 + uint32_t(k);
    c.vram_peak_mib = c.vram_worst_mib = 12000, c.vram_peak_budget_mib = c.vram_worst_budget_mib = 12288;
    for (double ms : {11200.0, 900.0, 300.0}) {
      WorstFrame fr = Frame(ms);
      fr.vram_mib = 12000, fr.budget_mib = 12288, fr.cause = "pipeline";
      AddWorstFrame(c.worst, fr);
    }
    big.chapters.push_back(c);
  }
  json = PerfJson(big);
  Check(!json.empty() && json.size() <= kMaxPerfBytes, "64 full chapters within 16 KB: " + std::to_string(json.size()));

  // The precise CPU clock: calibration and the check against the coarse thread time.
  Check(PreciseCpuClock::Rate(0, 0, 3000000, 999) == 0, "under a second: no rate yet");
  Check(std::abs(PreciseCpuClock::Rate(1000, 10, 3000001000ull, 1010) - 3e6) < 1, "3 GHz over a second");
  Check(PreciseCpuClock::Rate(5, 0, 5, 2000) < 0 && PreciseCpuClock::Rate(0, 0, 1000, 2000) < 0,
        "a cycle counter that does not move: nonsense");
  Check(PreciseCpuClock::Rate(0, 2000, 10, 0) < 0, "time going back: nonsense");
  PreciseCpuClock clock;
  Check(clock.Check(100.0, 93.6) && clock.Check(600.3, 593.0), "agreeing figures, under the check window");
  Check(clock.Check(1120.4, 1124.0) && clock.Trusted(), "agreeing over a second: trusted");
  Check(clock.Check(2130.0, 2140.0) && clock.Trusted(), "quantised coarse figures still agree");
  Check(!clock.Check(2200.0, 3200.0) && !clock.Trusted(), "a big disagreement: dropped");
  Check(!clock.Check(4000.0, 4000.0) && !clock.Trusted(), "dropped for good");
  PreciseCpuClock wine;
  wine.Check(0, 0);
  Check(!wine.Check(10000.0, 1000.0), "a cycle count 10x too fast is dropped");
  PreciseCpuClock failed;
  failed.Fail();
  Check(!failed.Trusted() && !failed.Check(1, 1), "Fail: never trusted again");
  // Resolution: a 3 ms frame of CPU time is seen as 3 ms, not 0 or 15.6.
  const double rate = 3e6;
  Check(std::abs((uint64_t(1000 * rate + 3 * rate) / rate - uint64_t(1000 * rate) / rate) - 3.0) < 1e-6,
        "3 ms of cycles is 3 ms");
}

int main(int argc, char** argv) {
  const fs::path out = argc > 1 ? argv[1] : "telemetry-test";
  fs::create_directories(out);
  Redaction();
  Json();
  Stats();
  Perf(out);
  WorstFrames(out);
  Task270(out);
  GzipSamples(out);
  Uploads(out);
  if (failures) return 1;
  std::printf("telemetry: all checks passed\n");
  return 0;
}
