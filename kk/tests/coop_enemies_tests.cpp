// Pure tests only. No guest memory, SDK, sockets, clocks or game executable.
#include "coop_enemies.h"
#include "coop_ownership.h"
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
  Check(!SpawnerTemplateKey(key,0x40001000,0,0),"unresolved pre-init template cannot fabricate provenance");
  Check(SpawnerTemplateKey(key,0x40001000,0x40001000,0x40002000)==
      SpawnerTemplateKey(key,0x50003000,0x50003000,0x50004000),
      "late-resolved templates on different heaps retain the same numeric slot identity");
  Check(SpawnerTemplateKey(key,0x40002000,0x40001000,0x40002000)!=
      SpawnerTemplateKey(key,0x40001000,0x40001000,0x40002000),"distinct live template selectors remain distinct");
  Check(!SpawnerTemplateKey(key,0x40003000,0x40001000,0x40002000),"replaced or unknown template is refused");
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
  Check(Family(0x82623058) == EnemyFamily::Crab && Family(0x8264DA70) == EnemyFamily::Crab &&
      Family(0x82641928) == EnemyFamily::Crab && LifeOffset(EnemyFamily::Crab) == 760,
      "chapter two crab tag/init/reflex and native firearm life are supported");
  auto crab = Enemy(); crab.id.family = EnemyFamily::Crab;
  Message decoded;
  Check(Decode(Packet(Snapshot(crab)), decoded) && decoded.enemies.enemies[0].id.family == EnemyFamily::Crab,
      "crab identity crosses snapshots");
  Check(Decode(Packet(Event(crab, EnemyEventKind::Died)), decoded), "crab confirmed death crosses reliable events");
  for (auto fn : {0x826718B8u, 0x8268D8E8u, 0x8268BF48u, 0x826F9980u, 0u})
    Check(Family(fn) == EnemyFamily::None, "hero, allies, KAnn, unknown excluded");
  Check(LifeOffset(EnemyFamily::Raptor) == 20 && LifeOffset(EnemyFamily::Worm) == 5392 &&
      LifeOffset(EnemyFamily::Scorpion) == 5444 && SecondaryLifeOffset(EnemyFamily::Scorpion) == 5792 &&
      LifeBytes(EnemyFamily::Scorpion) == 5796 && LifeOffset(EnemyFamily::None) == 0, "per-family health layout");
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
  Check(bytes.size() == kHeaderBytes+22+kEnemyStateBytes*kMaxEnemyBatch && bytes.size() <= kMaxPacketBytes && Decode(bytes, out), "maximum batch within datagram cap");
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
void ScorpionDamagePools() {
  auto a = Enemy(); a.id.family = EnemyFamily::Scorpion; a.life = 30; a.secondary_life = 5;
  auto b = a; b.life = 24; b.secondary_life = 3;
  Message decoded;
  Check(Decode(Packet(Snapshot(b)), decoded) && Near(decoded.enemies.enemies[0].life, 24) &&
      Near(decoded.enemies.enemies[0].secondary_life, 3), "scorpion snapshots carry both native damage pools");
  Channel receiver;
  receiver.Receive(Snapshot(a), 1); receiver.Receive(Snapshot(b, 1000.2, 11), 1.2);
  const auto sampled = receiver.Sample(1.2).records[0].enemy;
  Check(Near(sampled.life, 24) && Near(sampled.secondary_life, 3), "interpolation never restores an older damage pool");
  auto bad = b; bad.secondary_life = std::numeric_limits<float>::quiet_NaN();
  Check(!ValidEnemy(bad), "nonfinite secondary health refused");
  bad = Enemy(); bad.secondary_life = 5;
  Check(!ValidEnemy(bad), "secondary health is scorpion-only");
  Authority owner, guest; owner.last = guest.last = a; owner.matched = true;
  Check(owner.Observe(b), "owner observes scorpion firearm and secondary damage");
  const auto prepare = owner.Begin(World(), EnemyOwner::Guest, b); Check(prepare.has_value(), "scorpion handover starts");
  const auto ready = guest.Prepare(*prepare, a); Check(ready.has_value(), "replica accepts damaged owner sample");
  const auto commit = owner.Ready(*ready);
  Check(commit && Near(commit->enemy.life, 24) && Near(commit->enemy.secondary_life, 3), "handover preserves both damaged pools");
  Message wire; wire.type = MessageType::OwnershipChange; wire.epoch = 1; wire.ownership = *commit;
  Check(Decode(Packet(wire), decoded) && Near(decoded.ownership.enemy.secondary_life, 3), "handover wire retains secondary health");
  auto malformed = Packet(Snapshot(b)); malformed[malformed.size()-4] = 0x7f; malformed[malformed.size()-3] = 0xc0;
  Check(!Decode(malformed, decoded), "nonfinite wire secondary health refused");
  Check(owner.Commit(*commit) && guest.Commit(*commit), "both sides commit damaged scorpion");
  auto final = guest.last; final.life = 18; final.secondary_life = 2;
  const auto back = owner.Begin(World(), EnemyOwner::Host, owner.last);
  const auto final_ready = guest.Prepare(*back, final);
  const auto final_commit = owner.Ready(*final_ready);
  Check(final_commit && Near(final_commit->enemy.life, 18) && Near(final_commit->enemy.secondary_life, 2),
      "return to host preserves final guest-native damage in both pools");
}
struct Transport final : ICoopTransport {
  Transport* peer = nullptr;
  std::deque<std::vector<uint8_t>> queue;
  bool block_events = false;
  bool block_shots = false;
  bool block_inventory = false;
  bool block_ownership = false;
  size_t snapshots = 0, events = 0, shots = 0, inventories = 0;
  bool Send(std::span<const uint8_t> bytes, bool reliable) override {
    Message m; Check(Decode(bytes, m), "session emits valid packets");
    if (m.type == MessageType::OwnershipChange) { Check(reliable, "ownership reliable"); if (block_ownership) return false; }
    if (m.type == MessageType::EnemyEvent) { Check(reliable, "lifecycle reliable"); if (block_events) return false; ++events; }
    if (m.type == MessageType::EnemySnapshot) { Check(!reliable, "movement unreliable"); ++snapshots; }
    if (m.type == MessageType::ShotIntent) { Check(reliable, "shots reliable"); if (block_shots) return false; ++shots; }
    if (m.type == MessageType::InventoryState) { Check(reliable, "inventory reliable"); if (block_inventory) return false; ++inventories; }
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
InventoryState Inventory(double time = 100, uint8_t weapon = 1, uint32_t ammo = 80) {
  InventoryState i{World(), time, weapon}; i.ammo[weapon-1] = ammo; return i;
}
PendingShot Pending(ShotIntent shot) { return {shot, Inventory(shot.time, shot.weapon)}; }
void GuestInventory() {
  Message m; m.type = MessageType::InventoryState; m.inventory = Inventory(100, 2, 999);
  auto bytes = Packet(m); Message out;
  Check(bytes.size() == kHeaderBytes+37 && Decode(bytes, out) && out.inventory.weapon == 2 &&
      out.inventory.ammo[1] == 999 && out.inventory.world == World(), "guest inventory codec round trip");
  auto unchanged = m.inventory; unchanged.time += .1;
  Check(SameInventory(m.inventory, unchanged), "unchanged inventory does not spam reliable updates");
  unchanged.ammo[1]--; Check(!SameInventory(m.inventory, unchanged), "ammo consumption triggers an update");
  for (size_t n = 0; n < bytes.size(); ++n) {
    Message sentinel; sentinel.sequence = 999;
    Check(!Decode(std::span(bytes.data(), n), sentinel) && sentinel.sequence == 999, "inventory truncation preserves output");
  }
  bytes.push_back(0); Check(!Decode(bytes, out), "inventory trailing byte refused");
  m.inventory.weapon = 5; Check(!Encode(m, bytes), "inventory weapon bound");
  m.inventory = Inventory(); m.inventory.ammo[0] = UINT32_MAX; Check(!Encode(m, bytes), "inventory ammo bound");
  m.inventory = Inventory(); m.inventory.time = std::numeric_limits<double>::quiet_NaN();
  Check(!Encode(m, bytes), "inventory nonfinite time refused");
  m.inventory = Inventory(); bytes = Packet(m); bytes[kHeaderBytes+20] = 5;
  Check(!Decode(bytes, out), "wire inventory bounds checked");
  m.inventory = {World(), 100, 0}; bytes = Packet(m);
  Check(Decode(bytes, out) && out.inventory.weapon == 0, "empty loadout can be reported");

  PlayerPose peer{100, 303};
  auto shot = Shot(); shot.weapon = 2;
  ReceivedShot received{shot, 1, Inventory(100, 2, 1)};
  ShotValidator validator;
  Check(validator.Validate(received, World(), peer, 1) == ShotReject::None,
      "guest machine gun accepted with its own last round independent of host weapon");
  received.intent.number = 2; received.intent.time = 100.1;
  Check(validator.Validate(received, World(), peer, 1.1) == ShotReject::Weapon, "same inventory cannot spend its last round twice");
  received.inventory = Inventory(100.1, 2, 30); received.intent.number = 3;
  Check(validator.Validate(received, World(), peer, 1.1) == ShotReject::None, "reported reload supplies guest ammo");
  auto rejects = [&](std::optional<InventoryState> inventory) {
    ShotValidator v; return v.Validate({shot, 1, inventory}, World(), peer, 1);
  };
  Check(rejects({}) == ShotReject::Weapon, "missing guest inventory rejected");
  Check(rejects(Inventory(100, 1)) == ShotReject::Weapon, "shot must match guest selected weapon");
  Check(rejects(Inventory(100, 2, 0)) == ShotReject::Weapon, "empty guest weapon rejected");
  Check(rejects(Inventory(99, 2)) == ShotReject::Weapon, "stale guest inventory rejected");
  Check(rejects(Inventory(100.1, 2)) == ShotReject::Weapon, "post-fire future inventory rejected");
  auto wrong = Inventory(100, 2); wrong.world = World(2);
  Check(rejects(wrong) == ShotReject::Weapon, "other checkpoint inventory rejected");

  Transport a, b; a.peer = &b; b.peer = &a;
  const Compatibility compatible{kProtocolVersion, "test", "build", "gameplay"};
  CoopSession authority({true, Role::Host, 123, compatible}, a), guest({true, Role::Guest, 0, compatible}, b);
  for (int i = 0; i < 40; ++i) { authority.Tick(i*.01); guest.Tick(i*.01); }
  authority.PublishInventory(Inventory(.4)); authority.Tick(.4);
  Check(a.inventories == 1, "host inventory is available to guest-owned enemies");
  b.block_inventory = true;
  guest.PublishInventory(Inventory(.4)); guest.Tick(.4);
  Check(b.inventories == 0, "inventory change retained under backpressure");
  b.block_inventory = false; guest.Tick(.41); authority.Tick(.41);
  Check(b.inventories == 1, "inventory change retries without firing");
  guest.PublishInventory(Inventory(.42)); guest.Tick(.42);
  Check(b.inventories == 1, "identical inventory is coalesced regardless of sample time");
  guest.PublishInventory(Inventory(.43, 2, 30)); guest.Tick(.43); authority.Tick(.43);
  Check(b.inventories == 2, "weapon switch reports without firing");
  Check(guest.QueueShot(Pending(Shot(1, .44))), "pre-fire inventory accompanies shot after a weapon switch");
  guest.Tick(.44); authority.Tick(.44);
  const auto first = authority.TakeShots();
  Check(first.size() == 1 && first[0].inventory && first[0].inventory->weapon == 1,
      "shot captures its ordered pre-fire snapshot instead of a later inventory");
  auto mismatch = Pending(Shot(2, .45)); mismatch.inventory.world = World(2);
  ShotMailbox mailbox;
  Check(!guest.QueueShot(mismatch) && !mailbox.Submit(mismatch), "inconsistent inventory cannot poison either queue");
  authority.End(.46); Check(authority.TakeShots().empty(), "session end drops remote shot inventory copies");
}
void ShotWireAndBounds() {
  Message receipt; receipt.type=MessageType::ShotReceipt; receipt.shot_receipt={World(),1,ShotReject::Engine,false};
  auto receipt_bytes=Packet(receipt); Message receipt_out;
  Check(receipt_bytes.size()==kHeaderBytes+18 && Decode(receipt_bytes,receipt_out),"bounded host verdict wire");
  for(size_t n=0;n<receipt_bytes.size();++n)
    Check(!Decode(std::span(receipt_bytes).first(n),receipt_out),"every host verdict truncation rejected");
  auto invalid_receipt=receipt_bytes; invalid_receipt.back()=2;
  Check(!Decode(invalid_receipt,receipt_out),"host verdict queued flag is boolean");
  invalid_receipt=receipt_bytes; invalid_receipt[invalid_receipt.size()-2]=uint8_t(ShotReject::Count);
  Check(!Decode(invalid_receipt,receipt_out),"host verdict unknown reason refused");
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
  Check(bytes.size() == kHeaderBytes+50+30*kMaxShotHits && bytes.size() <= kMaxPacketBytes && Decode(bytes, out), "maximum shotgun intent bounded");
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
  const ReceivedShot in{Shot(), 1, Inventory()};
  auto valid = [&](ReceivedShot s, EnemyWorld world = World(), uint8_t weapon = 1,
                   std::optional<PlayerPose> p = PlayerPose{100, 303}, double now = 1.01) {
    if (s.inventory) s.inventory->weapon = weapon;
    ShotValidator v; return v.Validate(s, world, p, now);
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
  Check(v.Validate(in, World(), peer, 1) == ShotReject::None, "first shot accepted");
  Check(v.Validate(in, World(), peer, 1) == ShotReject::Sequence, "duplicate cannot damage twice");
  bad = {Shot(2, 100.01), 1.01, Inventory(100.01)};
  Check(v.Validate(bad, World(), peer, 1.01) == ShotReject::Rate, "too-fast gun rejected");
  bad = {Shot(3, 100.2), 1.2, Inventory(100.2)};
  Check(v.Validate(bad, World(), peer, 1.2) == ShotReject::None, "normal cadence accepted");
  Check(v.Validate(bad, World(), peer, 1.2) == ShotReject::Sequence, "rejected/accepted sequence stays consumed");
  v = {}; auto burst = in;
  Check(v.Validate(burst, World(), peer, 1) == ShotReject::None, "burst one");
  burst.intent = Shot(2, 100.2); Check(v.Validate(burst, World(), peer, 1) == ShotReject::None, "burst two allows batching");
  burst.intent = Shot(3, 100.4); Check(v.Validate(burst, World(), peer, 1) == ShotReject::None, "WAN batching accepts third legal shot");
  for (unsigned i=4;i<=6;++i) { burst.intent=Shot(i,100+.2*(i-1)); burst.inventory=Inventory(burst.intent.time);
    peer.time=burst.intent.time; Check(v.Validate(burst,World(),peer,1)==ShotReject::None,"bounded WAN burst"); }
  burst.intent=Shot(7,101.2); burst.inventory=Inventory(101.2); peer.time=101.2;
  Check(v.Validate(burst,World(),peer,1)==ShotReject::Rate,"arrival rate still bounds forged sender clock");
  auto target = Enemy(); target.x = 10; target.y = target.z = 0;
  Check(ValidateShotHit(in.intent, in.intent.hits[0], target) == ShotReject::None, "live nearby claimed hit accepted");
  target.life = 0; Check(ValidateShotHit(in.intent, in.intent.hits[0], target) == ShotReject::Late, "confirmed dead target counted late");
  target.x = 20;
  Check(ValidateShotHit(in.intent, in.intent.hits[0], target) == ShotReject::Distance, "dead target still requires valid hit geometry");
  ShotCounters counters; counters.Reject(ShotReject::Late);
  Check(counters.late == 1 && counters.rejected == 0 && counters.last == ShotReject::None, "late is separate from rejection totals");
  target = Enemy(999); Check(ValidateShotHit(in.intent, in.intent.hits[0], target) == ShotReject::Target, "wrong generation/id rejected");
  target = Enemy(); target.x = 20; target.y = target.z = 0;
  Check(ValidateShotHit(in.intent, in.intent.hits[0], target) == ShotReject::Distance, "impact too far from host enemy rejected");
  auto s = Shot(); s.direction.x = -1; target.x = 10;
  Check(ValidateShotHit(s, s.hits[0], target) == ShotReject::Aim, "backward hit rejected");
  s = Shot(); s.hits[0].point.x = target.x = 301;
  Check(ValidateShotHit(s, s.hits[0], target) == ShotReject::Distance, "weapon range rejected");
}
void WanShooterHistory() {
  PoseBuffer poses;
  poses.Push(PlayerPose{1000,303,0,0,0},10);
  poses.Push(PlayerPose{1000.25,303,8,0,0},10.25);
  poses.Push(PlayerPose{1000.5,303,16,0,0},10.5);
  auto shot=Shot(1,1000); const auto history=poses.At(shot.time,303);
  ShotValidator validator;
  Check(history && validator.Validate({shot,10.5,Inventory(1000),history},World(),history,10.51)==ShotReject::None,
      "delayed shot validates at historical shooter origin across independent PC clocks");
  validator={};
  Check(validator.Validate({shot,10.5,Inventory(1000)},World(),poses.Sample(10.5),10.51)==ShotReject::Origin,
      "old latest-pose path reproduces moving shooter WAN origin rejection");
  Check(!poses.At(998,303) && !poses.At(1000,101),"historical shooter lookup refuses old or wrong-level poses");
  auto crab=Enemy(); crab.id.family=EnemyFamily::Crab; crab.x=10; crab.y=crab.z=0;
  shot.hits[0].id=crab.id;
  Check(ValidateShotHit(shot,shot.hits[0],crab)==ShotReject::None,"host validates crab shot hit");
  ShotTargetHistory targets; targets.Observe(crab,10);
  crab.x=25;
  Check(ValidateShotHit(shot,shot.hits[0],crab)==ShotReject::Distance &&
      targets.Validate(shot,shot.hits[0],crab,10.6)==ShotReject::None,
      "moving enemy validates at recent host snapshot without widening impact radius");
  Check(targets.Validate(shot,shot.hits[0],crab,10.8)==ShotReject::Distance,
      "old enemy history cannot authorize a hit");
  crab.generation=1;
  Check(targets.Validate(shot,shot.hits[0],crab,10.6)==ShotReject::Target,"history cannot cross authority generation");
  crab.generation=0; crab.life=0;
  Check(targets.Validate(shot,shot.hits[0],crab,10.6)==ShotReject::Late,"history retains confirmed native death");
}
void ShotSessions() {
  Transport a, b; a.peer = &b; b.peer = &a;
  const Compatibility compatible{kProtocolVersion, "test", "build", "gameplay"};
  CoopSession host({true, Role::Host, 123, compatible}, a), guest({true, Role::Guest, 0, compatible}, b);
  for (int i = 0; i < 40; ++i) { host.Tick(i*.01); guest.Tick(i*.01); }
  auto invalid = Pending(Shot(1, .4)); invalid.intent.number = 0;
  Check(!host.QueueShot(invalid), "host also validates outgoing intents");
  Check(guest.QueueShot(Pending(Shot(1, .4))), "guest queues actual shot");
  b.block_inventory = true; guest.Tick(.395); host.Tick(.395);
  Check(b.shots == 0 && host.TakeShots().empty(), "inventory backpressure prevents intent overtaking it");
  b.block_inventory = false;
  b.block_shots = true; guest.Tick(.4); host.Tick(.4);
  Check(b.shots == 0 && host.TakeShots().empty(), "shot retained under backpressure");
  b.block_shots = false; guest.Tick(.41); host.Tick(.42);
  auto first = host.TakeShots();
  Check(b.shots == 1 && first.size() == 1 && guest.ShotsSent() == 1, "reliable shot retries once");
  ShotValidator validator; PlayerPose peer{.4, 303};
  Check(first[0].inventory && first[0].inventory->ammo[0] == 80, "reliable pre-fire inventory precedes intent");
  Check(validator.Validate(first[0], World(), peer, .42) == ShotReject::None, "received shot reaches authority validator");
  Check(host.QueueShotReceipt({World(),1,ShotReject::Origin,false}) &&
      !guest.QueueShotReceipt({World(),1,ShotReject::None,true}),"only host can publish shot verdicts");
  host.Tick(.425); guest.Tick(.426);
  auto receipts=guest.TakeShotReceipts();
  Check(receipts.size()==1 && receipts[0].number==1 && receipts[0].reason==ShotReject::Origin &&
      !receipts[0].queued && guest.TakeShotReceipts().empty(),"guest receives host reject reason once");
  Message receipt; receipt.type=MessageType::ShotReceipt; receipt.epoch=123; receipt.shot_receipt={World(),2,ShotReject::None,true};
  a.queue.push_back(Packet(receipt)); host.Tick(.427);
  Check(host.TakeShotReceipts().empty(),"guest cannot forge host shot confirmations");
  Message replay; replay.type = MessageType::ShotIntent; replay.epoch = 123; replay.shot = Shot(1, .4);
  a.queue.push_back(Packet(replay)); host.Tick(.43);
  auto duplicate = host.TakeShots();
  Check(duplicate.size() == 1 && validator.Validate(duplicate[0], World(), peer, .43) == ShotReject::Sequence,
      "duplicate reaches counted game-thread rejection and cannot damage twice");
  replay.shot = Shot(2, .4); replay.epoch = 999; a.queue.push_back(Packet(replay)); host.Tick(.44);
  Check(host.TakeShots().empty(), "wrong epoch shot ignored");
  replay.epoch = 123; b.queue.push_back(Packet(replay)); guest.Tick(.45);
  Check(guest.TakeShots().size() == 1, "guest can receive requests for its owned enemies");
  for (size_t i = 0; i < kMaxQueuedShots; ++i) Check(guest.QueueShot(Pending(Shot(uint32_t(i+2), .46))), "bounded outgoing queue capacity");
  Check(!guest.QueueShot(Pending(Shot(999, .46))), "outgoing queue refuses overflow");
  guest.Tick(1.1); host.Tick(1.1);
  Check(b.shots == 1 && host.TakeShots().empty(), "expired unsent shots discarded");
  ShotMailbox mailbox;
  for (size_t i = 0; i < kMaxQueuedShots; ++i) Check(mailbox.Submit(Pending(Shot(uint32_t(i+100), 1.1))), "mailbox capacity");
  Check(!mailbox.Submit(Pending(Shot(999, 1.1))), "mailbox overflow bounded");
  mailbox.Clear(); Check(mailbox.Submit(Pending(Shot(100, 1.1))), "mailbox reset");
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
    Check(guest.QueueShot(Pending(shot)), "miss and following hit both queue");
    guest.Tick(time); host.Tick(time+.01);
    const auto received = host.TakeShots();
    Check(received.size() == 1 && guest.ShotsSent() == number, "miss counts as a sent intent, once");
    Check(received[0].intent.hits.size() == number-1, "miss never invents a target; following hit retains it");
    Check(validator.Validate(received[0], World(), PlayerPose{time, 303}, time+.01) == ShotReject::None,
        "miss consumes cadence and sequence without blocking the next real hit");
  }
}
void OwnershipDecision() {
  Proximity p;
  Check(!p.Decide(EnemyOwner::Host, 40, 10, 0, true), "closer guest starts dwell");
  Check(!p.Decide(EnemyOwner::Host, 40, 10, 1, true), "exactly one second cannot transfer");
  Check(p.Decide(EnemyOwner::Host, 40, 10, 1.01, true) == EnemyOwner::Guest, "guest clearly closer for over one second");
  Check(!p.Decide(EnemyOwner::Guest, 11, 10, 2, true) &&
        !p.Decide(EnemyOwner::Guest, 9, 10, 4, true), "hysteresis holds through equal-distance jitter");
  Check(!p.Decide(EnemyOwner::Guest, 7, 10, 5, true) &&
        p.Decide(EnemyOwner::Guest, 7, 10, 6.01, true) == EnemyOwner::Host, "closer host takes back after dwell");
  Check(!p.Decide(EnemyOwner::Host, 100, 25, 10, true) &&
        !p.Decide(EnemyOwner::Host, 100, 26, 12, true), "25-unit grant radius is strict");
  Check(!p.Decide(EnemyOwner::Host, 40, 10, 20, true), "new dwell");
  Check(!p.Decide(EnemyOwner::Host, 40, 10, 21, false) &&
        !p.Decide(EnemyOwner::Host, 40, 10, 21.1, true), "ineligible sample resets dwell");
  Check(!p.Decide(EnemyOwner::Host, 40, 10, 19, true), "backward clock restarts dwell");
  Check(!p.Decide(EnemyOwner::Host, std::numeric_limits<float>::infinity(), 1, 30, true), "invalid distance resets dwell");
  Check(!p.Decide(EnemyOwner::Guest, 29, 31, 40, true) &&
        p.Decide(EnemyOwner::Guest, 29, 31, 41.01, true) == EnemyOwner::Host, "out-of-range guest returns authority with dwell");
}
void OwnershipWire() {
  for (auto phase : {OwnershipPhase::Matched, OwnershipPhase::Prepare, OwnershipPhase::Ready, OwnershipPhase::Commit}) {
    Message m; m.type = MessageType::OwnershipChange;
    m.ownership = {World(), phase, phase == OwnershipPhase::Matched ? 0u : 1u,
                   phase == OwnershipPhase::Matched ? EnemyOwner::Host : EnemyOwner::Guest, Enemy()};
    if (phase == OwnershipPhase::Commit) { m.ownership.enemy.owner = EnemyOwner::Guest; m.ownership.enemy.generation = 1; }
    auto bytes = Packet(m); Message out;
    Check(bytes.size() == 104 && Decode(bytes, out) && Packet(out) == bytes, "ownership phase round trip with final state");
    for (size_t n = 0; n < bytes.size(); ++n) {
      Message sentinel; sentinel.sequence = 77;
      Check(!Decode(std::span(bytes.data(), n), sentinel) && sentinel.sequence == 77, "ownership truncations preserve output");
    }
    bytes[kHeaderBytes+12] = 255; Check(!Decode(bytes, out), "unknown ownership phase rejected");
    bytes = Packet(m); bytes[kHeaderBytes+17] = 2; Check(!Decode(bytes, out), "unknown owner rejected");
    bytes = Packet(m); bytes.push_back(0); Check(!Decode(bytes, out), "ownership trailing byte rejected");
    m.ownership.enemy.x = std::numeric_limits<float>::quiet_NaN(); Check(!Encode(m, bytes), "invalid handover transform refused");
  }
  Message m; m.type = MessageType::OwnershipChange;
  m.ownership = {World(), OwnershipPhase::Prepare, 0, EnemyOwner::Guest, Enemy()};
  std::vector<uint8_t> bytes;
  Check(!Encode(m, bytes), "prepare needs a new generation");
  m.ownership.generation = 2; Check(!Encode(m, bytes), "skipped prepare generation refused");
  m.ownership.generation = 1; m.ownership.owner = EnemyOwner::Host;
  Check(!Encode(m, bytes), "prepare must change the owner");
  m.ownership.owner = EnemyOwner::Guest; m.ownership.world.serial = 0;
  Check(!Encode(m, bytes), "handover requires checkpoint scope");
  m.type = MessageType::ShotIntent; m.shot = Shot(); m.shot.hits[0].generation = 123;
  Message decoded; Check(Decode(Packet(m), decoded) && decoded.shot.hits[0].generation == 123, "shot generation crosses wire");
}
void OwnershipSequencing() {
  Authority host, guest; host.last = guest.last = Enemy();
  Check(!host.Begin(World(), EnemyOwner::Guest, Enemy()), "unmatched cannot transfer");
  host.matched = true;
  auto unsupported = Enemy(); unsupported.id.family = EnemyFamily::None;
  Check(!host.Begin(World(), EnemyOwner::Guest, unsupported), "unknown family cannot transfer");
  const ShotHit initial{Enemy().id, {10, 0, 0}, 0};
  Check(RoutesShot(host, EnemyOwner::Host, initial) && !RoutesShot(host, EnemyOwner::Guest, initial), "default host shot route");
  const auto prepare = host.Begin(World(), EnemyOwner::Guest, Enemy());
  Check(prepare && host.Frozen() && !host.Runs(EnemyOwner::Host), "old host freezes before sending prepare");
  Check(!RoutesShot(host, EnemyOwner::Host, initial), "freeze refuses damage");
  auto wrong_local = Enemy(); wrong_local.x = 999;
  const auto ready = guest.Prepare(*prepare, wrong_local);
  Check(ready && ready->enemy.x == Enemy().x && guest.Frozen(), "nonowner seeds host final pose, ignores divergent local copy");
  const auto commit = host.Ready(*ready);
  Check(commit && host.owner == EnemyOwner::Host && !guest.Runs(EnemyOwner::Guest), "Ready cannot grant authority");
  auto stale = *commit; stale.generation = stale.enemy.generation = 99;
  Check(!guest.Commit(stale), "unrequested future generation refused");
  Check(host.Commit(*commit) && guest.Commit(*commit), "only Commit changes both owners");
  Check(!guest.Commit(*commit) && !guest.Prepare(*prepare, Enemy()), "duplicate commit and old prepare cannot rewind");
  Check(host.owner == EnemyOwner::Guest && guest.Runs(EnemyOwner::Guest) && !host.Runs(EnemyOwner::Host), "exactly one native AI runs");
  Check(combat::FilterVictim(4, 10, 20, 10, true, !guest.Runs(EnemyOwner::Guest), false) == combat::VictimAction::Pass,
      "guest-owned native hit on guest Jack passes");
  Check(combat::FilterVictim(4, 10, 20, 10, true, !host.Runs(EnemyOwner::Host), false) == combat::VictimAction::SuppressDuplicate,
      "host replica cannot hurt host Jack");
  Check(combat::FilterVictim(4, 20, 20, 10, true, false, false) == combat::VictimAction::ProtectBody,
      "borrowed-body protection remains in force");
  auto hit = initial; hit.generation = 1;
  Check(RoutesShot(guest, EnemyOwner::Guest, hit) && !RoutesShot(host, EnemyOwner::Host, hit) &&
        !RoutesShot(guest, EnemyOwner::Guest, initial), "shots follow owner and expire across generations");
  auto final = guest.last; final.x = 42; final.life = 37;
  Check(guest.Observe(final), "guest observes native movement and damage");
  const auto back = host.Begin(World(), EnemyOwner::Host, host.last);
  Check(back.has_value(), "host requests return using latest received pose");
  const auto guest_ready = guest.Prepare(*back, final);
  Check(guest_ready && guest_ready->enemy.x == 42 && guest_ready->enemy.life == 37, "old guest supplies final native state");
  const auto return_commit = host.Ready(*guest_ready);
  Check(return_commit && return_commit->enemy.x == 42 && !host.Runs(EnemyOwner::Host), "new host stays held until commit of guest final state");
  Check(host.Commit(*return_commit) && guest.Commit(*return_commit) && host.Runs(EnemyOwner::Host), "return handover completes");
  auto dead = host.last; dead.life = 0;
  Check(host.Observe(dead) && !host.Begin(World(), EnemyOwner::Guest, dead), "authoritative death is final");
  auto live = dead; live.life = 50;
  Check(!host.Observe(live), "live sample cannot resurrect corpse");

  // Last hit on the old guest just before it sees Prepare cancels the flip.
  Authority h, g; h.last = g.last = commit->enemy; h.owner = g.owner = EnemyOwner::Guest;
  h.generation = g.generation = 1; h.matched = true;
  const auto death_prepare = h.Begin(World(), EnemyOwner::Host, h.last);
  auto last_hit = g.last; last_hit.life = 0;
  const auto death_ready = g.Prepare(*death_prepare, last_hit);
  const auto death_commit = h.Ready(*death_ready);
  Check(death_commit && death_commit->owner == EnemyOwner::Guest && death_commit->enemy.life == 0, "handover death keeps old owner");
  Check(h.Commit(*death_commit) && g.Commit(*death_commit) && h.dead && g.dead, "handover publishes death once on both sides");
  Authority overflow; overflow.matched = true; overflow.last = Enemy();
  overflow.generation = overflow.last.generation = UINT32_MAX;
  Check(!overflow.Begin(World(), EnemyOwner::Guest, overflow.last), "authority generations never wrap");
}
void OwnershipStreams() {
  Channel channel;
  auto old = Enemy(); channel.Receive(Snapshot(old, 1, 1), 1);
  auto current = old; current.owner = EnemyOwner::Guest; current.generation = 1; current.x = 40;
  channel.Receive(Snapshot(current, 2, 2), 2);
  old.life = 0; auto late = Event(old, EnemyEventKind::Died); late.sequence = 3;
  channel.Receive(late, 2.01);
  const auto sample = channel.Sample(2.02);
  Check(sample.records.size() == 1 && sample.records[0].enemy.generation == 1 && sample.records[0].enemy.x == 40 &&
        sample.records[0].enemy.life > 0, "late former-owner death cannot poison new history");
  channel.Clear(); channel.Publish({World(), 1, {{Enemy()}}});
  Check(channel.Event(), "initial local event queued");
  channel.Publish({World(), 2, {}});
  Check(!channel.Event() && channel.Snapshots()[0].enemies.empty(), "former owner drops queued events and snapshots");
}
void OwnershipSessions() {
  Transport a, b; a.peer = &b; b.peer = &a;
  const Compatibility compatible{kProtocolVersion, "test", "build", "gameplay"};
  CoopSession host({true, Role::Host, 123, compatible}, a), guest({true, Role::Guest, 0, compatible}, b);
  for (int i = 0; i < 40; ++i) { host.Tick(i*.01); guest.Tick(i*.01); }
  Message prepare; prepare.type = MessageType::OwnershipChange; prepare.epoch = 123;
  prepare.ownership = {World(), OwnershipPhase::Prepare, 1, EnemyOwner::Guest, Enemy()};
  Check(!guest.QueueOwnership(prepare) && host.QueueOwnership(prepare), "only host proposes ownership");
  a.block_ownership = true; host.Tick(.4); guest.Tick(.4);
  Check(guest.TakeOwnership().empty() && host.TakeOwnership().empty(), "prepare waits for reliable backpressure");
  a.block_ownership = false; host.Tick(.41); guest.Tick(.42);
  Check(guest.TakeOwnership().size() == 1 && guest.TakeOwnership().empty(), "prepare drains once");
  Message ready = prepare; ready.ownership.phase = OwnershipPhase::Ready;
  Check(!host.QueueOwnership(ready) && guest.QueueOwnership(ready), "only guest acknowledges freeze");
  guest.Tick(.43); host.Tick(.44); Check(host.TakeOwnership().size() == 1, "ready returns to host");
  auto commit = prepare; commit.ownership.phase = OwnershipPhase::Commit;
  commit.ownership.enemy.owner = EnemyOwner::Guest; commit.ownership.enemy.generation = 1;
  Check(host.QueueOwnership(commit), "host queues commit");
  a.block_ownership = true; host.Tick(.45);
  Check(host.TakeOwnership().empty(), "queueing or blocked send cannot grant local authority");
  a.block_ownership = false; host.Tick(.46); guest.Tick(.47);
  Check(host.TakeOwnership().size() == 1 && guest.TakeOwnership().size() == 1, "successful reliable commit reaches both game threads");
  auto wrong = commit; wrong.epoch = 999; Check(!host.QueueOwnership(wrong), "wrong epoch cannot queue transfer");
  b.queue.push_back(Packet(wrong)); guest.Tick(.48); Check(guest.TakeOwnership().empty(), "wrong epoch cannot change owner");
  // Guest-owned movement and native outcomes flow back to the host.
  auto e = commit.ownership.enemy; e.x = 45;
  Frame frame{World(), .5, {{e}}}; guest.Tick(.5, {}, &frame); host.Tick(.51);
  Check(host.Enemies(.51).records.size() == 1 && host.Enemies(.51).records[0].enemy.owner == EnemyOwner::Guest,
      "guest-owned snapshots arrive at host");
  e.life = 0; frame = {World(), .6, {{e, EnemyEventKind::Died}}}; guest.Tick(.6, {}, &frame); host.Tick(.61);
  Check(host.Enemies(.61).records[0].enemy.life == 0, "guest native death is reliable authority");
  auto shot = Shot(1, .62); shot.hits[0].generation = 1;
  Check(host.QueueShot(Pending(shot)), "host queues shot for current guest owner");
  host.Tick(.62); guest.Tick(.63); const auto requests = guest.TakeShots();
  Check(requests.size() == 1 && requests[0].inventory && requests[0].intent.hits[0].generation == 1,
      "host shot and pre-fire inventory reach guest together");
  ShotValidator validator;
  Check(validator.Validate(requests[0], World(), PlayerPose{.62, 303}, .63) == ShotReject::None,
      "guest uses same remote shot validation as host");
  OwnershipMailbox mailbox; Check(mailbox.Submit(prepare), "bounded ownership mailbox accepts values");
  mailbox.Exchange(host); host.Tick(.64); guest.Tick(.65); mailbox.Exchange(guest);
  Check(mailbox.Take().size() == 1 && mailbox.Take().empty(), "ownership worker/game handoff drains once");
  host.End(.66); mailbox.Exchange(host); Check(mailbox.Take().empty(), "disconnect clears ownership handoff");
}
} // namespace
int main() {
  ScorpionDamagePools();
  IdentityAndMatching(); Wire(); LifecycleAndInterpolation(); SessionAuthority();
  MatchingRetryAndTolerance(); GuestInventory(); ShotWireAndBounds(); ShotValidation(); ShotSessions(); MissThenHit();
  WanShooterHistory();
  OwnershipDecision(); OwnershipWire(); OwnershipSequencing(); OwnershipStreams(); OwnershipSessions();
  std::puts("coop enemies tests passed");
}
