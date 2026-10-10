#include "hooks/trace_replay.h"

#include <charconv>
#include <cstdlib>
#include <string>

namespace nr::hooks {

namespace {

bool ParseHex(std::string_view s, uint32_t& out) {
  auto r = std::from_chars(s.data(), s.data() + s.size(), out, 16);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

// The value of `key=` up to the next space.
std::optional<std::string_view> Field(std::string_view line, std::string_view key) {
  size_t pos = 0;
  while ((pos = line.find(key, pos)) != std::string_view::npos) {
    if (pos == 0 || line[pos - 1] == ' ') {
      size_t begin = pos + key.size();
      size_t end = line.find(' ', begin);
      return line.substr(begin, end == std::string_view::npos ? line.size() - begin : end - begin);
    }
    pos += key.size();
  }
  return std::nullopt;
}

}  // namespace

std::optional<TraceCall> ParseTraceLine(std::string_view line) {
  constexpr std::string_view kTag = "KK d3d: sub_";
  size_t tag = line.find(kTag);
  if (tag == std::string_view::npos) return std::nullopt;
  std::string_view rest = line.substr(tag + kTag.size());
  size_t space = rest.find(' ');
  TraceCall call;
  if (space == std::string_view::npos || !ParseHex(rest.substr(0, space), call.address)) {
    return std::nullopt;
  }
  static constexpr std::string_view kRegs[8] = {"r3=", "r4=", "r5=", "r6=",
                                                "r7=", "r8=", "r9=", "r10="};
  for (int i = 0; i < 8; ++i) {
    auto v = Field(rest, kRegs[i]);
    if (!v || !ParseHex(*v, call.args.r[i])) return std::nullopt;
  }
  if (auto f = Field(rest, "f1=")) {
    call.args.f1 = std::strtod(std::string(*f).c_str(), nullptr);
  }
  return call;
}

ReplayStats ReplayTrace(std::istream& log, const NrApi* api) {
  ReplayStats stats;
  std::string line;
  while (std::getline(log, line)) {
    ++stats.lines;
    auto call = ParseTraceLine(line);
    if (!call) continue;
    ++stats.calls;
    const HookEntry* entry = Find(call->address);
    if (!entry) {
      ++stats.unhooked;
      continue;
    }
    ++stats.hooked;
    Run(*entry, api, call->args, [] {});
  }
  return stats;
}

}  // namespace nr::hooks
