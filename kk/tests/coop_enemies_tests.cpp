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
  Check(bytes.size() == kHeaderBytes+22+41*kMaxEnemyBatch && bytes.size() <= kMaxPacketBytes && Decode(bytes, out), "maximum batch within datagram cap");
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
  size_t snapshots = 0, events = 0;
  bool Send(std::span<const uint8_t> bytes, bool reliable) override {
    Message m; Check(Decode(bytes, m), "session emits valid packets");
    if (m.type == MessageType::EnemyEvent) { Check(reliable, "lifecycle reliable"); if (block_events) return false; ++events; }
    if (m.type == MessageType::EnemySnapshot) { Check(!reliable, "movement unreliable"); ++snapshots; }
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
} // namespace
int main() { IdentityAndMatching(); Wire(); LifecycleAndInterpolation(); SessionAuthority(); std::puts("coop enemies tests passed"); }
