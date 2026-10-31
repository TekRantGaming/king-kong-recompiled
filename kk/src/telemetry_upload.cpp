#include "telemetry_upload.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace kk::telemetry {
namespace fs = std::filesystem;
namespace {

constexpr auto kReportAge = std::chrono::hours(24 * 14);

bool ValidUuid(const std::string& s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? s[i] != '-' : !std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

bool ReadFile(const fs::path& path, std::string& out, size_t limit) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(limit, '\0');
  f.read(out.data(), std::streamsize(limit));
  out.resize(size_t(f.gcount()));
  return true;
}

// Written whole or not at all: a quit can end the process at any moment.
bool WriteFileAtomic(const fs::path& path, const std::string& text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path temp = path.string() + ".new";
  {
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    if (!f.write(text.data(), std::streamsize(text.size()))) return false;
  }
  fs::rename(temp, path, ec);
  return !ec;
}

// `api`: the session the data is from (task 270: a crash report's own session), else this one's.
std::vector<std::string> Headers(const Context& c, const std::string& api = {}) {
  return {"X-KK-Install: " + c.install_id, "X-KK-Version: " + c.version, "X-KK-Api: " + (api.empty() ? c.api : api)};
}

std::string Hex32(uint32_t v) {
  char s[16];
  std::snprintf(s, sizeof(s), "%08x", v);
  return s;
}

std::set<std::string> SentReports(const Context& c) {
  std::set<std::string> sent;
  std::ifstream f(c.state_dir / "reports-sent.txt");
  for (std::string line; std::getline(f, line);)
    if (!line.empty()) sent.insert(line);
  return sent;
}

void MarkSent(const Context& c, const std::string& name) {
  std::set<std::string> sent = SentReports(c);
  sent.insert(name);
  std::string text;
  for (const auto& s : sent) text += s + "\n";
  WriteFileAtomic(c.state_dir / "reports-sent.txt", text);
}

}  // namespace

bool Allowed(const Context& c) {
  return c.switch_on && ValidUuid(c.install_id) && !c.version.empty() && (c.api == "vulkan" || c.api == "d3d12") &&
         static_cast<bool>(c.send);
}

int SendPipelines(const Context& c) {
  if (!Allowed(c)) return 0;
  std::string bytes;
  if (c.pipelines_file.empty() || !ReadFile(c.pipelines_file, bytes, 16 * 1024 * 1024)) return kNoPipelineFile;
  if (bytes.size() < 16 || bytes.compare(0, 4, "KGPC") != 0) return kNotPipelineFile;  // or empty
  const std::string signature =
      std::to_string(bytes.size()) + "-" + Hex32(Crc32(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  std::string last;
  ReadFile(c.state_dir / "pipelines-sent.txt", last, 256);
  while (!last.empty() && (last.back() == '\n' || last.back() == '\r')) last.pop_back();
  if (!c.force && last == signature) return kPipelinesUnchanged;  // already accepted as it is
  const std::vector<uint8_t> gz = Gzip(bytes);
  if (gz.size() > kMaxPipelinesGzip) return kPipelinesTooBig;
  const int status = c.send({"/v1/pipelines", "application/gzip", Headers(c), std::string(gz.begin(), gz.end())});
  if (status == 200) WriteFileAtomic(c.state_dir / "pipelines-sent.txt", signature + "\n");
  return status;
}

int SendPerf(const Context& c, const std::string& json) {
  if (!Allowed(c) || json.empty() || json.size() > kMaxPerfBytes) return 0;
  const fs::path pending = c.state_dir / "perf-pending.json";
  WriteFileAtomic(pending, json);
  return SendPendingPerf(c);
}

int SendPendingPerf(const Context& c) {
  if (!Allowed(c)) return 0;
  const fs::path pending = c.state_dir / "perf-pending.json";
  std::string json;
  if (!ReadFile(pending, json, kMaxPerfBytes + 1) || json.empty()) return 0;
  std::error_code ec;
  if (json.size() > kMaxPerfBytes) {
    fs::rename(pending, c.state_dir / "perf-refused.json", ec);
    return 0;
  }
  const int status = c.send({"/v1/perf", "application/json", {}, json});
  // Kept as perf-last.json (taken, or refused for good, so never sent again); 429 or no answer: next start.
  if (status == 200 || status == 400 || status == 413) fs::rename(pending, c.state_dir / "perf-last.json", ec);
  return status;
}

std::vector<fs::path> PendingReports(const Context& c) {
  std::vector<std::pair<fs::file_time_type, fs::path>> found;
  const std::set<std::string> sent = SentReports(c);
  const auto now = fs::file_time_type::clock::now();
  const std::string version_line = "Version: " + c.version;
  for (const auto& dir : c.report_dirs) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      const fs::path& p = it->path();
      const std::string name = p.filename().string();
      if (p.extension() != ".txt" || !(name.rfind("crash-", 0) == 0 || name.rfind("hang-", 0) == 0)) continue;
      if (sent.count(name)) continue;
      std::error_code tec;
      const auto time = fs::last_write_time(p, tec);
      if (tec || now - time > kReportAge) continue;
      // Only this version's reports: the version is in the report's text.
      std::string head;
      if (!ReadFile(p, head, 64 * 1024)) continue;
      const size_t at = head.find(version_line);
      if (at == std::string::npos) continue;
      const size_t after = at + version_line.size();
      if (after < head.size() && head[after] != '\n' && head[after] != '\r') continue;
      found.emplace_back(time, p);
    }
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<fs::path> out;
  for (auto& f : found) out.push_back(std::move(f.second));
  return out;
}

int SendCrashReports(const Context& c, int max) {
  if (!Allowed(c)) return 0;
  int sent = 0;
  for (const fs::path& p : PendingReports(c)) {
    if (sent >= max) break;
    std::string text;
    if (!ReadFile(p, text, 4 * kMaxCrashText)) continue;
    const std::string name = p.filename().string();
    const std::string api = ReportApi(text);  // the crashed session's API, not this one's
    const std::vector<uint8_t> gz = CrashGzip(RedactForUpload(std::move(text), c.personal));
    if (gz.empty()) {
      MarkSent(c, name);  // nothing to send
      continue;
    }
    std::vector<std::string> headers = Headers(c, api);
    headers.push_back(std::string("X-KK-Kind: ") + (name.rfind("hang-", 0) == 0 ? "hang" : "crash"));
    const int status = c.send({"/v1/crash", "application/gzip", headers, std::string(gz.begin(), gz.end())});
    ++sent;
    if (status == 200 || status == 400 || status == 413) MarkSent(c, name);
    else break;  // 429 or no answer: the rest wait for the next start
  }
  return sent;
}

}  // namespace kk::telemetry
