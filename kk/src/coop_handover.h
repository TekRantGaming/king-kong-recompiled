// Co-op hand over through the Play restart (task C80). With the live menu scene, Play starts the game as a new
// process (RelaunchWith); the co-op connection lives in the old one. The host sends a StartPlan over the live
// link, the guest acknowledges it, both write a small hand over file in their user folder and restart with
// --kk_coop_resume=<file name>. The new processes skip the launcher, reconnect by identity and session token,
// and hand over to the existing SessionStart / save transfer / load barrier.
//
// Pure, deterministic logic only: no file, socket, Steam, UI or game access (unit tested in
// tests/coop_handover_tests.cpp). The process side is coop_resume.cpp and coop_flow_game.cpp.
#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

#include "coop_protocol.h"
#include "coop_session.h"

namespace kk::coop::handover {

// Seconds a hand over file stays valid after it was written (both restarts take a few seconds).
inline constexpr double kFileLifetimeSeconds = 120;
// The resumed processes wait this long for each other before "Couldn't reach".
inline constexpr double kReconnectSeconds = 30;
// The old host waits this long for the guest's acknowledgement before giving up on the start.
inline constexpr double kAckSeconds = 10;
// The old guest restarts once the host's link ends (the host got the ack) or after this many seconds.
inline constexpr double kGuestLingerSeconds = 5;

inline bool ValidPlan(const StartPlan& p) { return ValidStartPlan(p); }

// ---- the start plan exchange in the old (launcher) process ----
enum class PlanPhase { Idle, WaitingAck, Acked, Ready, Failed };
class PlanExchange {
 public:
  PlanPhase Phase() const { return phase_; }
  const StartPlan& Plan() const { return plan_; }
  const char* Failure() const { return failure_; }
  const std::deque<Message>& Outgoing() const { return outgoing_; }
  void Sent() { outgoing_.pop_front(); }
  // Host: queue the plan (sent once the link is connected; the transport is reliable and ordered).
  bool Begin(const StartPlan& plan, double now) {
    if (phase_ != PlanPhase::Idle || !ValidPlan(plan)) return false;
    plan_ = plan; host_ = true; phase_ = PlanPhase::WaitingAck; since_ = now;
    Message m; m.type = MessageType::StartPlan; m.plan = plan; outgoing_.push_back(m);
    return true;
  }
  // A message from the partner. Duplicates are answered again, stale or foreign tokens are ignored.
  void Remote(const Message& m, double now) {
    if (m.type == MessageType::StartPlan) {
      if (host_ || !ValidPlan(m.plan)) return;
      if (phase_ == PlanPhase::Idle) { plan_ = m.plan; phase_ = PlanPhase::Acked; since_ = now; }
      else if (m.plan.token != plan_.token || phase_ == PlanPhase::Failed) return;  // another start: not ours
      Message ack; ack.type = MessageType::StartPlanAck; ack.plan.token = plan_.token;
      if (outgoing_.size() < 8) outgoing_.push_back(ack);
    } else if (m.type == MessageType::StartPlanAck) {
      if (!host_ || phase_ != PlanPhase::WaitingAck || m.plan.token != plan_.token) return;
      phase_ = PlanPhase::Ready;
    }
  }
  // The link's state each worker poll. Host: ack timeout. Guest: restart when the host's link ends (it had the
  // ack) or after the linger time.
  void Tick(double now, SessionState state) {
    if (phase_ == PlanPhase::WaitingAck) {
      if (state == SessionState::Ended) Fail("Your partner left before the start");
      else if (now - since_ >= kAckSeconds) Fail("Your partner did not answer the start");
    } else if (phase_ == PlanPhase::Acked && outgoing_.empty()) {
      if (state == SessionState::Ended || now - since_ >= kGuestLingerSeconds) phase_ = PlanPhase::Ready;
    }
  }
  void Reset() { *this = {}; }
 private:
  void Fail(const char* why) { phase_ = PlanPhase::Failed; failure_ = why; outgoing_.clear(); }
  StartPlan plan_;
  PlanPhase phase_ = PlanPhase::Idle;
  bool host_ = false;
  double since_ = 0;
  const char* failure_ = "";
  std::deque<Message> outgoing_;
};

// ---- the hand over file ----
struct File {
  bool host = false;
  StartPlan plan;
  int slot = -1;                 // host only: its own save slot for Continue (-1: newest)
  std::string partner_name;      // sanitized display name, for "Connecting to <name>"
  std::string address;           // Direct guest: the host's address; Direct host: the bind address
  std::string code;              // Direct join code
  uint16_t protocol = kProtocolVersion;
  std::string version;           // the game version that wrote it
  long long written_ms = 0;      // Unix epoch milliseconds
};
inline std::string Hex(uint64_t v) { char b[17]; std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v)); return b; }
inline bool ParseUnsigned(std::string_view s, uint64_t& out, int base = 10) {
  if (s.empty() || s.size() > (base == 16 ? 16u : 20u)) return false;
  uint64_t v = 0;
  for (char c : s) {
    int d = c >= '0' && c <= '9' ? c - '0' : base == 16 && c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0 || d >= base) return false;
    const uint64_t next = v * uint64_t(base) + uint64_t(d);
    if (next / uint64_t(base) != v) return false;
    v = next;
  }
  out = v; return true;
}
// A file name only (no folder): letters, digits, '-', '_' and '.', ending in .txt, so it never leaves the
// user folder and survives RelaunchWith's split on spaces.
inline bool SafeFileName(std::string_view n) {
  if (n.size() < 5 || n.size() > 80 || n.substr(n.size() - 4) != ".txt" || n.front() == '.') return false;
  for (char c : n) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
  return n.find("..") == std::string_view::npos;
}
inline std::string FileName(const File& f) { return std::string("coop-resume-") + (f.host ? "host-" : "guest-") + Hex(f.plan.token) + ".txt"; }
inline bool SafeText(std::string_view s, size_t limit) {
  if (s.size() > limit) return false;
  for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return false;
  return true;
}
inline std::string Format(const File& f) {
  std::string t = "kk_coop_resume=1\n";
  auto line = [&](const char* k, const std::string& v) { t += k; t += '='; t += v; t += '\n'; };
  line("role", f.host ? "host" : "guest");
  line("token", Hex(f.plan.token));
  line("transport", f.plan.transport == StartTransport::Steam ? "steam" : "direct");
  line("chapter", std::to_string(f.plan.chapter));
  line("continue", std::to_string(f.plan.continue_game));
  line("slot", std::to_string(f.slot));
  line("host_id", std::to_string(f.plan.host_id));
  line("guest_id", std::to_string(f.plan.guest_id));
  line("port", std::to_string(f.plan.port));
  line("address", f.address);
  line("code", f.code);
  line("partner", f.partner_name);
  line("protocol", std::to_string(f.protocol));
  line("version", f.version);
  line("written", std::to_string(f.written_ms));
  return t;
}
enum class ParseResult { Ok, Malformed, Expired, Protocol };
// now_ms: the reader's Unix clock. A file from the future (clock change) by more than 10 s is also stale.
inline ParseResult Parse(std::string_view text, long long now_ms, std::string_view version, File& out) {
  File f; bool magic = false, role = false, token = false, transport = false, written = false;
  uint64_t chapter = 99, cont = 9;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    std::string_view l = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    if (l.empty()) continue;
    const size_t eq = l.find('=');
    if (eq == std::string_view::npos) return ParseResult::Malformed;
    const auto k = l.substr(0, eq), v = l.substr(eq + 1);
    uint64_t n = 0;
    if (k == "kk_coop_resume") magic = v == "1";
    else if (k == "role") { if (v != "host" && v != "guest") return ParseResult::Malformed; f.host = v == "host"; role = true; }
    else if (k == "token") { if (!ParseUnsigned(v, f.plan.token, 16)) return ParseResult::Malformed; token = true; }
    else if (k == "transport") {
      if (v == "steam") f.plan.transport = StartTransport::Steam;
      else if (v == "direct") f.plan.transport = StartTransport::Direct;
      else return ParseResult::Malformed;
      transport = true;
    }
    else if (k == "chapter") { if (!ParseUnsigned(v, chapter)) return ParseResult::Malformed; }
    else if (k == "continue") { if (!ParseUnsigned(v, cont)) return ParseResult::Malformed; }
    else if (k == "slot") {
      if (v == "-1") f.slot = -1;
      else if (!ParseUnsigned(v, n) || n > 8) return ParseResult::Malformed;
      else f.slot = int(n);
    }
    else if (k == "host_id") { if (!ParseUnsigned(v, f.plan.host_id)) return ParseResult::Malformed; }
    else if (k == "guest_id") { if (!ParseUnsigned(v, f.plan.guest_id)) return ParseResult::Malformed; }
    else if (k == "port") { if (!ParseUnsigned(v, n) || n > 65535) return ParseResult::Malformed; f.plan.port = uint16_t(n); }
    else if (k == "address") { if (!SafeText(v, 64)) return ParseResult::Malformed; f.address = std::string(v); }
    else if (k == "code") { if (!SafeText(v, 16)) return ParseResult::Malformed; f.code = std::string(v); }
    else if (k == "partner") { if (v.size() > kMaxDisplayNameBytes) return ParseResult::Malformed; f.partner_name = SanitizeDisplayName(v); }
    else if (k == "protocol") { if (!ParseUnsigned(v, n) || n > 65535) return ParseResult::Malformed; f.protocol = uint16_t(n); }
    else if (k == "version") { if (!SafeText(v, 64)) return ParseResult::Malformed; f.version = std::string(v); }
    else if (k == "written") { if (!ParseUnsigned(v, n) || n > uint64_t(1) << 62) return ParseResult::Malformed; f.written_ms = (long long)n; written = true; }
    // Unknown keys are ignored (a newer writer may add some).
  }
  if (!magic || !role || !token || !transport || !written || chapter >= 42 || cont > 1) return ParseResult::Malformed;
  f.plan.chapter = uint8_t(chapter); f.plan.continue_game = uint8_t(cont);
  if (!ValidPlan(f.plan)) return ParseResult::Malformed;
  if (f.plan.transport == StartTransport::Direct && (f.code.size() != 6 || f.address.empty())) return ParseResult::Malformed;
  if (now_ms - f.written_ms > (long long)(kFileLifetimeSeconds * 1000) || f.written_ms - now_ms > 10000) return ParseResult::Expired;
  if (f.protocol != kProtocolVersion || f.version != version) return ParseResult::Protocol;
  if (f.partner_name.empty()) f.partner_name = "Partner";
  out = std::move(f); return ParseResult::Ok;
}

