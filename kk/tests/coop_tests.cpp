// No game, SDK, windows, sockets, sleeps or wall-clock dependencies.
#include "coop_protocol.h"
#include "coop_session.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <utility>

using namespace kk::coop;
namespace {
void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
bool Near(double a, double b) { return std::abs(a - b) < .002; }
Compatibility Compatible() { return {kProtocolVersion, "test-version", "test-build", "test-gameplay"}; }
SessionConfig Config(Role role) { return {true, role, role == Role::Host ? 123456789ull : 0, Compatible()}; }
std::vector<uint8_t> Packet(Message m) {
  std::vector<uint8_t> out;
  Check(Encode(m, out), "encode test packet"); return out;
}
struct MemoryTransport final : ICoopTransport {
  MemoryTransport* peer = nullptr;
  std::deque<std::vector<uint8_t>> incoming;
  bool deliver = true;
  mutable int calls = 0;
  int unreliable_poses = 0;
  TransportState state = TransportState::Ready;
  bool Send(std::span<const uint8_t> bytes, bool reliable) override {
    ++calls;
    Message m;
    if (Decode(bytes, m) && m.type == MessageType::PlayerPose) {
      Check(!reliable, "poses are unreliable"); ++unreliable_poses;
    }
    if (deliver && peer) peer->incoming.emplace_back(bytes.begin(), bytes.end());
    return true;
  }
  bool Receive(std::vector<uint8_t>& out) override {
    ++calls;
    if (incoming.empty()) return false;
    out = std::move(incoming.front()); incoming.pop_front(); return true;
  }
  void Poll(double) override { ++calls; }
  TransportState State() const override { ++calls; return state; }
  TransportStats Stats() const override { ++calls; return {}; }
  void Close() override { ++calls; state = TransportState::Closed; }
};
struct Pair {
  MemoryTransport a, b;
  CoopSession host, guest;
  explicit Pair(Compatibility guest_compatibility = Compatible())
      : host(Config(Role::Host), a), guest([&] {
          auto c = Config(Role::Guest); c.compatibility = guest_compatibility; return c;
        }(), b) { a.peer = &b; b.peer = &a; }
  void Pump(double begin, double end, bool guest_first = false) {
    const int ticks = int(std::lround((end - begin) * 100));
    for (int i = 0; i <= ticks; ++i) {
      const double now = begin + i * .01;
      if (guest_first) { guest.Tick(now); host.Tick(now); }
      else { host.Tick(now); guest.Tick(now); }
    }
  }
};
void Protocol() {
  for (MessageType type : {MessageType::Hello, MessageType::HelloAck, MessageType::Reject,
      MessageType::Ping, MessageType::Pong, MessageType::PlayerPose, MessageType::Bye}) {
    Message input;
    input.type = type; input.sequence = 0x12345678; input.epoch = 0x0123456789ABCDEFull;
    input.hello = Compatible(); input.reason = RejectReason::GameplayMods;
    input.clock_ms = 0x123456789ABCDEFull;
    input.pose = {12.75, 1402, 1.25f, -9.5f, 33.75f, 2.5f, 7.125f, 65535};
    const auto bytes = Packet(input);
    Check(bytes[0] == 'K' && bytes[1] == 'K' && bytes[2] == 'C' && bytes[3] == '1', "wire magic");
    Check(bytes[8] == 0x12 && bytes[11] == 0x78 && bytes[12] == 1 && bytes[19] == 0xEF,
        "header byte order independent of host endianness");
    Message output;
    Check(Decode(bytes, output), "decode every message type");
    Check(output.type == type && output.sequence == input.sequence && output.epoch == input.epoch,
        "fixed header round trip");
    Check(Packet(output) == bytes, "canonical full payload round trip");
    if (type == MessageType::Hello)
      Check(CheckCompatibility(input.hello, output.hello) == RejectReason::None, "Hello fields");
    if (type == MessageType::PlayerPose)
      Check(Near(output.pose.time, 12.75) && output.pose.level == 1402 && Near(output.pose.y, -9.5) &&
          output.pose.action == 65535, "pose fields");
    // Every possible truncation, including incomplete string lengths and floats.
    for (size_t size = 0; size < bytes.size(); ++size) {
      Message sentinel; sentinel.sequence = 99;
      Check(!Decode(std::span(bytes.data(), size), sentinel) && sentinel.sequence == 99,
          "truncation rejected without mutating destination");
    }
    auto extra = bytes; extra.push_back(0);
    Check(!Decode(extra, output), "trailing bytes rejected");
    auto bad = bytes; bad[0] ^= 1;
    Check(!Decode(bad, output), "bad magic rejected");
    bad = bytes; bad[20] = 0xFF;
    Check(!Decode(bad, output), "oversized declared length rejected");
    bad = bytes; bad[6] = bad[7] = 0xFF;
    Check(!Decode(bad, output), "unknown message type rejected");
  }
  Message out, hello; hello.hello = Compatible();
  Check(!Decode(std::vector<uint8_t>(kMaxPacketBytes + 1), out), "oversized datagram rejected");
  hello.hello.build.assign(129, 'x');
  std::vector<uint8_t> bytes;
  Check(!Encode(hello, bytes), "oversized string cannot encode");
  hello.hello = Compatible();
  auto bad = Packet(hello);
  bad[kHeaderBytes + 2] = 0xFF; bad[kHeaderBytes + 3] = 0xFF;
  Check(!Decode(bad, out), "oversized embedded string length rejected");
  Message pose; pose.type = MessageType::PlayerPose; pose.pose.time = 1;
  bytes = Packet(pose);
  // First position float is header + time + level. A NaN must not reach interpolation or ImGui.
  const size_t x = kHeaderBytes + 12;
  bytes[x] = 0x7F; bytes[x + 1] = 0xC0; bytes[x + 2] = bytes[x + 3] = 0;
  Check(!Decode(bytes, out), "NaN wire position rejected");
  pose.pose.speed = std::numeric_limits<float>::infinity();
  Check(!Encode(pose, bytes), "infinite local speed rejected");
  Message reject; reject.type = MessageType::Reject;
  Check(!Encode(reject, bytes), "invalid rejection reason rejected");
}
void Handshake() {
  for (bool guest_first : {false, true}) {
    Pair pair;
    pair.Pump(0, .4, guest_first);
    Check(pair.host.State() == SessionState::Connected && pair.guest.State() == SessionState::Connected,
        "compatible handshake, either scheduler order");
    pair.Pump(.41, 2);
    Check(pair.host.Stats().ping_ms > 0 && pair.guest.Stats().ping_ms > 0, "ping and pong update RTT");
    pair.host.End(2.01); pair.guest.Tick(2.02);
    Check(pair.guest.State() == SessionState::Ended, "clean Bye ends peer");
  }
  for (RejectReason reason : {RejectReason::Protocol, RejectReason::Version, RejectReason::Build,
      RejectReason::GameplayMods}) {
    auto c = Compatible();
    if (reason == RejectReason::Protocol) ++c.protocol;
    if (reason == RejectReason::Version) c.version += "-different";
    if (reason == RejectReason::Build) c.build += "-different";
    if (reason == RejectReason::GameplayMods) c.gameplay_digest += "-different";
    Pair pair(c); pair.Pump(0, .4);
    Check(pair.host.State() == SessionState::Ended && pair.guest.State() == SessionState::Ended,
        "incompatible peers both end");
    Check(pair.host.Rejection() == reason && pair.guest.Rejection() == reason, "specific compatibility reason");
  }
  Pair late;
  late.guest.Tick(0); late.host.Tick(.2); late.Pump(.21, .5);
  Check(late.host.State() == SessionState::Connected && late.guest.State() == SessionState::Connected,
      "guest starts first, Hello queued before Ack");
}
void Timeouts() {
  Pair p; p.Pump(0, .4);
  p.a.deliver = p.b.deliver = false;
  p.a.incoming.clear(); p.b.incoming.clear();
  p.host.Tick(3.5); p.guest.Tick(3.5);
  Check(p.host.State() == SessionState::Lost && p.guest.State() == SessionState::Lost, "three second loss warning");
  p.a.deliver = p.b.deliver = true;
  p.Pump(3.6, 4.8);
  Check(p.host.State() == SessionState::Connected && p.guest.State() == SessionState::Connected, "loss recovers on heartbeat");
  p.a.deliver = p.b.deliver = false;
  p.a.incoming.clear(); p.b.incoming.clear();
  Message stale; stale.type = MessageType::Ping; stale.epoch = 999;
  p.a.incoming.push_back(Packet(stale)); p.host.Tick(24.9);
  Check(p.host.State() == SessionState::Ended, "wrong epoch cannot postpone twenty second timeout");
  MemoryTransport alone;
  CoopSession s(Config(Role::Host), alone);
  s.Tick(0); s.Tick(19.99);
  Check(s.State() == SessionState::Connecting, "handshake waits up to twenty seconds");
  s.Tick(20);
  Check(s.State() == SessionState::Ended, "handshake has finite deadline");
  Pair failed; failed.Pump(0, .4); failed.a.state = TransportState::Failed; failed.host.Tick(.5);
  Check(failed.host.State() == SessionState::Ended, "failed transport ends session");
}
void Poses() {
  PoseBuffer buffer;
  Check(!buffer.Sample(0), "empty interpolation buffer");
  PlayerPose a{1000, 101, 0, 2, 3, 3.10f, 4, 10}, b = a;
  b.time = 1000.2; b.x = 20; b.facing = -3.10f; b.action = 20;
  buffer.Push(a, 1); buffer.Push(b, 1.2);
  auto middle = buffer.Sample(1.2);
  Check(middle && Near(middle->x, 10) && std::abs(middle->facing) > 3,
      "100 ms interpolation, independent remote clock and shortest facing arc");
  buffer.Push(a, 1.25);
  Check(Near(buffer.Sample(1.2)->x, 10), "out of order sample ignored");
  Check(buffer.Sample(1.5)->x == 20 && !buffer.Sample(1.71), "holds without extrapolation then expires");
  b.time += 1; b.level = 202; b.x = 99; buffer.Push(b, 2);
  Check(buffer.Sample(2)->level == 202 && buffer.Sample(2)->x == 99, "level transition clears old samples");
  buffer.Clear(); Check(!buffer.Sample(2), "explicit clear");

  Pair pair; pair.Pump(0, .4);
  PlayerPose local{.5, 101, 5, 6, 7, .4f, 8, 9};
  pair.host.Tick(.5, local); pair.guest.Tick(.51);
  Check(pair.guest.Partner(.51) && Near(pair.guest.Partner(.51)->z, 7), "session pose reaches partner");
  const int count = pair.a.unreliable_poses;
  pair.host.Tick(.51, local);
  Check(pair.a.unreliable_poses == count, "pose rate capped at twenty Hz");
  // A malformed packet and old-epoch pose cannot change the interpolated partner.
  pair.b.incoming.push_back({1, 2, 3});
  Message stale; stale.type = MessageType::PlayerPose; stale.epoch = 55; stale.pose = local; stale.pose.x = 999;
  pair.b.incoming.push_back(Packet(stale)); pair.guest.Tick(.52);
  Check(pair.guest.Stats().malformed == 1 && Near(pair.guest.Partner(.52)->x, 5), "malformed counted, stale epoch dropped");
  Message newer = stale; newer.epoch = Config(Role::Host).epoch; newer.sequence = 500;
  newer.pose.time = .6; newer.pose.x = 50;
  pair.b.incoming.push_back(Packet(newer)); pair.guest.Tick(.6);
  newer.sequence = 499; newer.pose.time = .7; newer.pose.x = 999;
  pair.b.incoming.push_back(Packet(newer)); pair.guest.Tick(.61);
  Check(pair.guest.Partner(.8)->x == 50, "reordered pose sequence cannot overwrite newer state");
}
void Disabled() {
  MemoryTransport trap;
  int callbacks = 0;
  SessionConfig config; config.enabled = false;
  CoopSession session(config, trap, [&](SessionState) { ++callbacks; });
  for (double now : {0., 1., 4., 21., 1000.}) {
    session.Tick(now, PlayerPose{}); session.End(now); (void)session.Stats(); (void)session.Partner(now);
  }
  Check(session.State() == SessionState::Idle && callbacks == 0 && trap.calls == 0,
      "disabled: zero transport methods, callbacks, sends, polls or state transitions");
}
}  // namespace
int main() {
  Protocol(); Handshake(); Timeouts(); Poses(); Disabled();
  std::puts("co-op core tests passed");
}
