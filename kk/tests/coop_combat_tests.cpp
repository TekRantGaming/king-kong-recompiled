// Source-only handoff. No game memory, native calls, SDK, sockets or wall clock.
#include "coop_session.h"
#include "coop_stimulus_debug.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <utility>

using namespace kk::coop;
using namespace kk::coop::combat;
namespace {
void Check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
EnemyWorld World(uint32_t visit = 1) { return {303, 3, visit}; }
Message Damage(uint32_t number = 1, uint64_t life = 11) {
  Message m; m.type = MessageType::DamageEvent; m.epoch = 90;
  m.damage = {World(), life, number, {123, 0, EnemyFamily::Raptor}, 16388, 6, {1, 0, 0}, 10}; return m;
}
Message Status(uint32_t number = 1, uint64_t life = 11, PlayerCondition condition = PlayerCondition::Alive) {
  Message m; m.type = MessageType::PlayerStatus; m.epoch = 90;
  m.player_status = {World(), life, number, 0, condition}; return m;
}
std::vector<uint8_t> Packet(const Message& m) {
  std::vector<uint8_t> out; Check(Encode(m, out), "fixture encodes"); return out;
}
void StimulusDiagnostics() {
  using namespace stimulus_debug;
  constexpr uint32_t entries = 0x41000000, handle = 0x42000000, body = 0x43000000, source = 0x44000000;
  std::map<uint32_t, uint32_t> words{
      {kPools+16, entries}, {kPools+24, 1}, {kPools+28, 2},
      {handle, 0x01000000}, {handle+4, 7}, {entries, 1}, {entries+112, 7},
      {entries+8, source}, {entries+12, body}};
  auto u32 = [&](uint32_t p) { return words.at(p); }; // any unbounded read fails the fixture
  auto r = ReadHandle(u32, handle);
  Check(r.valid && !r.enumerable && r.source == source && r.target == body,
      "queued pending stimulus is valid but not enumerated; source is not victim");
  words[entries] = 0; r = ReadHandle(u32, handle);
  Check(r.valid && r.enumerable, "native update exposes pending stimulus");
  words[handle+4] = 6;
  Check(!ReadHandle(u32, handle).valid, "recycled stimulus generation refused");
  words[handle+4] = 7; words[entries+112] = 0; words[entries] = 6;
  Check(!ReadHandle(u32, handle).valid, "expired free-list record refused");
  words[entries+112] = 7; words[entries] = 0; words[handle] = 0x01000001;
  Check(!ReadHandle(u32, handle).valid, "index equal to count refused before dereference");
  words[handle] = UINT32_MAX;
  Check(!ReadHandle(u32, handle).valid, "invalid native handle cannot address a pool");
  Check(!ReadHandle(u32, handle+1).valid && !ReadHandle(u32, 0x9FFFFFFC).valid,
      "unaligned and overflowing handle spans refused");
  words[kPools+24] = 3;
  Check(!ReadPool(u32, 1).valid, "pool count beyond capacity refused");
  words[kPools+24] = 1; words[kPools+16] = 0x9FFFFFFC;
  Check(!ReadPool(u32, 1).valid, "record span overflow refused");
}
void Codecs() {
  static_assert(kProtocolVersion == 8);
  for (const auto& m : {Damage(), Status()}) {
    const auto bytes = Packet(m); Message out;
    Check(bytes.size() == (m.type == MessageType::DamageEvent ? 90u : 53u), "fixed bounded wire length");
    Check(bytes[4] == 0 && bytes[5] == 8 && bytes[7] == uint16_t(m.type), "protocol 8 byte order");
    Check(Decode(bytes, out) && Packet(out) == bytes, "canonical combat round trip");
    for (size_t n = 0; n < bytes.size(); ++n) {
      out.type = MessageType::Bye;
      Check(!Decode(std::span<const uint8_t>(bytes.data(), n), out) && out.type == MessageType::Bye, "truncation preserves output");
    }
    auto extra = bytes; extra.push_back(0); extra[23]++;
    Check(!Decode(extra, out), "trailing combat payload refused");
  }
  auto m = Damage(); std::vector<uint8_t> out{9};
  for (auto flags : {0u, 1u, 3u, 0xFFFFFFFFu, 0x4005u}) {
    m.damage.flags = flags; Check(!Encode(m, out) && out == std::vector<uint8_t>{9}, "unaudited flags refuse atomically");
  }
  m = Damage(); m.damage.magnitude = 1000; Check(!Encode(m, out), "flag magnitude pair must match producer");
  for (const auto& pair : {std::pair{4096u, 1u}, {16644u, 1000u}, {16388u, 10u}, {16386u, 6u}, {16388u, 6u}}) {
    auto valid = Damage(); valid.damage.flags = pair.first; valid.damage.magnitude = pair.second;
    const auto wire = Packet(valid); Message decoded;
    Check(Decode(wire, decoded) && decoded.damage.flags == pair.first && decoded.damage.magnitude == pair.second,
        "every audited native raptor hit pair survives codec");
  }
  m.damage.flags = 16644; Check(Encode(m, out), "audited strong raptor hit allowed");
  for (auto magnitude : {0u, 1001u, UINT32_MAX}) {
    m.damage.magnitude = magnitude; Check(!Encode(m, out), "integer magnitude bounded");
  }
  m = Damage(); m.damage.direction.x = std::numeric_limits<float>::quiet_NaN(); Check(!Encode(m, out), "NaN direction refused");
  m.damage.direction = {0, 0, 0}; Check(!Encode(m, out), "zero direction refused");
  m.damage.direction = {2, 0, 0}; Check(!Encode(m, out), "unbounded direction refused");
  m = Damage(); m.damage.enemy.family = EnemyFamily::Worm; Check(!Encode(m, out), "unaudited family refused");
  m = Damage(); m.damage.enemy.source = 0; Check(!Encode(m, out), "missing enemy identity refused");
  m = Damage(); m.damage.incarnation = 0; Check(!Encode(m, out), "missing life refused");
  m = Damage(); m.damage.number = 0; Check(!Encode(m, out), "zero damage sequence refused");
  m = Damage(); m.damage.world.serial = 0; Check(!Encode(m, out), "missing visit refused");
  m = Damage(); m.damage.time = std::numeric_limits<double>::infinity(); Check(!Encode(m, out), "nonfinite hit time refused");
  m.damage.time = -1; Check(!Encode(m, out), "negative hit time refused");
  m = Status(); m.player_status.condition = PlayerCondition(0); Check(!Encode(m, out), "invalid condition refused");
  m.player_status.condition = PlayerCondition(4); Check(!Encode(m, out), "unknown condition refused");
  m = Status(); m.player_status.number = 0; Check(!Encode(m, out), "zero status sequence refused");
  m = Status(); m.player_status.incarnation = 0; Check(!Encode(m, out), "zero status life refused");
  auto wire = Packet(Damage()); wire[69] = 7; Message decoded; // last byte of magnitude
  Check(!Decode(wire, decoded), "malformed wire magnitude refused");
  wire = Packet(Status()); wire.back() = 99; Check(!Decode(wire, decoded), "malformed wire condition refused");
}
void Replay() {
  Check(DamageClock(10000, 10000.1) && DamageClock(9999, 10000) && DamageClock(10000.5, 10000), "fresh hits use host clock independently of guest clock");
  Check(!DamageClock(9998.99, 10000) && !DamageClock(10000.51, 10000), "delayed and future native hits refused against host pose");
  DamageReplay replay; replay.Reset(90, World(), 11);
  Check(replay.Accept(Damage(1), 10, 10), "first hit accepted");
  Check(!replay.Accept(Damage(1), 10, 10.1), "duplicate cannot enqueue even after native immunity or pool refusal");
  Check(replay.Accept(Damage(3), 10, 10.1) && !replay.Accept(Damage(2), 10, 10.2), "older ordered hit refused");
  auto m = Damage(4); m.epoch++;
  Check(!replay.Accept(m, 10, 10.2), "old session refused");
  m = Damage(4); m.damage.world = World(2); Check(!replay.Accept(m, 10, 10.2), "other checkpoint visit refused");
  Check(!replay.Accept(Damage(4, 12), 10, 10.2), "other incarnation refused");
  Check(!replay.Accept(Damage(4), 10, 11.01), "expired game-thread handoff refused");
  Check(!replay.Accept(Damage(4), 11, 10), "future arrival refused");
  Check(replay.Last() == 3 && replay.Accept(Damage(4), 10, 11), "bad inputs never advance replay; age bound inclusive");
  replay.Reset(90, World(), 12);
  Check(!replay.Accept(Damage(5, 11), 10, 10) && replay.Accept(Damage(1, 12), 10, 10), "revive life invalidates old hits and restarts sequence");
  Check(replay.Accept(Damage(UINT32_MAX, 12), 10, 10) && !replay.Accept(Damage(1, 12), 10, 10), "damage sequence never wraps within a life");
  StatusReplay status; status.Reset(90, World());
  Check(status.Accept(Status(), 10, 10), "first status accepted");
  Check(!status.Accept(Status(), 10, 10), "duplicate status refused");
  auto p = Status(2, 11, PlayerCondition::Hurt); p.player_status.last_damage = 3;
  Check(status.Accept(p, 10, 10), "hurt and processed acknowledgement accepted");
  p = Status(3); Check(!status.Accept(p, 10, 10), "acknowledgement cannot regress");
  p = Status(3, 11, PlayerCondition::Dead); p.player_status.last_damage = 3;
  Check(status.Accept(p, 10, 10), "death accepted");
  p = Status(4); p.player_status.last_damage = 3;
  Check(!status.Accept(p, 10, 10), "death cannot silently become alive in the same life");
  Check(status.Accept(Status(1, 12), 10, 10) && !status.Accept(Status(99, 11), 10, 10), "new life replaces old status only forwards");
  p = Status(2, 12); p.epoch++; Check(!status.Accept(p, 10, 10), "wrong status epoch refused");
  p = Status(2, 12); p.player_status.world = World(2); Check(!status.Accept(p, 10, 10), "wrong status world refused");
  Check(!status.Accept(Status(2, 12), 10, 12), "stale status refused");
}
void VictimsAndLease() {
  Check(FilterVictim(4, 20, 20, 10, false, true, false) == VictimAction::ProtectBody, "leased body never accepts native type-4 damage");
  Check(FilterVictim(4, 20, 20, 10, false, false, false) == VictimAction::ProtectBody, "unknown attackers cannot injure authored body");
  Check(FilterVictim(4, 10, 20, 10, true, true, false) == VictimAction::SuppressDuplicate, "guest matched enemy local hit filtered");
  Check(FilterVictim(4, 10, 20, 10, true, true, true) == VictimAction::Pass, "forwarded native hit passes once");
  Check(FilterVictim(4, 10, 20, 10, true, false, false) == VictimAction::Pass, "unmatched enemy and local hazards preserved");
  Check(FilterVictim(4, 30, 20, 10, true, true, false) == VictimAction::Pass, "other story victim preserved");
  Check(FilterVictim(4, 10, 20, 10, false, true, false) == VictimAction::Pass, "host Jack still takes normal damage");
  Check(FilterVictim(1, 20, 20, 10, false, true, false) == VictimAction::Pass, "perception is not a damage event");
  Check(FilterVictim(4, 0, 0, 0, true, true, false) == VictimAction::Pass, "zero actor cannot alias a victim");
  Check(CandidateReference(6244, 1) && CandidateReference(6320, 20), "only occupied audited raptor candidate actors allowed");
  for (auto offset : {0u, 5100u, 5216u, 6240u, 6245u, 6324u, 19812u})
    Check(!CandidateReference(offset, 20), "hero, handle, unknown and mission fields still refuse");
  Check(!CandidateReference(6244, 0) && !CandidateReference(6248, 1) && !CandidateReference(6244, 21), "invalid/unoccupied candidate slots refuse");
}
void Deaths() {
  DeathPolicy fallback;
  Check(fallback.Step(false, false, 1) == DeathAction::None, "living players unchanged");
  Check(fallback.Step(true, false, 2) == DeathAction::ReloadBoth, "production lone-death C24 fallback retained");
  Check(fallback.Step(true, true, 2) == DeathAction::ReloadBoth, "both dead reload immediately");
  DeathPolicy future;
  Check(future.Step(true, false, 10, true) == DeathAction::Wait && future.Step(true, false, 12.99, true) == DeathAction::Wait,
      "an audited future revive must wait three seconds");
  Check(future.Step(true, true, 13, true) == DeathAction::ReloadBoth, "both dead always wins over due revive");
  Check(future.Step(false, false, 14, true) == DeathAction::None && future.Step(true, false, 20, true) == DeathAction::Wait,
      "new death starts a fresh timer");
  Check(future.Step(true, false, 23, true) == DeathAction::Revive, "future safe revive delay bound");
  Check(future.Step(true, false, 1, true) == DeathAction::Wait, "clock rollback restarts timer");
}
struct Transport : ICoopTransport {
  Transport* peer = nullptr;
  bool blocked = false;
  unsigned combat_sent = 0;
  std::deque<std::vector<uint8_t>> incoming;
  bool Send(std::span<const uint8_t> bytes, bool reliable) override {
    Message m; Check(Decode(bytes, m), "transport sees valid packets");
    if (IsCombatMessage(m.type)) { Check(reliable, "combat uses reliable transport"); if (blocked) return false; ++combat_sent; }
    if (peer) peer->incoming.emplace_back(bytes.begin(), bytes.end()); return true;
  }
  bool Receive(std::vector<uint8_t>& out) override {
    if (incoming.empty()) return false; out = std::move(incoming.front()); incoming.pop_front(); return true;
  }
  void Poll(double) override {}
  TransportState State() const override { return TransportState::Ready; }
  TransportStats Stats() const override { return {}; }
  void Close() override {}
};
SessionConfig Config(Role role) {
  SessionConfig c; c.enabled = true; c.role = role; c.epoch = role == Role::Host ? 90 : 0;
  c.compatibility = {kProtocolVersion, "test", "test", "none"}; return c;
}
struct Pair {
  Transport a, b;
  CoopSession host{Config(Role::Host), a}, guest{Config(Role::Guest), b};
  Pair() { a.peer = &b; b.peer = &a; for (int i = 0; i < 10; ++i) Tick(i*.01); }
  void Tick(double now) { host.Tick(now); guest.Tick(now); }
};
void Handoff() {
  Pair p; Check(p.host.State() == SessionState::Connected && p.guest.Epoch() == 90, "session epoch learned");
  Check(!p.guest.QueueCombat(Damage(), .1) && !p.host.QueueCombat(Status(), .1), "wrong-role outgoing rejected");
  auto stale = Damage(); stale.epoch = 91;
  Check(!p.host.QueueCombat(stale, .1), "wrong-epoch outgoing rejected");
  CombatMailbox host_box, guest_box;
  Check(host_box.Submit(Damage(), .1), "host submits bounded value hit");
  host_box.Exchange(p.host, .1); p.Tick(.11); guest_box.Exchange(p.guest, .11);
  auto delivered = guest_box.Take();
  Check(delivered.size() == 1 && delivered[0].message.damage.number == 1, "hit crosses common handoff");
  Check(guest_box.Submit(Status(), .12), "guest submits observed status");
  guest_box.Exchange(p.guest, .12); p.Tick(.13); p.Tick(.14); host_box.Exchange(p.host, .14);
  Check(host_box.Take().size() == 1, "status returns through same handoff");
  p.a.blocked = true;
  Check(p.host.QueueCombat(Damage(2), .2), "blocked reliable hit queued"); p.Tick(.2);
  Check(p.a.combat_sent == 1, "send refusal does not lose reliable queue head");
  p.a.blocked = false; p.Tick(.3);
  Check(p.a.combat_sent == 2 && p.guest.TakeCombat().size() == 1, "backpressure retries once");
  p.a.blocked = true; Check(p.host.QueueCombat(Damage(3), .3), "expiry fixture queued"); p.Tick(.4);
  p.a.blocked = false; p.Tick(1.31);
  Check(p.a.combat_sent == 2 && p.guest.TakeCombat().empty(), "stale queued hits expire before send");
  for (uint32_t i = 0; i < kQueueLimit; ++i) Check(host_box.Submit(Damage(i+10), 2), "mailbox bounded admission");
  Check(!host_box.Submit(Damage(99), 2), "mailbox cap refuses backpressure");
  host_box.Clear(); host_box.Exchange(p.host, 2); p.Tick(2);
  Check(p.a.combat_sent == 2, "load clear discards unsent mailbox");
  Check(host_box.Submit(Damage(99), 2), "stale epoch fixture");
  stale = Damage(); stale.epoch = 100; Check(host_box.Submit(stale, 2), "replacement session fixture");
  host_box.Exchange(p.host, 3.01); p.Tick(3.01);
  Check(p.a.combat_sent == 2, "expired and foreign-session mailbox entries discarded");
  // Wrong-role incoming packets must not reach game memory, even if well formed.
  p.b.incoming.push_back(Packet(Status())); p.a.incoming.push_back(Packet(Damage())); p.Tick(3.02);
  Check(p.host.TakeCombat().empty() && p.guest.TakeCombat().empty(), "wrong-role incoming refused");
  p.a.incoming.push_back(Packet(Status())); p.host.Tick(3.03); p.host.End(3.04);
  Check(p.host.TakeCombat().empty(), "end clears incoming combat");
}
void StatusCoalescingAndCaps() {
  Pair p; CombatMailbox box;
  for (uint32_t n = 1; n <= 70; ++n) {
    auto m = Status(n); m.player_status.last_damage = n;
    Check(p.guest.QueueCombat(m, .1+n*.01), "status heartbeat queued");
    p.Tick(.1+n*.01); p.host.Tick(.101+n*.01); box.Exchange(p.host, .101+n*.01);
  }
  const auto latest = box.Take();
  Check(p.host.State() == SessionState::Connected && latest.size() == 1 && latest[0].message.player_status.number == 70,
      "paused game thread retains newest heartbeat without overflow");
  auto dead = Status(71, 11, PlayerCondition::Dead); dead.player_status.last_damage = 70;
  Check(p.guest.QueueCombat(dead, .82), "death status queued");
  p.Tick(.82); p.host.Tick(.83); box.Exchange(p.host, .83);
  auto invalid = Status(72); invalid.player_status.last_damage = 70;
  Check(p.guest.QueueCombat(invalid, .84), "invalid native resurrection fixture queued");
  p.Tick(.84); p.host.Tick(.85); box.Exchange(p.host, .85);
  auto status = box.Take(); Check(status.size() == 1 && status[0].message.player_status.condition == PlayerCondition::Dead,
      "coalescing cannot erase a death in the same life");
  for (uint32_t n = 1; n <= kQueueLimit; ++n) Check(p.host.QueueCombat(Damage(n), 1), "session damage cap fixture");
  Check(!p.host.QueueCombat(Damage(99), 1), "session reliable damage queue bounded");
  Pair overflow;
  for (uint32_t n = 1; n <= kQueueLimit; ++n) overflow.b.incoming.push_back(Packet(Damage(n)));
  overflow.guest.Tick(1); // Retain incoming without the game mailbox draining it.
  Check(overflow.guest.TakeCombat().size() == kQueueLimit, "incoming bound admitted exactly");
  for (uint32_t n = 1; n <= kQueueLimit+1; ++n) overflow.b.incoming.push_back(Packet(Damage(n)));
  overflow.guest.Tick(1.1);
  Check(overflow.guest.State() == SessionState::Ended && overflow.guest.TakeCombat().empty(), "reliable incoming overflow fails closed and clears");
  Pair lost; CombatMailbox lost_box;
  Check(lost_box.Submit(Damage(), .1), "loss clear fixture");
  lost.host.Tick(4);
  Check(lost.host.State() == SessionState::Lost, "missing peer poses timeout before end");
  lost_box.Exchange(lost.host, 4);
  lost.a.combat_sent = 0; lost_box.Exchange(lost.guest, 4);
  Check(lost_box.Take().empty() && lost.a.combat_sent == 0, "loss clears handoff");
}
} // namespace
int main() {
  StimulusDiagnostics(); Codecs(); Replay(); VictimsAndLease(); Deaths(); Handoff(); StatusCoalescingAndCaps();
  std::puts("co-op combat tests passed");
}