// ---- the resumed process: reconnect, then the existing start ----
enum class ResumePhase { Connecting, Starting, Launched, Failed };
enum class ResumeFailure { None, Timeout, PartnerLeft, Mismatch, StartFailed };
struct ResumeInput {
  SessionState state = SessionState::Idle;
  RejectReason rejection = RejectReason::None;
  bool start_ready = false;   // the SessionStart (and the guest's save) arrived
  bool flow_failed = false;
};
class ResumeMachine {
 public:
  void Begin(double now) { *this = {}; started_ = now; }
  ResumePhase Phase() const { return phase_; }
  ResumeFailure Failure() const { return failure_; }
  bool Connected() const { return connected_; }
  double Remaining(double now) const { return started_ + kReconnectSeconds - now; }
  // Returns true once, when the host should begin the SessionStart (first time connected).
  bool Tick(double now, const ResumeInput& in) {
    if (phase_ == ResumePhase::Failed || phase_ == ResumePhase::Launched) return false;
    if (in.rejection == RejectReason::Handshake || in.rejection == RejectReason::Protocol ||
        in.rejection == RejectReason::Version || in.rejection == RejectReason::Build ||
        in.rejection == RejectReason::GameplayMods) return Fail(ResumeFailure::Mismatch);
    if (in.state == SessionState::Ended) {
      // A link that gave up before the partner arrived (a guest's handshake ends after 20 s) starts again
      // until the reconnect time is over.
      if (!connected_ && now - started_ < kReconnectSeconds) { relink_ = true; return false; }
      return Fail(connected_ ? ResumeFailure::PartnerLeft : ResumeFailure::Timeout);
    }
    if (in.flow_failed && connected_) return Fail(ResumeFailure::StartFailed);
    bool begin = false;
    if (in.state == SessionState::Connected && !connected_) { connected_ = true; phase_ = ResumePhase::Starting; begin = true; }
    if (connected_ && in.start_ready) { phase_ = ResumePhase::Launched; return begin; }
    if (now - started_ >= kReconnectSeconds) return Fail(connected_ ? ResumeFailure::StartFailed : ResumeFailure::Timeout);
    return begin;
  }
  // The driver restarts the transport (same plan) when this returns true.
  bool TakeRelink() { const bool r = relink_; relink_ = false; return r; }
  // The launch could not be carried out (save import, a start the game refused).
  void LaunchFailed() { Fail(ResumeFailure::StartFailed); }
 private:
  bool Fail(ResumeFailure f) { phase_ = ResumePhase::Failed; failure_ = f; return false; }
  ResumePhase phase_ = ResumePhase::Connecting;
  ResumeFailure failure_ = ResumeFailure::None;
  bool connected_ = false, relink_ = false;
  double started_ = 0;
};
inline const char* FailureText(ResumeFailure f) {
  switch (f) {
    case ResumeFailure::Timeout: return "Your partner did not arrive in time.";
    case ResumeFailure::PartnerLeft: return "Your partner left during the start.";
    case ResumeFailure::Mismatch: return "This is not the same session (or a different game version).";
    case ResumeFailure::StartFailed: return "The chapter could not be started together.";
    default: return "";
  }
}
}  // namespace kk::coop::handover
