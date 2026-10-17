// Beta 7 uploads (codex/state/telemetry-b7.md): what is sent, when, and what is remembered afterwards, with
// the network call passed in (the game's is HttpPostBody; the tests' a fake). Nothing here sends anything
// when the Online player count switch is off. No game or runtime needed (tests/telemetry_tests.cpp).

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "telemetry_logic.h"

namespace kk::telemetry {

struct Request {
  std::string path;          // "/v1/pipelines", "/v1/perf", "/v1/crash"
  std::string content_type;  // application/gzip or application/json
  std::vector<std::string> headers;  // "X-KK-Install: <uuid>", ...
  std::string body;
};
// Sends one request; the HTTP status, or 0 when nothing answered in time.
using Sender = std::function<int(const Request&)>;

struct Context {
  bool switch_on = false;   // Online player count (kk_online_count)
  std::string install_id;   // kk_install_id
  std::string version;      // KK_VERSION
  std::string api;          // vulkan or d3d12
  std::filesystem::path state_dir;                 // what was sent (user data/telemetry)
  std::vector<std::filesystem::path> report_dirs;  // crash-*.txt (user data/crashes) and hang-*.txt (logs)
  std::filesystem::path pipelines_file;            // cache/native-pipelines/<title>.pipelines
  Private personal;
  Sender send;
  bool force = false;  // developer aid: send the pipeline file even when it was sent unchanged
};

// True when uploads may happen at all: the switch is on, an install ID, the version and the API are known
// and a sender is set.
bool Allowed(const Context& c);

// The pipeline record file, gzip (at most 1 MB): sent when it changed since it was last accepted.
// Returns the HTTP status; 0: not allowed or no answer; below 0, why it was not sent:
constexpr int kNoPipelineFile = -1, kNotPipelineFile = -2, kPipelinesUnchanged = -3, kPipelinesTooBig = -4;
int SendPipelines(const Context& c);

// The session's performance summary (JSON from PerfJson). Saved as telemetry/perf-pending.json first, so
// a summary the server never answered goes at the next start (SendPendingPerf).
int SendPerf(const Context& c, const std::string& json);
int SendPendingPerf(const Context& c);

// Crash and hang reports of this version from the last 14 days not sent yet, newest first, user names and
// personal folders removed, at most `max` (the server takes one per install per 10 minutes). A report the
// server took (200) or refused for good (400, 413) is remembered as sent; on 429 or no answer it waits for
// the next start. Returns how many requests went out.
int SendCrashReports(const Context& c, int max = 1);

// The reports SendCrashReports would send now (names), for the tests and the log.
std::vector<std::filesystem::path> PendingReports(const Context& c);

}  // namespace kk::telemetry
