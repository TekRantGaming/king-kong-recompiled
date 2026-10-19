// Pure tests only. No guest memory, SDK, sockets, clocks or game executable.
#include "coop_enemies.h"
#include "coop_session.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

using namespace kk::coop;
using namespace kk::coop::enemies;
namespace {
void Check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
bool Near(float a, float b) { return std::abs(a-b) < .002f; }
EnemyWorld World(uint32_t serial = 1) { return {303, 3, serial}; }
EnemyState Enemy(uint64_t source = 123, uint32_t sequence = 0) {
  return {{source, sequence, EnemyFamily::Raptor}, 1, 2, 3, .5f, 50, 0x825E06B0, 20, 0};
}
std::vector<uint8_t> Packet(const Message& m) {
  std::vector<uint8_t> out; Check(Encode(m, out), "encode fixture"); return out;
}
Message Snapshot(EnemyState e, double time = 1000, uint32_t sequence = 10, uint32_t serial = 1) {
  Message m; m.type = MessageType::EnemySnapshot; m.sequence = sequence;
  m.enemies = {World(serial), time, {e}}; return m;
}
Message Event(EnemyState e, EnemyEventKind kind) {
  Message m; m.type = MessageType::EnemyEvent; m.enemy_event = {World(), kind, e}; return m;
}
void IdentityAndMatching() {
  const auto key = AuthoredKey(uint32_t(EnemyFamily::Raptor), 210.75f, 82.34f, -3.93f);
  Check(key != 0 && key == AuthoredKey(uint32_t(EnemyFamily::Raptor), 210.75f, 82.34f, -3.93f), "stable spawn signature");
  Check(key != AuthoredKey(uint32_t(EnemyFamily::Worm), 210.75f, 82.34f, -3.93f), "families distinguish authored identity");
  Check(key != AuthoredKey(uint32_t(EnemyFamily::Raptor), 212, 82.34f, -3.93f), "different spawn point");
  Check(AuthoredKey(1, 0, -0.f, 0) == AuthoredKey(1, 0, 0, 0), "canonical negative zero");
  Check(!AuthoredKey(1, std::numeric_limits<float>::quiet_NaN(), 0, 0) && !AuthoredKey(1, 1e6f, 0, 0), "invalid identity coordinates");
  auto one = SpawnId(key, 1592, 1, EnemyFamily::Raptor), two = SpawnId(key, 1592, 2, EnemyFamily::Raptor);
  Check(one != two && one != SpawnId(key, 1776, 1, EnemyFamily::Raptor), "wave generation and template selector");
  Check(!SpawnId(0, 1, 1, EnemyFamily::Raptor).source && !SpawnId(1, 1, 0, EnemyFamily::Raptor).source, "missing spawn provenance refuses");
  EnemyId authored{key, 0, EnemyFamily::Raptor};
  // Two machines have different heap values and table order but the same authored IDs.
  std::vector<Binding> host{{authored, 10, 20, 30, 40}, {one, 50, 60, 70, 80}};
  std::vector<Binding> guest{{one, 150, 160, 170, 180}, {authored, 110, 120, 130, 140}};
  Check(Match(authored, host)->actor == 10 && Match(authored, guest)->actor == 110, "match ignores heap and enumeration order");
  Check(!Match(two, guest), "missing wave leaves unmatched");
  guest.push_back(guest.back()); Check(!Match(authored, guest), "duplicate ID refuses all candidates"); guest.pop_back();
  guest[0].ai = guest[1].ai; Check(!Match(authored, guest), "shared AI refuses hold"); guest[0].ai = 160;
  guest[1].vars = 0; Check(!Match(authored, guest), "incomplete local identity refuses");
  Check(Family(0x8254BCB0) == EnemyFamily::Raptor && Family(0x82516508) == EnemyFamily::Worm &&
      Family(0x825FE790) == EnemyFamily::Scorpion, "explicit creature families");
  for (auto fn : {0x826718B8u, 0x8268D8E8u, 0x8268BF48u, 0x826F9980u, 0u})
    Check(Family(fn) == EnemyFamily::None, "hero, allies, KAnn, unknown excluded");
  Check(LifeOffset(EnemyFamily::Raptor) == 20 && LifeOffset(EnemyFamily::Worm) == 5392 &&
      LifeOffset(EnemyFamily::Scorpion) == 5792 && LifeOffset(EnemyFamily::None) == 0, "per-family health layout");
}
void Wire() {
  Message out;
  for (bool event : {false, true}) {
    auto m = event ? Event(Enemy(), EnemyEventKind::Died) : Snapshot(Enemy());
    auto bytes = Packet(m);
    Check(Decode(bytes, out) && Packet(out) == bytes, "enemy wire round trip");
    for (size_t n = 0; n < bytes.size(); ++n) {
      Message sentinel; sentinel.sequence = 77;
      Check(!Decode(std::span(bytes.data(), n), sentinel) && sentinel.sequence == 77, "all enemy truncations leave output unchanged");
    }
    bytes.push_back(0); Check(!Decode(bytes, out), "enemy trailing bytes rejected");
  }
  auto batch = Snapshot(Enemy()); batch.enemies.enemies.clear();
  for (size_t i = 0; i < kMaxEnemyBatch; ++i) batch.enemies.enemies.push_back(Enemy(i+1));
  auto bytes = Packet(batch);
  Check(bytes.size() == kHeaderBytes+22+53*kMaxEnemyBatch && bytes.size() <= kMaxPacketBytes && Decode(bytes, out), "maximum batch within datagram cap");
  batch.enemies.enemies.push_back(Enemy(999)); Check(!Encode(batch, bytes), "oversized local batch rejected");
  batch = Snapshot(Enemy()); batch.enemies.enemies.push_back(Enemy()); Check(!Encode(batch, bytes), "duplicate encode refused");
  batch = Snapshot(Enemy()); bytes = Packet(batch);
  bytes[kHeaderBytes+20] = 0xFF; bytes[kHeaderBytes+21] = 0xFF;
  Check(!Decode(bytes, out), "count checked before allocation");
  bytes = Packet(batch); bytes[kHeaderBytes+22+14] = 0x7F; bytes[kHeaderBytes+22+15] = 0xC0;
  Check(!Decode(bytes, out), "NaN wire position rejected");
  bytes = Packet(batch); bytes[kHeaderBytes+22+12] = 0xFF; bytes[kHeaderBytes+22+13] = 0xFF;
  Check(!Decode(bytes, out), "unknown family rejected");
  auto invalid = Enemy(); invalid.id.source = 0; batch = Snapshot(invalid); Check(!Encode(batch, bytes), "zero source rejected");
  batch = Snapshot(Enemy()); batch.enemies.time = std::numeric_limits<double>::infinity(); Check(!Encode(batch, bytes), "nonfinite sample clock rejected");
  batch = Snapshot(Enemy()); batch.enemies.world.serial = 0; Check(!Encode(batch, bytes), "zero load serial rejected");
  batch = Snapshot(Enemy()); batch.enemies.enemies[0].spawn_z = std::numeric_limits<float>::infinity();
  Check(!Encode(batch, bytes), "nonfinite authored spawn rejected");
  batch = Snapshot(Enemy()); bytes = Packet(batch);
  bytes[kHeaderBytes+22+41] = 0x7F; bytes[kHeaderBytes+22+42] = 0xC0;
  Check(!Decode(bytes, out), "nonfinite wire authored spawn rejected");
  for (auto kind : {EnemyEventKind::Spawned, EnemyEventKind::Died, EnemyEventKind::Removed}) {
    auto event = Event(Enemy(), kind); Check(Decode(Packet(event), out) && out.enemy_event.kind == kind, "every reliable lifecycle kind");
  }
  auto bad_event = Event(Enemy(), EnemyEventKind(255)); Check(!Encode(bad_event, bytes), "unknown lifecycle kind rejected");
  bytes = Packet(Event(Enemy(), EnemyEventKind::Spawned)); bytes[kHeaderBytes+12] = 0;
  Check(!Decode(bytes, out), "invalid wire lifecycle rejected");
  batch = Snapshot(Enemy()); bytes = Packet(batch);
  // Manufacture two identical records with a matching length/count to exercise decoder ambiguity.
  const std::vector<uint8_t> record(bytes.begin()+kHeaderBytes+22, bytes.end());
  bytes.insert(bytes.end(), record.begin(), record.end());
  const auto length = bytes.size()-kHeaderBytes;
  for (int i = 0; i < 4; ++i) bytes[20+i] = uint8_t(length >> (24-i*8));
  bytes[kHeaderBytes+21] = 2;
  Check(!Decode(bytes, out), "duplicate wire IDs refused");
}
void LifecycleAndInterpolation() {
  Channel sender;
  Frame frame{World(), 1000, {{Enemy()}}}; sender.Publish(frame);
  Check(sender.Event() && sender.Event()->kind == EnemyEventKind::Spawned, "initial reliable spawn");
  sender.SentEvent(); sender.Publish(frame); Check(!sender.Event(), "unchanged publish has no repeated event");
  frame.records[0].enemy.life = 0; frame.records[0].kind = EnemyEventKind::Died; sender.Publish(frame);
  Check(sender.Event()->kind == EnemyEventKind::Died, "death queued"); sender.SentEvent();
  frame.records[0].kind = EnemyEventKind::Removed; sender.Publish(frame);
  Check(sender.Event()->kind == EnemyEventKind::Removed, "removal queued"); sender.SentEvent();
  frame.records[0] = {Enemy()}; sender.Publish(frame);
  Check(!sender.Event() && sender.Snapshots().front().enemies.empty(), "old generation cannot resurrect");
  frame.records.push_back({Enemy(123, 1)}); sender.Publish(frame);
  Check(sender.Event()->enemy.id.sequence == 1, "pooled actor gets new generation");
  Channel receiver;
  auto a = Enemy(), b = a; a.facing = 3.1f; b.facing = -3.1f; b.x = 21;
  receiver.Receive(Snapshot(a), 1); receiver.Receive(Snapshot(b, 1000.2, 11), 1.2);
  auto sampled = receiver.Sample(1.2);
  Check(sampled.records.size() == 1 && Near(sampled.records[0].enemy.x, 11) && std::abs(sampled.records[0].enemy.facing) > 3,
      "100ms interpolation independent clocks shortest facing arc");
  Check(Near(sampled.records[0].latest.x, 21), "numeric diagnostics retain latest uninterpolated host position");
  receiver.Receive(Snapshot(a, 1000.1, 9), 1.21);
  Check(Near(receiver.Sample(1.2).records[0].enemy.x, 11), "old unreliable sequence ignored per entity");
  Check(receiver.Sample(1.8).records.empty(), "stale live state expires");
  receiver.Receive(Event(b, EnemyEventKind::Removed), 2);
  receiver.Receive(Snapshot(a, 1001, 12), 2.1); receiver.Receive(Event(a, EnemyEventKind::Spawned), 2.2);
  sampled = receiver.Sample(20);
  Check(sampled.records.size() == 1 && sampled.records[0].kind == EnemyEventKind::Removed && sampled.records[0].enemy.life == 0,
      "terminal event persists and wins over delayed live packets");
  receiver.Receive(Snapshot(Enemy(555), 1, 13, 2), 21);
  receiver.Receive(Snapshot(a, 1002, 14, 1), 21.01);
  Check(receiver.Sample(21.02).world.serial == 2 && receiver.Sample(21.02).records[0].enemy.id.source == 555, "late world cannot roll load backward");
  Channel batcher;
  frame = {World(), 1, {}};
  for (size_t i = 0; i < kMaxEnemies; ++i) frame.records.push_back({Enemy(i+1)});
  batcher.Publish(frame);
  const auto batches = batcher.Snapshots(); size_t total = 0;
  for (const auto& batch : batches) { total += batch.enemies.size(); Check(batch.enemies.size() <= kMaxEnemyBatch, "each batch capped"); }
  Check(total == kMaxEnemies, "large census not truncated to one packet");
  receiver.Clear();
  for (size_t i = 0; i <= kMaxEnemies; ++i) receiver.Receive(Snapshot(Enemy(i+1), 1, uint32_t(i+1)), 1);
  Check(receiver.Sample(1).records.size() == kMaxEnemies, "remote entity storage is bounded");
  receiver.Clear();
  receiver.Receive(Event(Enemy(), EnemyEventKind::Spawned), 1);
  Check(receiver.Sample(1).records.empty(), "spawn alone cannot authorize a stale live transform");
  receiver.Receive(Event(Enemy(), EnemyEventKind::Died), 1.1);
  Check(receiver.Sample(20).records[0].enemy.life == 0, "death before first unreliable snapshot persists");
  receiver.Receive(Snapshot(Enemy(), 1, 5), 21);
  Check(receiver.Sample(21).records[0].enemy.life == 0, "delayed first snapshot cannot resurrect a reliable death");
  receiver.Clear(); Check(receiver.Sample(0).records.empty(), "disconnect reset drops all remote memory");
}
struct Transport final : ICoopTransport {
  Transport* peer = nullptr;
  std::deque<std::vector<uint8_t>> queue;
  bool block_events = false;
  bool block_shots = false;
  size_t snapshots = 0, events = 0, shots = 0;
  bool Send(std::span<const uint8_t> bytes, bool reliable) override {
    Message m; Check(Decode(bytes, m), "session emits valid packets");
    if (m.type == MessageType::EnemyEvent) { Check(reliable, "lifecycle reliable"); if (block_events) return false; ++events; }
    if (m.type == MessageType::EnemySnapshot) { Check(!reliable, "movement unreliable"); ++snapshots; }
    if (m.type == MessageType::ShotIntent) { Check(reliable, "shots reliable"); if (block_shots) return false; ++shots; }
    if (peer) peer->queue.emplace_back(bytes.begin(), bytes.end()); return true;
  }
  bool Receive(std::vector<uint8_t>& out) override { if (queue.empty()) return false; out = std::move(queue.front()); queue.pop_front(); return true; }
  void Poll(double) override {}
  TransportState State() const override { return TransportState::Ready; }
  TransportStats Stats() const override { return {}; }
  void Close() override {}
};
void SessionAuthority() {
  Transport a, b; a.peer = &b; b.peer = &a;
  const Compatibility compatibility{kProtocolVersion, "test", "build", "gameplay"};
  CoopSession host({true, Role::Host, 123, compatibility}, a), guest({true, Role::Guest, 0, compatibility}, b);
  Frame frame{World(), 0, {{Enemy()}}};
  for (int i = 0; i < 40; ++i) { host.Tick(i*.01); guest.Tick(i*.01); }
  Check(host.State() == SessionState::Connected && guest.State() == SessionState::Connected, "enemy fixture handshake");
  a.block_events = true;
  for (int i = 40; i < 50; ++i) { frame.time = i*.01; host.Tick(frame.time, {}, &frame); guest.Tick(frame.time, {}, &frame); }
  Check(a.events == 0 && a.snapshots > 0 && b.events == 0 && b.snapshots == 0, "guest cannot publish authority; snapshots survive event backpressure");
  a.block_events = false;
  host.Tick(.5); guest.Tick(.5);
  Check(a.events == 1, "queued lifecycle drains even without a new game frame");
  for (int i = 51; i < 80; ++i) { frame.time = i*.01; host.Tick(frame.time, {}, &frame); guest.Tick(frame.time); }
  Check(a.events == 1 && guest.Enemies(.8).records.size() == 1 && host.Enemies(.8).records.empty(), "reliable spawn retries without duplication");
  Check(a.snapshots >= 4 && a.snapshots <= 6, "enemy stream around 12.5Hz");
  auto spoof = Event(Enemy(999), EnemyEventKind::Removed); spoof.epoch = 123;
  a.queue.push_back(Packet(spoof)); host.Tick(.81);
  Check(host.Enemies(.81).records.empty(), "host ignores guest authority messages");
  auto wrong_epoch = Snapshot(Enemy(999), 1, 100); wrong_epoch.epoch = 999;
  b.queue.push_back(Packet(wrong_epoch)); guest.Tick(.82);
  Check(guest.Enemies(.82).records.size() == 1, "wrong session epoch ignored");
  host.End(.83); guest.Tick(.84); Check(guest.Enemies(.84).records.empty(), "ended clears guest replicas");
  Transport disabled; CoopSession off({false, Role::Host, 1, compatibility}, disabled); off.Tick(0, {}, &frame);
  Check(disabled.events == 0 && disabled.snapshots == 0 && off.Enemies(0).records.empty(), "disabled inert");
}
void MatchingRetryAndTolerance() {
  auto host = Enemy(100); host.spawn_x = 10; host.spawn_y = 20; host.spawn_z = 30;
  Binding local{{101, 0, EnemyFamily::Raptor}, 10, 20, 30, 40};
  std::vector<MatchCandidate> candidates{{local, 10.1f, 20, 30, true}};
  MatchRetry retry;
  Check(!MatchAuthored(host, {}, retry.ToleranceAllowed(1)), "missing actor stays pending");
  Check(MatchAuthored(host, candidates, retry.ToleranceAllowed(4))->actor == 10, "late actor matches stable authored position within tolerance");
  host.x = 999; host.y = -999;
  Check(MatchAuthored(host, candidates, true), "fallback uses authored position, never moving snapshot position");
  candidates[0].x = 10.25f; Check(MatchAuthored(host, candidates, true), "inclusive tolerance boundary");
  candidates[0].x = 10.251f; Check(!MatchAuthored(host, candidates, true), "outside tolerance refuses");
  candidates[0].x = 10.1f;
  Check(!MatchAuthored(host, candidates, retry.ToleranceAllowed(11.01)), "tolerance retry expires after ten seconds");
  candidates[0].binding.id = host.id;
  Check(MatchAuthored(host, candidates, retry.ToleranceAllowed(30)), "exact authored ID can activate after retry window");
  candidates[0].binding.id = local.id;
  candidates.push_back({{{102, 0, EnemyFamily::Raptor}, 11, 21, 31, 41}, 9.9f, 20, 30, true});
  Check(!MatchAuthored(host, candidates, true), "two within radius refuse even if one is nearer");
  candidates[1].available = false;
  Check(!MatchAuthored(host, candidates, true), "reserved or initializing neighbour still blocks ambiguity");
  candidates.pop_back(); candidates[0].available = false;
  Check(!MatchAuthored(host, candidates, true), "uninitialized or reserved actor cannot bind");
  candidates[0].available = true; candidates[0].binding.id.sequence = 1;
  Check(!MatchAuthored(host, candidates, true), "wave cannot be claimed by authored fallback");
  candidates[0].binding.id = local.id; host.id.sequence = 1;
  Check(!MatchAuthored(host, candidates, true), "host wave never gets positional fallback");
  host.id.sequence = 0;
  candidates.push_back({{{999, 0, EnemyFamily::Worm}, 11, 20, 31, 41}, 100, 100, 100, true});
  Check(!MatchAuthored(host, candidates, true), "mutable AI alias refuses tolerance matching");
}
ShotIntent Shot(uint32_t number = 1, double time = 100) {
  ShotIntent s; s.world = World(); s.number = number; s.weapon = 1; s.time = time;
  s.direction = {1, 0, 0}; s.hits.push_back({Enemy().id, {10, 0, 0}}); return s;
}
void ShotWireAndBounds() {
  Message m; m.type = MessageType::ShotIntent; m.shot = Shot();
  for (bool miss : {true, false}) {
    m.shot = Shot(); if (miss) m.shot.hits.clear();
    const auto bytes = Packet(m); Message out;
    Check(Decode(bytes, out) && Packet(out) == bytes, "shot and miss round trip");
    for (size_t n = 0; n < bytes.size(); ++n) {
      Message sentinel; sentinel.sequence = 999;
      Check(!Decode(std::span(bytes.data(), n), sentinel) && sentinel.sequence == 999, "every shot truncation leaves output untouched");
    }
  }
  m.shot = Shot(); m.shot.weapon = 3; m.shot.hits.clear();
  for (size_t i = 0; i < kMaxShotHits; ++i) m.shot.hits.push_back({Enemy(i+1).id, {10, float(i)*.1f, 0}});
  Message out; auto bytes = Packet(m);
  Check(bytes.size() == kHeaderBytes+50+26*kMaxShotHits && bytes.size() <= kMaxPacketBytes && Decode(bytes, out), "maximum shotgun intent bounded");
  m.shot.hits.push_back({Enemy(999).id, {10, 0, 0}}); Check(!Encode(m, bytes), "too many shotgun impacts refused");
  m.shot = Shot(); m.shot.hits.push_back({Enemy(456).id, {10, 0, 0}}); Check(!Encode(m, bytes), "single ray weapon cannot send multiple targets");
  m.shot.weapon = 3; m.shot.hits[1].id = m.shot.hits[0].id; Check(!Encode(m, bytes), "repeated shotgun target refused");
  m.shot = Shot(); m.shot.weapon = 5; Check(!Encode(m, bytes), "unsupported weapon refused");
  m.shot = Shot(); m.shot.number = 0; Check(!Encode(m, bytes), "zero shot number refused");
  m.shot = Shot(); m.shot.world.serial = 0; Check(!Encode(m, bytes), "shot needs world visit");
  m.shot = Shot(); m.shot.direction = {}; Check(!Encode(m, bytes), "zero direction refused");
  m.shot = Shot(); m.shot.direction.x = 2; Check(!Encode(m, bytes), "unnormalized direction refused");
  m.shot = Shot(); m.shot.origin.z = std::numeric_limits<float>::quiet_NaN(); Check(!Encode(m, bytes), "nonfinite origin refused");
  m.shot = Shot(); m.shot.hits[0].point.x = 1e6f; Check(!Encode(m, bytes), "hit point range bound");
  m.shot = Shot(); m.shot.time = std::numeric_limits<double>::infinity(); Check(!Encode(m, bytes), "nonfinite shot time refused");
  m.shot = Shot(); bytes = Packet(m); bytes[kHeaderBytes+49] = 255;
  Check(!Decode(bytes, out), "wire count rejected before allocation");
  bytes = Packet(m); bytes[kHeaderBytes+25] = 0x7F; bytes[kHeaderBytes+26] = 0xC0;
  Check(!Decode(bytes, out), "wire nonfinite origin refused");
  bytes = Packet(m); bytes[kHeaderBytes+16] = 0;
  Check(!Decode(bytes, out), "wire weapon bound checked");
  bytes = Packet(m); bytes.push_back(0); Check(!Decode(bytes, out), "shot trailing byte refused");
}
void ShotValidation() {
  PlayerPose peer; peer.level = 303; peer.time = 100;
  const ReceivedShot in{Shot(), 1};
  auto valid = [&](ReceivedShot s, EnemyWorld world = World(), uint8_t weapon = 1,
                   std::optional<PlayerPose> p = PlayerPose{100, 303}, double now = 1.01) {
    ShotValidator v; return v.Validate(s, world, weapon, p, now);
  };
  Check(valid(in) == ShotReject::None, "different process clocks accepted using peer time and local arrival");
  Check(valid(in, World(2)) == ShotReject::World, "wrong visit rejected");
  Check(valid(in, World(), 2) == ShotReject::Weapon, "unowned weapon rejected");
  Check(valid(in, World(), 1, {}, 1.01) == ShotReject::Stale, "missing peer pose rejected");
  Check(valid(in, World(), 1, peer, 1.51) == ShotReject::Stale, "stale queue rejected");
  Check(valid(in, World(), 1, peer, .9) == ShotReject::Stale, "future queue timestamp rejected");
  auto nonfinite_peer = peer; nonfinite_peer.x = std::numeric_limits<float>::quiet_NaN();
  Check(valid(in, World(), 1, nonfinite_peer) == ShotReject::Stale, "nonfinite peer position rejected");
  auto bad = in; bad.intent.time = 98; Check(valid(bad) == ShotReject::Stale, "remote shot clock checked against pose");
  bad = in; bad.intent.origin.x = 4.01f; Check(valid(bad) == ShotReject::Origin, "origin too far from guest rejected");
  ShotValidator v;
  Check(v.Validate(in, World(), 1, peer, 1) == ShotReject::None, "first shot accepted");
  Check(v.Validate(in, World(), 1, peer, 1) == ShotReject::Sequence, "duplicate cannot damage twice");
  bad = {Shot(2, 100.01), 1.01};
  Check(v.Validate(bad, World(), 1, peer, 1.01) == ShotReject::Rate, "too-fast gun rejected");
  bad = {Shot(3, 100.2), 1.2};
  Check(v.Validate(bad, World(), 1, peer, 1.2) == ShotReject::None, "normal cadence accepted");
  Check(v.Validate(bad, World(), 1, peer, 1.2) == ShotReject::Sequence, "rejected/accepted sequence stays consumed");
  v = {}; auto burst = in;
  Check(v.Validate(burst, World(), 1, peer, 1) == ShotReject::None, "burst one");
  burst.intent = Shot(2, 100.2); Check(v.Validate(burst, World(), 1, peer, 1) == ShotReject::None, "burst two allows batching");
  burst.intent = Shot(3, 100.4); Check(v.Validate(burst, World(), 1, peer, 1) == ShotReject::Rate, "arrival rate blocks forged clock acceleration");
  auto target = Enemy(); target.x = 10; target.y = target.z = 0;
  Check(ValidateShotHit(in.intent, in.intent.hits[0], target) == ShotReject::None, "live nearby claimed hit accepted");
  target.life = 0; Check(ValidateShotHit(in.intent, in.intent.hits[0], target) == ShotReject::Target, "dead target rejected");
  target = Enemy(999); Check(ValidateShotHit(in.intent, in.intent.hits[0], target) == ShotReject::Target, "wrong generation/id rejected");
  target = Enemy(); target.x = 20; target.y = target.z = 0;
  Check(ValidateShotHit(in.intent, in.intent.hits[0], target) == ShotReject::Distance, "impact too far from host enemy rejected");
  auto s = Shot(); s.direction.x = -1; target.x = 10;
  Check(ValidateShotHit(s, s.hits[0], target) == ShotReject::Aim, "backward hit rejected");
  s = Shot(); s.hits[0].point.x = target.x = 301;
  Check(ValidateShotHit(s, s.hits[0], target) == ShotReject::Distance, "weapon range rejected");
}
void ShotSessions() {
  Transport a, b; a.peer = &b; b.peer = &a;
  const Compatibility compatible{kProtocolVersion, "test", "build", "gameplay"};
  CoopSession host({true, Role::Host, 123, compatible}, a), guest({true, Role::Guest, 0, compatible}, b);
  for (int i = 0; i < 40; ++i) { host.Tick(i*.01); guest.Tick(i*.01); }
  Check(!host.QueueShot(Shot(1, .4)), "host cannot send guest intents");
  Check(guest.QueueShot(Shot(1, .4)), "guest queues actual shot");
  b.block_shots = true; guest.Tick(.4); host.Tick(.4);
  Check(b.shots == 0 && host.TakeShots().empty(), "shot retained under backpressure");
  b.block_shots = false; guest.Tick(.41); host.Tick(.42);
  auto first = host.TakeShots();
  Check(b.shots == 1 && first.size() == 1 && guest.ShotsSent() == 1, "reliable shot retries once");
  ShotValidator validator; PlayerPose peer{.4, 303};
  Check(validator.Validate(first[0], World(), 1, peer, .42) == ShotReject::None, "received shot reaches authority validator");
  Message replay; replay.type = MessageType::ShotIntent; replay.epoch = 123; replay.shot = Shot(1, .4);
  a.queue.push_back(Packet(replay)); host.Tick(.43);
  auto duplicate = host.TakeShots();
  Check(duplicate.size() == 1 && validator.Validate(duplicate[0], World(), 1, peer, .43) == ShotReject::Sequence,
      "duplicate reaches counted game-thread rejection and cannot damage twice");
  replay.shot = Shot(2, .4); replay.epoch = 999; a.queue.push_back(Packet(replay)); host.Tick(.44);
  Check(host.TakeShots().empty(), "wrong epoch shot ignored");
  replay.epoch = 123; b.queue.push_back(Packet(replay)); guest.Tick(.45);
  Check(guest.TakeShots().empty(), "guest cannot receive authoritative shot requests");
  for (size_t i = 0; i < kMaxQueuedShots; ++i) Check(guest.QueueShot(Shot(uint32_t(i+2), .46)), "bounded outgoing queue capacity");
  Check(!guest.QueueShot(Shot(999, .46)), "outgoing queue refuses overflow");
  guest.Tick(1.1); host.Tick(1.1);
  Check(b.shots == 1 && host.TakeShots().empty(), "expired unsent shots discarded");
  ShotMailbox mailbox;
  for (size_t i = 0; i < kMaxQueuedShots; ++i) Check(mailbox.Submit(Shot(uint32_t(i+100), 1.1)), "mailbox capacity");
  Check(!mailbox.Submit(Shot(999, 1.1)), "mailbox overflow bounded");
  mailbox.Clear(); Check(mailbox.Submit(Shot(100, 1.1)), "mailbox reset");
  mailbox.Exchange(guest); guest.Tick(1.11); host.Tick(1.12); mailbox.Exchange(host);
  Check(mailbox.Take().size() == 1 && mailbox.Take().empty(), "worker/game shot handoff drains once");
  host.End(1.13); mailbox.Exchange(host); Check(mailbox.Take().empty(), "disconnected mailbox cleared");
}
void MissThenHit() {
  Transport a, b; a.peer = &b; b.peer = &a;
  const Compatibility compatible{kProtocolVersion, "test", "build", "gameplay"};
  CoopSession host({true, Role::Host, 123, compatible}, a), guest({true, Role::Guest, 0, compatible}, b);
  for (int i = 0; i < 40; ++i) { host.Tick(i*.01); guest.Tick(i*.01); }
  ShotValidator validator;
  for (uint32_t number : {1u, 2u}) {
    const double time = .4 + (number-1)*.2;
    auto shot = Shot(number, time);
    if (number == 1) shot.hits.clear();
    Check(guest.QueueShot(shot), "miss and following hit both queue");
    guest.Tick(time); host.Tick(time+.01);
    const auto received = host.TakeShots();
    Check(received.size() == 1 && guest.ShotsSent() == number, "miss counts as a sent intent, once");
    Check(received[0].intent.hits.size() == number-1, "miss never invents a target; following hit retains it");
    Check(validator.Validate(received[0], World(), 1, PlayerPose{time, 303}, time+.01) == ShotReject::None,
        "miss consumes cadence and sequence without blocking the next real hit");
  }
}
} // namespace
int main() {
  IdentityAndMatching(); Wire(); LifecycleAndInterpolation(); SessionAuthority();
  MatchingRetryAndTolerance(); ShotWireAndBounds(); ShotValidation(); ShotSessions(); MissThenHit();
  std::puts("coop enemies tests passed");
}
