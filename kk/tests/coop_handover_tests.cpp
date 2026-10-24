// C80: co-op hand over through the Play restart (coop_handover.h). No game, sockets, files or clocks.
#include "coop_flow.h"
#include "coop_handover.h"
#include "coop_protocol.h"
#include "coop_session.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

using namespace kk::coop;
namespace h = kk::coop::handover;
namespace {
int checks = 0;
void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
Compatibility Compatible() { return {kProtocolVersion, "test-version", "test-build", "test-gameplay"}; }
std::vector<uint8_t> Packet(const Message& m) { std::vector<uint8_t> out; Check(Encode(m, out), "encode"); return out; }
StartPlan DirectPlan(uint64_t token = 0xC80C80C801ull) { return {token, StartTransport::Direct, 3, 0, 0, 0, 27179}; }
StartPlan SteamPlan() { return {0x1234, StartTransport::Steam, 12, 1, 76561190000000001ull, 76561190000000002ull, 0}; }

struct MemoryTransport final : ICoopTransport {
  MemoryTransport* peer = nullptr;
  std::deque<std::vector<uint8_t>> incoming;
  TransportState state = TransportState::Ready;
  bool Send(std::span<const uint8_t> bytes, bool) override {
    if (peer && state == TransportState::Ready) peer->incoming.emplace_back(bytes.begin(), bytes.end());
    return state == TransportState::Ready;
  }
  bool Receive(std::vector<uint8_t>& out) override {
    if (incoming.empty()) return false;
    out = std::move(incoming.front()); incoming.pop_front(); return true;
  }
  void Poll(double) override {}
  TransportState State() const override { return state; }
  TransportStats Stats() const override { TransportStats s; s.received = incoming.size(); return s; }
  void Close() override { state = TransportState::Closed; }
};
SessionConfig Config(Role role, uint64_t token, uint64_t epoch) {
  SessionConfig c{true, role, role == Role::Host ? epoch : 0, Compatible()};
  c.display_name = role == Role::Host ? "Host" : "Guest";
  c.resume_token = token;
  return c;
}
struct Pair {
  MemoryTransport a, b;
  CoopSession host, guest;
  Pair(uint64_t host_token, uint64_t guest_token, uint64_t epoch)
      : host(Config(Role::Host, host_token, epoch), a), guest(Config(Role::Guest, guest_token, 0), b) { a.peer = &b; b.peer = &a; }
  void Pump(double begin, double end) {
    for (double t = begin; t <= end; t += .01) { host.Tick(t); guest.Tick(t); }
  }
};

void PlanCodec() {
  for (const auto& plan : {DirectPlan(), SteamPlan()}) {
    Message m; m.type = MessageType::StartPlan; m.epoch = 77; m.plan = plan;
    Message out;
    Check(Decode(Packet(m), out), "StartPlan decodes");
    Check(out.plan.token == plan.token && out.plan.transport == plan.transport && out.plan.chapter == plan.chapter &&
          out.plan.continue_game == plan.continue_game && out.plan.host_id == plan.host_id &&
          out.plan.guest_id == plan.guest_id && out.plan.port == plan.port, "StartPlan round trip");
    Check(IsFlowMessage(MessageType::StartPlan) && IsPlanMessage(MessageType::StartPlan), "StartPlan is a reliable flow message");
  }
  std::vector<uint8_t> bytes;
  Message bad; bad.type = MessageType::StartPlan;
  bad.plan = DirectPlan(0); Check(!Encode(bad, bytes), "zero token cannot encode");
  bad.plan = DirectPlan(); bad.plan.chapter = 42; Check(!Encode(bad, bytes), "chapter 42 cannot encode");
  bad.plan = DirectPlan(); bad.plan.port = 0; Check(!Encode(bad, bytes), "Direct plan needs a port");
  bad.plan = DirectPlan(); bad.plan.host_id = 5; Check(!Encode(bad, bytes), "Direct plan carries no Steam IDs");
  bad.plan = SteamPlan(); bad.plan.guest_id = bad.plan.host_id; Check(!Encode(bad, bytes), "Steam plan needs two accounts");
  bad.plan = SteamPlan(); bad.plan.transport = StartTransport(7); Check(!Encode(bad, bytes), "unknown transport rejected");
  // Tampered wire values never decode.
  Message good; good.type = MessageType::StartPlan; good.plan = DirectPlan();
  auto wire = Packet(good);
  auto t = wire; t[kHeaderBytes + 9] = 42; Message out;
  Check(!Decode(t, out), "wire chapter 42 rejected");
  t = wire; t[kHeaderBytes + 8] = 3; Check(!Decode(t, out), "wire transport 3 rejected");
  t = wire; t.pop_back(); Check(!Decode(t, out), "truncated plan rejected");
  Message ack; ack.type = MessageType::StartPlanAck; ack.plan.token = 99;
  Check(Decode(Packet(ack), out) && out.plan.token == 99 && out.type == MessageType::StartPlanAck, "ack round trip");
  ack.plan.token = 0; Check(!Encode(ack, bytes), "ack needs a token");
}
void HelloToken() {
  Message hello; hello.type = MessageType::Hello; hello.hello = Compatible(); hello.display_name = "Host";
  for (const char* code : {"", "C80ABC"}) {
    hello.join_code = code;
    for (uint64_t token : {0ull, 0xFEEDull}) {
      hello.resume_token = token;
      Message out;
      Check(Decode(Packet(hello), out) && out.resume_token == token && out.join_code == code, "Hello token and join code round trip");
    }
  }
  hello.join_code = "C80ABC"; hello.resume_token = 5;
  auto wire = Packet(hello); wire.pop_back();
  wire[20] = 0; wire[21] = 0; wire[22] = uint8_t((wire.size() - kHeaderBytes) >> 8); wire[23] = uint8_t(wire.size() - kHeaderBytes);
  Message out; Check(!Decode(wire, out), "truncated resume token rejected");
}
void ResumeHandshake() {
  { Pair p(0xAB, 0xAB, 0xAB); p.Pump(0, .3);
    Check(p.host.State() == SessionState::Connected && p.guest.State() == SessionState::Connected && p.guest.Epoch() == 0xAB,
          "same token reconnects; the host's epoch is the token"); }
  { Pair p(0xAB, 0xAC, 0xAB); p.Pump(0, .3);
    Check(p.host.State() == SessionState::Ended && p.host.Rejection() == RejectReason::Handshake, "host rejects a foreign token"); }
  { Pair p(0xAB, 0xAB, 0xAD); p.Pump(0, .3);
    Check(p.guest.State() == SessionState::Ended && p.guest.Rejection() == RejectReason::Handshake, "guest rejects a host of another session"); }
  { Pair p(0, 0xAB, 0x99); p.Pump(0, .3);
    Check(p.host.State() == SessionState::Ended && p.host.Rejection() == RejectReason::Handshake, "normal lobby never takes a resumed guest"); }
  { Pair p(0, 0, 0x99); p.Pump(0, .3);
    Check(p.host.State() == SessionState::Connected && p.guest.State() == SessionState::Connected, "normal session unchanged"); }
}
void FileFormat() {
  h::File f; f.host = false; f.plan = DirectPlan(); f.address = "127.0.0.1"; f.code = "C80ABC";
  f.partner_name = "Sam"; f.version = "2.0.0-test"; f.written_ms = 1'000'000;
  const auto text = h::Format(f);
  h::File out;
  Check(h::Parse(text, 1'000'000 + 5000, "2.0.0-test", out) == h::ParseResult::Ok, "file parses");
  Check(!out.host && out.plan.token == f.plan.token && out.plan.port == 27179 && out.address == "127.0.0.1" &&
        out.code == "C80ABC" && out.partner_name == "Sam" && out.plan.chapter == 3, "file round trip");
  Check(h::Parse(text, 1'000'000 + 121'000, "2.0.0-test", out) == h::ParseResult::Expired, "a 121 s old file is stale");
  Check(h::Parse(text, 1'000'000 - 11'000, "2.0.0-test", out) == h::ParseResult::Expired, "a file from the future is stale");
  Check(h::Parse(text, 1'000'000, "2.0.0-other", out) == h::ParseResult::Protocol, "another version's file is refused");
  auto edit = [&](const std::string& from, const std::string& to) { auto t = text; t.replace(t.find(from), from.size(), to); return t; };
  Check(h::Parse(edit("protocol=" + std::to_string(kProtocolVersion), "protocol=1"), 1'000'000, "2.0.0-test", out) == h::ParseResult::Protocol, "old protocol refused");
  Check(h::Parse(edit("role=guest", "role=boss"), 1'000'000, "2.0.0-test", out) == h::ParseResult::Malformed, "bad role");
  Check(h::Parse(edit("token=", "token=zz"), 1'000'000, "2.0.0-test", out) == h::ParseResult::Malformed, "bad token");
  Check(h::Parse(edit("code=C80ABC", "code="), 1'000'000, "2.0.0-test", out) == h::ParseResult::Malformed, "Direct needs a code");
  Check(h::Parse(edit("chapter=3", "chapter=42"), 1'000'000, "2.0.0-test", out) == h::ParseResult::Malformed, "chapter range");
  Check(h::Parse(edit("kk_coop_resume=1", "kk_coop_resume=0"), 1'000'000, "2.0.0-test", out) == h::ParseResult::Malformed, "magic line");
  Check(h::Parse("", 0, "x", out) == h::ParseResult::Malformed, "empty file");
  Check(h::Parse(text + "future_key=1\n", 1'000'000, "2.0.0-test", out) == h::ParseResult::Ok, "unknown keys ignored");
  h::File s; s.host = true; s.plan = SteamPlan(); s.slot = 2; s.version = "v"; s.written_ms = 50;
  Check(h::Parse(h::Format(s), 60, "v", out) == h::ParseResult::Ok && out.host && out.slot == 2 &&
        out.plan.host_id == s.plan.host_id && out.partner_name == "Partner", "Steam host file");
  Check(h::SafeFileName(h::FileName(f)), "own file name is safe");
  for (const char* bad : {"..\\x.txt", "a/b.txt", "c:x.txt", "x.ini", "a b.txt", ".txt", "x..txt"})
    Check(!h::SafeFileName(bad), "unsafe names refused");
}
void Exchange() {
  h::PlanExchange host, guest;
  Check(!host.Begin(DirectPlan(0), 0), "invalid plan refused");
  Check(host.Begin(DirectPlan(), 0) && host.Phase() == h::PlanPhase::WaitingAck && host.Outgoing().size() == 1, "host queues plan");
  Check(!host.Begin(DirectPlan(), 0), "one plan at a time");
  const Message plan = host.Outgoing().front(); host.Sent();
  guest.Remote(plan, 1);
  Check(guest.Phase() == h::PlanPhase::Acked && guest.Outgoing().size() == 1 && guest.Outgoing().front().plan.token == plan.plan.token, "guest acks");
  guest.Remote(plan, 1.1);
  Check(guest.Outgoing().size() == 2, "duplicate plan answered again");
  Message other = plan; other.plan.token = 5; guest.Remote(other, 1.2);
  Check(guest.Outgoing().size() == 2 && guest.Plan().token == plan.plan.token, "stale plan of another start ignored");
  Message foreign; foreign.type = MessageType::StartPlanAck; foreign.plan.token = 5;
  host.Remote(foreign, 2); Check(host.Phase() == h::PlanPhase::WaitingAck, "ack with another token ignored");
  host.Remote(guest.Outgoing().front(), 2); Check(host.Phase() == h::PlanPhase::Ready, "host ready on its ack");
  host.Remote(guest.Outgoing().back(), 2.1); Check(host.Phase() == h::PlanPhase::Ready, "duplicate ack harmless");
  host.Tick(50, SessionState::Ended); Check(host.Phase() == h::PlanPhase::Ready, "a ready plan survives the link's end");
  guest.Remote(plan, 3); guest.Tick(3, SessionState::Connected);
  Check(guest.Phase() == h::PlanPhase::Acked, "guest waits while acks are queued");
  while (!guest.Outgoing().empty()) guest.Sent();
  guest.Tick(3, SessionState::Connected); Check(guest.Phase() == h::PlanPhase::Acked, "guest lingers for the host");
  guest.Tick(3.1, SessionState::Ended); Check(guest.Phase() == h::PlanPhase::Ready, "guest restarts once the host left");
  h::PlanExchange g2; g2.Remote(plan, 0); g2.Sent();
  g2.Tick(h::kGuestLingerSeconds + .1, SessionState::Connected); Check(g2.Phase() == h::PlanPhase::Ready, "guest restarts after the linger time");
  h::PlanExchange h2; h2.Begin(DirectPlan(), 0);
  h2.Tick(h::kAckSeconds + .1, SessionState::Connected); Check(h2.Phase() == h::PlanPhase::Failed, "host gives up without an ack");
  h::PlanExchange h3; h3.Begin(DirectPlan(), 0);
  h3.Tick(1, SessionState::Ended); Check(h3.Phase() == h::PlanPhase::Failed && h3.Outgoing().empty(), "guest gone before the ack");
  h::PlanExchange g3; Message plan_from_guest = plan; h3.Remote(plan_from_guest, 2);
  Check(h3.Outgoing().empty(), "a host never answers a plan");
  Message ack_to_guest; ack_to_guest.type = MessageType::StartPlanAck; ack_to_guest.plan.token = plan.plan.token;
  g3.Remote(ack_to_guest, 0); Check(g3.Phase() == h::PlanPhase::Idle, "a guest ignores acks");
}
void Machine() {
  h::ResumeMachine m; m.Begin(100);
  h::ResumeInput in; in.state = SessionState::Connecting;
  Check(!m.Tick(101, in) && m.Phase() == h::ResumePhase::Connecting, "connecting");
  in.state = SessionState::Connected;
  Check(m.Tick(102, in) && m.Phase() == h::ResumePhase::Starting, "begin once when connected");
  Check(!m.Tick(102.1, in), "begin is not repeated");
  in.start_ready = true;
  m.Tick(103, in); Check(m.Phase() == h::ResumePhase::Launched, "start ready launches");
  m.Tick(104, {SessionState::Ended}); Check(m.Phase() == h::ResumePhase::Launched, "launched is final");

  h::ResumeMachine t; t.Begin(0);
  t.Tick(29.9, {SessionState::Connecting}); Check(t.Phase() == h::ResumePhase::Connecting, "still within 30 s");
  t.Tick(30, {SessionState::Connecting}); Check(t.Phase() == h::ResumePhase::Failed && t.Failure() == h::ResumeFailure::Timeout, "timeout at 30 s");
  t.Tick(31, {SessionState::Connected}); Check(t.Phase() == h::ResumePhase::Failed, "a late partner after the timeout is not taken");

  h::ResumeMachine r; r.Begin(0);
  r.Tick(20, {SessionState::Ended});
  Check(r.Phase() == h::ResumePhase::Connecting && r.TakeRelink() && !r.TakeRelink(), "an early link end relinks once");
  r.Tick(30.5, {SessionState::Ended}); Check(r.Failure() == h::ResumeFailure::Timeout, "link end after 30 s fails");

  h::ResumeMachine l; l.Begin(0);
  l.Tick(1, {SessionState::Connected}); l.Tick(2, {SessionState::Ended});
  Check(l.Failure() == h::ResumeFailure::PartnerLeft, "partner left after reconnecting");
  h::ResumeMachine x; x.Begin(0);
  x.Tick(1, {SessionState::Ended, RejectReason::Handshake}); Check(x.Failure() == h::ResumeFailure::Mismatch, "token mismatch");
  h::ResumeMachine v; v.Begin(0);
  v.Tick(1, {SessionState::Connecting, RejectReason::Version}); Check(v.Failure() == h::ResumeFailure::Mismatch, "version mismatch");
  h::ResumeMachine f; f.Begin(0);
  f.Tick(1, {SessionState::Connected}); f.Tick(2, {SessionState::Connected, RejectReason::None, false, true});
  Check(f.Failure() == h::ResumeFailure::StartFailed, "flow failure after reconnect");
  h::ResumeMachine s; s.Begin(0);
  s.Tick(1, {SessionState::Connected}); s.Tick(31, {SessionState::Connected});
  Check(s.Failure() == h::ResumeFailure::StartFailed, "no SessionStart within the window");
  h::ResumeMachine g; g.Begin(0); g.Tick(1, {SessionState::Connected}); g.LaunchFailed();
  Check(g.Phase() == h::ResumePhase::Failed && *h::FailureText(g.Failure()), "launch failure has a message");
  g.Begin(40); Check(g.Phase() == h::ResumePhase::Connecting && g.Remaining(40) == h::kReconnectSeconds, "Try again starts afresh");
}
void MailboxIntegration() {
  // The plan rides the real session and FlowMailbox (as both runtime workers use them).
  Pair p(0, 0, 0x5150);
  FlowMailbox host_box, guest_box;
  Check(host_box.BeginPlan(DirectPlan()), "host queues the plan before the link is up");
  for (double t = 0; t <= .5; t += .01) {
    p.host.Tick(t); host_box.Exchange(p.host, t);
    p.guest.Tick(t); guest_box.Exchange(p.guest, t);
  }
  StartPlan seen;
  Check(guest_box.PlanState(&seen) == h::PlanPhase::Acked && seen.token == DirectPlan().token, "guest received and acked");
  Check(host_box.PlanState() == h::PlanPhase::Ready, "host ready after the ack");
  // The host restarts (Bye); the guest then restarts too.
  p.host.End(.6);
  for (double t = .6; t <= .8; t += .01) { p.guest.Tick(t); guest_box.Exchange(p.guest, t); }
  Check(guest_box.PlanState() == h::PlanPhase::Ready, "guest ready once the host left");
  guest_box.Disconnect(); host_box.Disconnect();
  Check(guest_box.PlanState() == h::PlanPhase::Ready && host_box.PlanState() == h::PlanPhase::Ready, "agreed plans survive the link's teardown");
  FlowMailbox lone; lone.BeginPlan(DirectPlan()); lone.Disconnect();
  Check(lone.PlanState() == h::PlanPhase::Idle, "an unanswered plan is dropped on Leave");
  // A guest never sends a plan of its own.
  Pair q(0, 0, 0x5151);
  FlowMailbox hb, gb; gb.BeginPlan(DirectPlan());
  for (double t = 0; t <= .3; t += .01) { q.host.Tick(t); hb.Exchange(q.host, t); q.guest.Tick(t); gb.Exchange(q.guest, t); }
  Check(hb.PlanState() == h::PlanPhase::Idle && gb.PlanState() == h::PlanPhase::Idle, "guest plan refused");
}
}  // namespace

int main() {
  PlanCodec();
  HelloToken();
  ResumeHandshake();
  FileFormat();
  Exchange();
  Machine();
  MailboxIntegration();
  std::printf("coop handover tests: %d checks passed\n", checks);
  return 0;
}
