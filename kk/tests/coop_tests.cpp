// No game, SDK, windows, sockets, sleeps or wall-clock dependencies.
#include "coop_protocol.h"
#include "coop_session.h"
#include "coop_pose.h"
#include "coop_puppet.h"
#include "coop_animation_fire.h"
#include "cheat_rules.h"
#include "coop_partner_selection.h"
#include "coop_nametag.h"

#include <bit>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <string_view>
#include <utility>

using namespace kk::coop;
namespace {
void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
bool Near(double a, double b) { return std::abs(a - b) < .002; }
Compatibility Compatible() { return {kProtocolVersion, "test-version", "test-build", "test-gameplay"}; }
SessionConfig Config(Role role) {
  SessionConfig config{true, role, role == Role::Host ? 123456789ull : 0, Compatible()};
  config.display_name = role == Role::Host ? "Host" : "Guest";
  return config;
}
std::vector<uint8_t> Packet(Message m) {
  std::vector<uint8_t> out;
  Check(Encode(m, out), "encode test packet"); return out;
}
struct MemoryTransport final : ICoopTransport {
  MemoryTransport* peer = nullptr;
  std::deque<std::vector<uint8_t>> incoming;
  bool deliver = true;
  bool block_cheats = false;
  mutable int calls = 0;
  int unreliable_poses = 0;
  TransportState state = TransportState::Ready;
  bool Send(std::span<const uint8_t> bytes, bool reliable) override {
    ++calls;
    Message m;
    if (Decode(bytes, m) && m.type == MessageType::Cheats) {
      Check(reliable, "host cheats reliable"); if (block_cheats) return false;
    }
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
  static_assert(kProtocolVersion == 25);
  constexpr std::array types{MessageType::Hello, MessageType::HelloAck, MessageType::Reject,
      MessageType::Ping, MessageType::Pong, MessageType::PlayerPose, MessageType::Bye,
      MessageType::EnemySnapshot, MessageType::EnemyEvent, MessageType::ShotIntent,
      MessageType::Cheats, MessageType::InventoryState, MessageType::SessionStart,
      MessageType::SaveChunk, MessageType::LevelChange, MessageType::Loaded,
      MessageType::LoadRelease, MessageType::Checkpoint, MessageType::ChapterFinished,
      MessageType::ReloadRequest, MessageType::FlowAbort, MessageType::DamageEvent, MessageType::PlayerStatus,
      MessageType::OwnershipChange,
      MessageType::PlayerDowned, MessageType::PlayerRevived,
      MessageType::CameraPose, MessageType::CameraCut, MessageType::StoryEvent, MessageType::TriggerRequest,
      MessageType::WorldDigest, MessageType::ShotReceipt, MessageType::StartPlan, MessageType::StartPlanAck, MessageType::ObjectState, MessageType::ObjectRequest};
  for (size_t i = 0; i < types.size(); ++i) {
    const auto type = types[i];
    Check(uint16_t(type) == i+1, "protocol 22 message IDs have no collisions or gaps");
    Check(IsFlowMessage(type) == ((i >= 12 && i <= 20) || i == 24 || i == 25 || i == 32 || i == 33), "flow includes life events and excludes combat and cameras");
    Message input;
    input.type = type; input.sequence = 0x12345678; input.epoch = 0x0123456789ABCDEFull;
    input.hello = Compatible(); input.reason = RejectReason::GameplayMods;
    input.clock_ms = 0x123456789ABCDEFull;
    input.pose = {12.75, 1402, 1.25f, -9.5f, 33.75f, 2.5f, 7.125f, 65535, 255};
    input.display_name = "Host\xC3\xA9";
    const EnemyState enemy{{123, 0, EnemyFamily::Raptor}, 1, 2, 3, 0, 50};
    input.enemies = {{101, 1, 1}, 1, {enemy}};
    input.enemy_event = {{101, 1, 1}, EnemyEventKind::Spawned, enemy};
    input.shot.world = {101, 1, 1}; input.shot.number = 1;
    input.shot.weapon = 1; input.shot.direction = {1, 0, 0};
    input.cheats = {0x125}; input.inventory = {{101, 1, 1}, 1, 2, {0, 99, 0, 0}};
    input.start.chapter = 1; input.start.level = 101; input.start.start = 1;
    input.start.cheats = 0x125; input.start.difficulty = 1;
    input.save_chunk = {0, {1, 2, 3}}; input.flow = {9, 101, 5, 2};
    input.damage = {{101, 1, 1}, 77, 3, enemy.id, 16388, 6, {1, 0, 0}};
    input.player_status = {{101, 1, 1}, 77, 4, 3, PlayerCondition::Hurt};
    input.ownership = {{101, 1, 1}, OwnershipPhase::Prepare, 1, EnemyOwner::Guest, enemy};
    input.life = {input.flow,77,4,5};
    input.camera.generation=9; input.camera.level=101; input.camera.cut=1;
    input.story={9,101,1,123,456,1,kStoryRecords,StoryPhase::Armed};
    input.object={9,101,1,123,ObjectKind::Breakable,1};
    input.shot_receipt={{101,1,1},1,ShotReject::Origin,false};
    input.digest.generation=9; input.digest.level=101; input.digest.sweep=1;
    input.digest.enemy_world={101,0,1};
    input.plan={0xC80C80C80ull,StartTransport::Direct,3,0,0,0,27179};
    const auto bytes = Packet(input);
    Check(bytes[0] == 'K' && bytes[1] == 'K' && bytes[2] == 'C' && bytes[3] == '1', "wire magic");
    Check(bytes[4] == 0 && bytes[5] == kProtocolVersion && bytes[6] == 0 && bytes[7] == i+1,
        "protocol 22 and message ID in wire header");
    Check(bytes[8] == 0x12 && bytes[11] == 0x78 && bytes[12] == 1 && bytes[19] == 0xEF,
        "header byte order independent of host endianness");
    Message output;
    Check(Decode(bytes, output), "decode every message type");
    Check(output.type == type && output.sequence == input.sequence && output.epoch == input.epoch,
        "fixed header round trip");
    Check(Packet(output) == bytes, "canonical full payload round trip");
    if (type == MessageType::Hello)
      Check(CheckCompatibility(input.hello, output.hello) == RejectReason::None &&
          output.display_name == input.display_name, "unified Hello compatibility and display name");
    if (type == MessageType::PlayerPose)
      Check(Near(output.pose.time, 12.75) && output.pose.level == 1402 && Near(output.pose.y, -9.5) &&
          output.pose.action == 65535 && output.pose.item == 255, "pose fields including diagnostic item");
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
void IntegratedMessageRouting() {
  Pair pair; pair.Pump(0,.2);
  Check(pair.host.State()==SessionState::Connected && pair.guest.State()==SessionState::Connected,
      "protocol 22 integration handshake");
  const EnemyState enemy{{123,0,EnemyFamily::Raptor},1,2,3,0,50};
  Message ownership; ownership.type=MessageType::OwnershipChange; ownership.epoch=pair.host.Epoch();
  ownership.ownership={{101,1,1},OwnershipPhase::Prepare,1,EnemyOwner::Guest,enemy};
  Message life; life.type=MessageType::PlayerDowned; life.epoch=pair.guest.Epoch();
  life.life={{1,101,1,0},77,1,1};
  Message camera; camera.type=MessageType::CameraPose;
  camera.camera.generation=1; camera.camera.level=101; camera.camera.cut=1; camera.camera.time=.3;
  OwnershipMailbox host_ownership,guest_ownership;
  CameraMailbox host_camera,guest_camera;
  Check(host_ownership.Submit(ownership) && pair.guest.QueueFlow(life),
      "ownership and downed event can be queued together");
  Check(!pair.host.QueueFlow(ownership) && !pair.host.QueueFlow(camera) &&
      !pair.guest.QueueOwnership(life) && !pair.guest.QueueCombat(life,.3),
      "new protocol messages cannot enter each other's queues");
  host_ownership.Exchange(pair.host);
  host_camera.Publish(camera.camera); host_camera.Exchange(pair.host,.3);
  pair.Pump(.3,.35);
  guest_ownership.Exchange(pair.guest); guest_camera.Exchange(pair.guest,.35);
  const auto received_ownership=guest_ownership.Take();
  const auto received_life=pair.host.TakeFlow();
  const auto received_camera=guest_camera.Sample(.35);
  Check(received_ownership.size()==1 && received_ownership.front().type==MessageType::OwnershipChange &&
      received_ownership.front().ownership.phase==OwnershipPhase::Prepare,
      "ownership reaches its mailbox while camera and life traffic are active");
  Check(received_life.size()==1 && received_life.front().type==MessageType::PlayerDowned &&
      received_life.front().life.incarnation==77, "downed event reaches flow only");
  Check(received_camera && received_camera->generation==1 && received_camera->level==101 &&
      pair.guest.TakeFlow().empty() && pair.host.TakeOwnership().empty() &&
      pair.host.TakeCombat().empty() && pair.guest.TakeCombat().empty(),
      "camera remains separate from flow, ownership and combat");
  life.type=MessageType::PlayerRevived; ++life.life.incarnation; ++life.life.number;
  Check(pair.guest.QueueFlow(life),"revived event queues after downed event");
  pair.Pump(.36,.4);
  const auto revived=pair.host.TakeFlow();
  Check(revived.size()==1 && revived.front().type==MessageType::PlayerRevived &&
      revived.front().life.incarnation==78,"revival retains its distinct message ID");
  pair.guest.End(.41); guest_ownership.Exchange(pair.guest); guest_camera.Exchange(pair.guest,.41);
  Check(guest_ownership.Take().empty() && !guest_camera.Sample(.41),
      "session end clears both merged mailboxes");
}
void CheatSets() {
  for (uint16_t bits = 0; bits <= 0x1ff; ++bits) {
    Message m; m.type = MessageType::Cheats; m.cheats = {bits};
    const auto bytes = Packet(m); Message decoded;
    Check(bytes.size() == kHeaderBytes+2 && Decode(bytes, decoded) && decoded.cheats == m.cheats,
        "every gameplay cheat set round trips including master off");
    for (size_t n = 0; n < bytes.size(); ++n) {
      Message out; out.sequence = 999;
      Check(!Decode(std::span(bytes.data(), n), out) && out.sequence == 999, "cheat truncation preserves output");
    }
  }
  Message m; m.type = MessageType::Cheats; m.cheats = {0x200};
  std::vector<uint8_t> bytes; Check(!Encode(m, bytes), "unknown cheat bit refused by encoder");
  m.cheats = {}; bytes = Packet(m); bytes[kHeaderBytes] = 2;
  Message out; out.sequence = 999;
  Check(!Decode(bytes, out) && out.sequence == 999, "unknown wire cheat bit refused without mutation");
  m.cheats = {0x121}; bytes = Packet(m); bytes.push_back(0);
  Check(!Decode(bytes, out), "cheat trailing byte refused");

  const CheatSet own{0x102}, host{0x125};
  CheatOverride overlay;
  Check(SessionCheats(own, overlay.Snapshot()) == own, "solo uses own choices");
  Check(overlay.Apply(host) && SessionCheats(own, overlay.Snapshot()) == host, "host overlay applies all choices");
  Check(own.bits == 0x102, "applying overlay never mutates own saved choices");
  Check(overlay.Apply(CheatSet{}) && !SessionCheats(own, overlay.Snapshot()).Enabled(), "host master off defeats guest master on");
  Check(!overlay.Apply(CheatSet{0x200}) && overlay.Snapshot() == CheatSet{}, "invalid overlay cannot replace active host choices");
  overlay.Apply({});
  Check(!overlay.Snapshot() && SessionCheats(own, overlay.Snapshot()) == own, "disconnect restores own choices without a settings write");
  Check(kk::cheat_rules::Suppressed("machine_gun", 303) && !kk::cheat_rules::Suppressed("healing", 303),
      "host weapon setting still subject to unchanged chapter rules");

  Pair p;
  const CheatSet first{0x125}, changed{0x109};
  for (int i = 0; i < 40; ++i) { p.host.Tick(i*.01, {}, nullptr, &first); p.guest.Tick(i*.01); }
  Check(p.guest.HostCheats() == first && !p.host.HostCheats(), "only guest receives initial host settings after handshake");
  p.a.block_cheats = true;
  p.host.Tick(.4, {}, nullptr, &changed); p.guest.Tick(.4);
  Check(p.guest.HostCheats() == first, "backpressure retains previous host cheat set");
  p.a.block_cheats = false;
  p.host.Tick(.41, {}, nullptr, &changed); p.guest.Tick(.41);
  Check(p.guest.HostCheats() == changed, "pause changes retry reliably");
  m.cheats = first; m.epoch = 123456789; m.sequence = 1;
  p.b.incoming.push_back(Packet(m)); p.guest.Tick(.42);
  Check(p.guest.HostCheats() == changed, "older cheat packet cannot roll settings back");
  m.sequence = 999; m.epoch = 42; p.b.incoming.push_back(Packet(m)); p.guest.Tick(.43);
  Check(p.guest.HostCheats() == changed, "wrong epoch cheat packet refused");
  m.epoch = 123456789; p.a.incoming.push_back(Packet(m)); p.host.Tick(.44);
  Check(!p.host.HostCheats(), "guest cannot set host cheats");
  p.guest.Tick(3.5);
  Check(p.guest.State() == SessionState::Lost && p.guest.HostCheats() == changed, "temporary loss keeps host override");
  p.guest.End(3.51);
  Check(!p.guest.HostCheats(), "session end clears override");
}
void Handshake() {
  for (bool guest_first : {false, true}) {
    Pair pair;
    pair.Pump(0, .4, guest_first);
    Check(pair.host.State() == SessionState::Connected && pair.guest.State() == SessionState::Connected,
        "compatible handshake, either scheduler order");
    Check(pair.host.PartnerName() == "Guest" && pair.guest.PartnerName() == "Host", "Hello names cross roles");
    pair.Pump(.41, 2);
    Check(pair.host.Stats().ping_ms > 0 && pair.guest.Stats().ping_ms > 0, "ping and pong update RTT");
    pair.host.End(2.01); pair.guest.Tick(2.02);
    Check(pair.guest.State() == SessionState::Ended, "clean Bye ends peer");
    Check(pair.guest.PartnerName().empty(), "ended session forgets persona");
  }
  for (RejectReason reason : {RejectReason::Protocol, RejectReason::Version, RejectReason::Build,
      RejectReason::GameplayMods}) {
    auto c = Compatible();
    if (reason == RejectReason::Protocol) --c.protocol; // Previous protocol peers must fail the handshake.
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
  PoseVelocity velocity;
  PlayerPose sampled;
  sampled.level=201; velocity.Apply(sampled);
  sampled.time=.001; sampled.x=.03f; velocity.Apply(sampled);
  Check(!sampled.vx,"sub-physics callbacks cannot amplify displacement");
  sampled.time=.05; sampled.x=.2f; velocity.Apply(sampled);
  Check(Near(sampled.vx,4),"velocity averages across the physics sampling window");
  sampled.time=.051; velocity.Apply(sampled);
  Check(Near(sampled.vx,4),"velocity holds between samples instead of flickering to idle");
  sampled.time=.101; velocity.Apply(sampled);
  Check(!sampled.vx,"stationary position settles to zero velocity");
  sampled.time=.16; sampled.x=20; velocity.Apply(sampled);
  Check(!sampled.vx,"teleport never drives a running gait");
  sampled.time=.22; sampled.x=20.2f; ++sampled.level; velocity.Apply(sampled);
  Check(!sampled.vx,"new level clears the velocity anchor");
  Message extended; extended.type=MessageType::PlayerPose;
  extended.pose.vx=3; extended.pose.vy=-2; extended.pose.pitch=.6f;
  extended.pose.weapon=4; extended.pose.crouched=1; extended.pose.airborne=1; extended.pose.aiming=1;
  extended.pose.fire=17; extended.pose.fire_weapon=2;
  Message decoded;
  Check(Decode(Packet(extended),decoded) && decoded.pose.fire==17 && decoded.pose.fire_weapon==2 &&
      decoded.pose.weapon==4 && decoded.pose.crouched==1 && decoded.pose.airborne==1 && decoded.pose.aiming==1 && Near(decoded.pose.pitch,.6) && Near(decoded.pose.vy,-2),
      "animation pose fields survive the wire");
  std::vector<uint8_t> invalid;
  extended.pose.airborne=2;
  Check(!Encode(extended,invalid),"invalid airborne state rejected");
  extended.pose.airborne=1;
  for (float value : {std::numeric_limits<float>::quiet_NaN(), 1000.f}) {
    extended.pose.vx=value; Check(!Encode(extended,invalid), "invalid animation velocity rejected");
  }
  extended.pose.vx=0; extended.pose.pitch=2;
  Check(!Encode(extended,invalid), "invalid view pitch rejected");
  extended.pose.pitch=0; extended.pose.weapon=11;
  Check(!Encode(extended,invalid), "unknown weapon rejected");
  for (uint8_t equipment=0;equipment<=10;++equipment) {
    extended.pose.weapon=equipment;
    Check(Decode(Packet(extended),decoded) && decoded.pose.weapon==equipment,
        "every native held equipment kind survives the wire");
  }
  extended.pose.aiming=2;
  Check(!Encode(extended,invalid),"unknown aim stance rejected");
  PoseBuffer buffer;
  Check(!buffer.Sample(0), "empty interpolation buffer");
  PlayerPose a{1000, 101, 0, 2, 3, 3.10f, 4, 10}, b = a;
  b.time = 1000.2; b.x = 20; b.facing = -3.10f; b.action = 20; b.item = 7;
  b.vx=4; b.pitch=1; b.fire=1; b.fire_weapon=2;
  buffer.Push(a, 1); buffer.Push(b, 1.2);
  auto middle = buffer.Sample(1.2);
  Check(middle && Near(middle->x, 10) && std::abs(middle->facing) > 3,
      "100 ms interpolation, independent remote clock and shortest facing arc");
  Check(middle && Near(middle->vx,2) && Near(middle->pitch,.5) && middle->fire==0 && buffer.Sample(1.3)->fire==1,
      "animation velocity and pitch interpolate; shot waits for its pose time");
  Check(buffer.Sample(1.19)->action == a.action && buffer.Sample(1.19)->item == a.item &&
      buffer.Sample(1.21)->action == b.action && buffer.Sample(1.21)->item == b.item,
      "interpolation changes diagnostic action/item together, without blending IDs");
  buffer.Push(a, 1.25);
  Check(Near(buffer.Sample(1.2)->x, 10), "out of order sample ignored");
  Check(buffer.Sample(1.5)->x == 20 && !buffer.Sample(1.71), "holds without extrapolation then expires");
  b.time += 1; b.level = 202; b.x = 99; buffer.Push(b, 2);
  Check(buffer.Sample(2)->level == 202 && buffer.Sample(2)->x == 99, "level transition clears old samples");
  buffer.Clear(); Check(!buffer.Sample(2), "explicit clear");

  // Also exercise a receiver clock far ahead of the sender and exact local expiry.
  a.time = 2; b = a; b.time = 2.2; b.x = 20;
  buffer.Push(a, 9000); buffer.Push(b, 9000.2);
  Check(Near(buffer.Sample(9000.2)->x, 10), "receiver clock ahead does not expire a remote-clock pose");
  Check(buffer.Sample(9000.7).has_value() && !buffer.Sample(9000.701), "expiry uses last local arrival, inclusive 500 ms");

  Pair pair; pair.Pump(0, .4);
  PlayerPose local{.5, 101, 5, 6, 7, .4f, 8, 9};
  pair.host.Tick(.5, local); pair.guest.Tick(.51);
  Check(pair.guest.Partner(.51) && Near(pair.guest.Partner(.51)->z, 7), "session pose reaches partner");
  const int count = pair.a.unreliable_poses;
  pair.host.Tick(.51, local);
  Check(pair.a.unreliable_poses == count, "pose rate capped at twenty Hz");
  Check(pair.host.Poses().sent == 1 && pair.guest.Poses().received == 1 &&
      pair.guest.Poses().remote_level == 101, "pose counters and last remote level follow accepted packets");
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
  Check(pair.guest.Poses().received == 2 && pair.guest.Poses().remote_level == 101,
      "malformed, old epoch and reordered packets do not count as received poses");
}
void PoseReadingAndDisplay() {
  constexpr uint32_t g = 0x40000100, actor = 0x40001000, ext = 0x40002000,
      ai = 0x40003000, block = 0x40004000, matrix = 0x40005000, vars = 0x40006000,
      rig = 0x4000A000, animation = 0x4000B000;
  std::map<uint32_t, uint32_t> words{
      {0x82CC97C0, g}, {g + 0x18, 201}, {0x82CCBF7C, actor}, {actor + 24, ext},
      {ext + 4, ai}, {ai + 24, block}, {block + 20, 0x826718B8},
      {actor + 12, matrix}, {ai + 64, vars}, {0x828E3B48, 1},
      {actor + 20, rig}, {rig, animation}, {animation + 156, 123}, {animation + 160, 0xAB123456}};
  std::map<uint32_t, float> floats{{matrix + 20, -1}, {matrix + 48, 3},
      {matrix + 52, 4}, {matrix + 56, 5}};
  auto read = [&] {
    return ReadJackPose([&](uint32_t a) { return words[a]; },
        [&](uint32_t a) { return uint16_t(words[a]); }, [&](uint32_t a) { return floats[a]; }, 50);
  };
  auto check = [&](const char* failure) {
    const auto result = read();
    Check(!result.pose && std::string_view(result.failure) == failure, failure);
  };
  auto pose = read();
  Check(pose.pose && pose.pose->level == 201 && pose.pose->x == 3 &&
      std::string_view(pose.failure) == "ok", "retained load-page mode does not suppress a loaded Jack");
  Check(pose.pose->action == 123 && pose.pose->item == 0xAB, "read diagnostic action and big-endian item byte");
  constexpr uint32_t child=0x4000C000;
  constexpr uint32_t weapons=0x40010000;
  words[0x82CC8848]=weapons; words[vars+4]=1; words[weapons+0x3348]=2;
  words[g+13128]=4;
  Check(read().pose->weapon==2,"native H inventory wins over the universe request block");
  words[vars+0xBAC]=1;
  Check(read().pose->aiming==1,"real native aim flag is published");
  words[vars+0xBAC]=0;
  words[vars+0xB94]=1;
  Check(read().pose->aiming==0,"zoom alone does not publish a raised weapon");
  words[vars+0xB94]=0;
  words[weapons+0x3348]=6;
  Check(read().pose->weapon==6,"spear equipment is not published as empty hands");
  words[weapons+0x3348]=2;
  words[animation+0x60]=child; words[animation+0x7C]=0x74;
  Check(read().pose->action==0x74 && read().pose->item==0,"active machine-gun reload child is visible to the peer");
  words[child]=0x80;
  Check(read().pose->action==123,"finished reload never hides the main animation");
  words[child]=0; words[animation+0x7C]=0x73;
  Check(read().pose->action==123,"another weapon's child action is not a reload");
  words[animation+0x60]=0; words[weapons+0x3348]=0;
  Check(!read().pose->airborne,"cleared native flight height stays grounded");
  floats[vars+13268]=1;
  Check(!read().pose->airborne,"native one-unit flight threshold excludes small steps");
  floats[vars+13268]=1.2f;
  Check(read().pose->airborne,"native fall height publishes airborne state");
  floats[vars+13268]=-1.2f;
  Check(read().pose->airborne,"rising above the last floor also uses the airborne clip");
  floats[vars+13268]=std::numeric_limits<float>::quiet_NaN();
  Check(!read().pose->airborne,"invalid flight height cannot invent a jump");
  floats[vars+13268]=0;
  words[0x82CC97C0] = 0x9fffe000;
  check("globals-span"); // Weapon sampling also needs the far end of G to be readable.
  words[0x82CC97C0] = g;
  words[g + 4] = 1; words[actor] = 1;
  check("level-request");  // First failure, before touching the actor.
  words[g + 4] = 0; words[actor] = 0;
  words[0x82D64080] = 123;
  for (uint32_t state : {1u, 2u}) { words[0x82D64080 + 12] = state; check("loading-screen"); }
  words[0x82D64080] = 0;
  words[actor] = 1; check("actor-flags"); words[actor] = 0;
  words[ai + 80] = 1; check("track-range"); words[ai + 80] = 0;
  words[block + 20] = 0; check("jack-track"); words[block + 20] = 0x826718B8;
  words[actor + 12] = 0x9FFFFFC4; check("matrix-span"); words[actor + 12] = matrix;
  words[ai + 64] = 0x9FFFFFFC; check("vars-span"); words[ai + 64] = vars;
  floats[matrix + 20] = 0; check("facing"); floats[matrix + 20] = -1;
  floats[matrix + 48] = std::numeric_limits<float>::quiet_NaN(); check("pose-values");

  PlayerPose local, partner;
  Check(PartnerHere(PartnerDistance(local, partner)), "identical start positions show Partner: here");
  partner.x = .6f; partner.y = .8f;
  Check(PartnerHere(PartnerDistance(local, partner)), "one unit is here");
  partner.z = .1f;
  Check(!PartnerHere(PartnerDistance(local, partner)), "here uses full three-dimensional distance");
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
void PuppetPolicies() {
  using namespace kk::coop::puppet;
  std::map<uint32_t,uint32_t> weapon_memory{{0x82CC8848,0x40000000},{0x82CC97C0,0x50000000},
      {0x40000000+4*(3+3281),2},{0x50000000+4*(3+3281),1}};
  const auto weapon_word=[&](uint32_t p) { return weapon_memory.at(p); };
  const auto weapon_address=[](uint32_t p,uint32_t n) { return p==0x40000000 && n==13176; };
  Check(HumanWeapon(3,weapon_word,weapon_address)==2,
      "body weapon follows the native character bank, not universe request globals");
  Check(!HumanWeapon(0,weapon_word,weapon_address) && !HumanWeapon(13,weapon_word,weapon_address),
      "invalid character slots never index a weapon bank");
  weapon_memory[0x82CC8848]=0;
  Check(!HumanWeapon(3,weapon_word,weapon_address),"absent native bank never reaches a table read");
  weapon_memory[0x82CC8848]=0x40000000;
  weapon_memory[0x40000000+4*(3+3281)]=999;
  Check(!HumanWeapon(3,weapon_word,weapon_address),"unknown native gun cannot choose an animation");
  std::map<uint32_t, uint32_t> animation_memory{{100 + 132, 300}, {300 + 4, 400}, {400, 500}};
  const auto pointer = [&](uint32_t p, uint32_t off, uint32_t) {
    // Invalid or absent pointers must never cause the next memory read.
    if (!p) return uint32_t{0};
    const auto it = animation_memory.find(p + off);
    return it == animation_memory.end() ? uint32_t{0} : it->second;
  };
  Check(AnimationEventHeader(100, pointer) == 500, "complete animation event metadata remains usable");
  Check(AnimationLayerHeader(300, pointer) == 500, "root motion resolves a complete layer header");
  animation_memory[600+4]=700; animation_memory[700]=800;
  animation_memory[600+24]=900; animation_memory[900]=1000;
  const auto two_items=[](uint32_t) { return 2u; };
  Check(AnimationItemHeader(600,0,two_items,pointer)==800 &&
      AnimationItemHeader(600,1,two_items,pointer)==1000,"loaded action variants resolve independently");
  Check(!AnimationItemHeader(600,2,two_items,pointer) &&
      !AnimationItemHeader(600,UINT32_MAX,two_items,pointer),"native item count bounds animation requests");
  const auto descriptor=AnimationItemDescriptor(600,1,two_items,pointer);
  animation_memory[1104]=descriptor;
  Check(descriptor==624 && AnimationLayerHeader(1100,pointer)==900,
      "scratch root layer points to the item pointer field, not the resource or track array");
  for (const auto address : {232u, 304u, 400u}) {
    const auto saved = animation_memory[address]; animation_memory[address] = 0;
    Check(!AnimationEventHeader(100, pointer), "missing animation metadata returns no event instead of reading null");
    if (address != 232u) Check(!AnimationLayerHeader(300, pointer), "root motion rejects a missing descriptor or header");
    animation_memory[address] = saved;
  }
  const MutableRig original{1, 2, 3, 4, 5, 6, 7}, copy{8, 9, 10, 11, 12, 13, 14};
  Check(Independent(copy, original), "new body owns all mutable components");
  for (std::size_t i = 0; i < copy.size(); ++i) {
    auto alias = copy; alias[i] = original[i];
    Check(!Independent(alias, original), "reject each shared mutable component");
    alias = copy; alias[i] = 0;
    Check(!Independent(alias, original), "reject each absent mutable component");
  }
  auto alias = copy; alias[1] = original[2];
  Check(!Independent(alias, original), "reject cross-component alias to original vars");
  alias = copy; alias[2] = alias[1];
  Check(!Independent(alias, original), "reject internally aliased copy");
  std::array<uint32_t, 5> actors{3, 8, 4, 9, 5};
  const uint32_t hidden[]{8, 9};
  Check(FilterActors(actors, hidden) == 3 && actors[0] == 3 && actors[1] == 4 && actors[2] == 5,
      "volume query removes only spawned bodies and preserves original actor order");
  Check(FilterActors(std::span(actors.data(), 3), {}) == 3, "empty quarantine leaves query alone");
  std::array<uint32_t, 3> all_hidden{8, 8, 9};
  Check(FilterActors(all_hidden, hidden) == 0, "volume query can exclude every spawned result");
  const Action observed[] = {{41, 3, 0}, {22, 3, 1}, {35, 4, 4}};
  AnimationDrive drive;
  {
    FireCapture fire(true);
    FireCapture::Ray(0x11111111);
    Check(!fire.Fired(true,false),"an unrelated ray cannot manufacture a partner shot");
    {
      FireCapture npc(false); FireCapture::Ray(0x8269727C);
      Check(!npc.Fired(true,true),"NPC ammo consumption never becomes the player's visual shot");
    }
    Check(!fire.Fired(true,false),"nested NPC fire does not mark the surrounding player callback");
    FireCapture::Ray(0x8269727C);
    Check(fire.Fired(true,false),"confirmed native ray animates even without enemy forwarding or ammo delta");
    Check(!fire.Fired(false,true),"world changes discard old native firing evidence");
  }
  { FireCapture fire(true); Check(fire.Fired(true,true),"spent native ammo also confirms pellet weapons"); }
  PlayerPose motion; motion.vx=4; motion.fire=10; motion.fire_weapon=2;
  drive.Update(motion,.05f);
  Check(drive.moving && !drive.new_shot, "late binding never replays old shots");
  motion.fire=12; drive.Update(motion,.05f);
  Check(drive.new_shot && drive.fire_left > 0, "lost pose recovers cumulative shots once");
  drive.Update(motion,.05f);
  Check(!drive.new_shot, "same shot never restarts again");
  motion.fire=11; drive.Update(motion,.05f);
  motion.fire=12; drive.Update(motion,.05f);
  Check(!drive.new_shot, "arrival jitter cannot replay a confirmed shot");
  motion.vx=0;
  for (unsigned i=0;i<20;++i) drive.Update(motion,.05f);
  Check(!drive.moving && !drive.fire_left, "stop settles to idle and recoil expires");
  AnimationDrive jitter;
  motion.vx=2.2f;
  for (unsigned i=0;i<60;++i) jitter.Update(motion,1.f/60);
  for (unsigned i=0;i<120;++i) {
    motion.vx=i%12<8 ? 2.2f : 0;
    jitter.Update(motion,1.f/60);
    Check(jitter.moving,"short network/physics gaps never flap walking to idle");
  }
  motion.vx=0;
  for (unsigned i=0;i<45;++i) jitter.Update(motion,1.f/60);
  Check(!jitter.moving,"sustained stop enters idle within 750 ms");
  motion.action=0x56; jitter.Update(motion,.05f);
  Check(jitter.use_left>0 && jitter.use_action==0x56,"peer equipment throw starts its own loaded action");
  for (unsigned i=0;i<20;++i) jitter.Update(motion,.05f);
  Check(!jitter.use_left,"repeated equipment pose packets do not restart the gesture");
  motion.time=2; motion.action=0; jitter.Update(motion,.05f);
  motion.time=1; motion.action=0x56; jitter.Update(motion,.05f);
  Check(!jitter.use_left,"rewound network timeline cannot replay an equipment edge");
  Check(AnimationDrive::EquipmentAction(0x74) && !AnimationDrive::FullBodyEquipmentAction(0x74),
      "native arm-only reload uses a gesture without replacing the leg pose");
  motion.time=3;
  motion.fire++; jitter.Update(motion,.05f); jitter.Update(motion,.05f);
  Check(jitter.GesturePitch()>.05f,"confirmed fire supplies bounded recoil even without a gun clip");
  motion.action=0;
  motion.airborne=1; drive.Update(motion,.05f);
  motion.airborne=0; drive.Update(motion,.05f);
  Check(drive.land_left>0,"native airborne to grounded edge starts a landing clip");
  AnimationDrive late_air; late_air.Update(motion,.05f);
  Check(!late_air.land_left,"joining an already grounded partner never invents a landing");
  Check(HumanAction(1,false,false,false)!=HumanAction(1,true,false,false) &&
      HumanAction(3,true,false,true)!=HumanAction(1,true,false,true), "native motion and weapon firing differ");
  Check(HumanAction(2,true,true,false)!=HumanAction(2,true,true,true),"crouched motion and fire use separate native actions");
  Check(HumanAction(2,false,false,true)==146 && HumanAction(1,false,false,true)==145,
      "native armed stationary clips differ from walking clips");
  Check(AnimationWeapon(2,0)==0 && AnimationWeapon(4,0)==0 &&
      AnimationWeapon(4,2)==4 && AnimationWeapon(6,0)==6 && AnimationWeapon(0,2)==0,
      "unarmed donor never plays unrelated gun IDs; native armed and held item banks remain usable");
  Check(HumanAction(6,false,false,false)==96 && HumanAction(6,false,false,false,true)==75 &&
      HumanAction(7,true,false,false,true)==11,"native spear and bone aim preserves the moving gait");
  AnimationDrive aimed; PlayerPose aim_pose; aim_pose.time=1; aim_pose.weapon=2; aim_pose.aiming=1;
  aimed.Update(aim_pose,.05f);
  const auto first_aim=aimed.aim_weight;
  Check(first_aim>0 && first_aim<1,"armed aim enters smoothly without restarting its native clip");
  aim_pose.time=.5; aim_pose.weapon=0; aim_pose.aiming=0; aimed.Update(aim_pose,.05f);
  Check(aimed.weapon==2 && aimed.aim_weight>first_aim,"rewound pose does not lower the armed aim");
  const auto before_reload=aimed.aim_weight;
  aim_pose.time=2; aim_pose.weapon=2; aim_pose.aiming=1; aim_pose.action=0x74; aimed.Update(aim_pose,.05f);
  Check(aimed.aim_weight<before_reload,"native reload takes precedence over arm aiming");
  Check(WeaponVariant(2,true)==2 && WeaponVariant(4,true)==5 && WeaponVariant(0,true)==0 &&
      WeaponVariant(6,true)==10 && WeaponVariant(7,true)==7 && WeaponVariant(6,false)==3,
      "native hand variants distinguish guns, spear, bone and empty hands");
  const auto carl=[](uint16_t action)->std::optional<uint8_t> {
    return action==11 ? std::optional<uint8_t>{2} : std::nullopt;
  };
  const auto fallback=FireAction(2,false,carl);
  Check(!FireAction(0,false,carl),"unarmed body never uses an unrelated loaded gesture as gunfire");
  Check(fallback && fallback->action==11 && fallback->item==2,
      "missing machine-gun bank uses loaded revolver aim and its own variant");
  Check(FireAction(2,true,carl)->action==11,"crouched missing weapon still has an aim visual");
  Check(!FireAction(2,false,[](uint16_t a)->std::optional<uint8_t> {
    return a==14 || a==164 || a==162 || a==28 || a==38 ? std::optional<uint8_t>{0} : std::nullopt;
  }),"hit reactions must never masquerade as gunfire");
  Check(!FireAction(2,false,[](uint16_t)->std::optional<uint8_t> { return {}; }),
      "empty action bank cannot manufacture an unloaded clip");
  const Gait directional[] = {{{10,0,3},0,false},{{42,1,2},1.570796327f,false},
      {{43,2,1.5f},3.141592654f,false},{{27,0,1},0,true}};
  auto gait=ChooseGait(0,2,0,false,directional);
  Check(gait && gait->clip.action==42 && gait->clip.item==1 && gait->directional && Near(gait->facing,0),
      "loaded strafe keeps view yaw and native speed");
  gait=ChooseGait(-1.5f,0,0,false,directional);
  Check(gait && gait->clip.action==43 && gait->directional && Near(gait->facing,0),
      "loaded backward gait keeps the body facing the view");
  gait=ChooseGait(0,2,0,false,std::span(directional,1));
  Check(gait && !gait->directional && Near(gait->facing,1.570796327f),
      "missing strafe turns a forward-only body into its travel direction");
  Check(ChooseGait(1,0,0,true,directional)->clip.action==27,"crouch uses its own calibrated gait");
  Check(!ChooseGait(0,0,0,false,directional),"idle never chooses a directional gait");
  const Gait similar[] = {{{10,0,2},0,false},{{15,0,2.5f},0,false}};
  uint32_t current=10<<8;
  for (unsigned i=0;i<200;++i) {
    const auto stable=ChooseGait(i%2 ? 2.20f : 2.28f,.01f,0,false,similar,current);
    Check(stable && stable->clip.action==10,"speed jitter at gait boundary preserves clip phase");
    current=(uint32_t(stable->clip.action)<<8)|stable->clip.item;
  }
  Check(ChooseGait(3,0,0,false,similar,current)->clip.action==15,"sustained faster travel can change gait");
  Check(ChooseAction(.09f, observed)->action == 41, "observed idle below threshold");
  Check(ChooseAction(.1f, observed)->action == 22, "threshold chooses observed moving tuple");
  Check(ChooseAction(1.2f, observed)->action == 22, "walk uses closest observed speed");
  Check(ChooseAction(-4.5f, observed)->action == 35 && ChooseAction(4.5f, observed)->item == 4,
      "run uses observed tuple, including its own item");
  Check(!ChooseAction(3, std::span(observed, 1)), "no observed moving action requests keeping current");
  Check(!ChooseAction(0, std::span(observed + 1, 2)), "no idle observation never invents action zero");
  Check(!ChooseAction(1, {}) && !ChooseAction(std::numeric_limits<float>::quiet_NaN(), observed),
      "empty and invalid speed never request animation");
  const Point origin{};
  Check(!ClampStep(origin, {3, 4, 0})->teleport, "five unit boundary is normal motion");
  const auto jump = ClampStep(origin, {3, 4, .1f});
  Check(jump && jump->teleport && jump->target.z == .1f && jump->target.x == 3,
      "over five units permits exact teleport and preserves partner height");
  Check(!ClampStep(origin, {std::numeric_limits<float>::infinity(), 0, 0}), "invalid teleport rejected");
  Check(!ClampStep({1000000, 0, 0}, origin), "invalid prior position rejected");
  Conditions ready{true, SessionState::Connected, true, true, true, true, false, false, true};
  Check(!ReleaseReason(ready), "connected matching level may drive");
  auto expect = [&](Conditions c, const char* reason) {
    Check(ReleaseReason(c) && std::string_view(ReleaseReason(c)) == reason, "puppet release reason");
  };
  auto c = ready; c.enabled = false; expect(c, "coop-disabled");
  c = ready; c.state = SessionState::Lost; expect(c, "lost");
  c = ready; c.state = SessionState::Ended; expect(c, "ended");
  c = ready; c.state = SessionState::Connecting; expect(c, "not-connected");
  c = ready; c.fresh = false; expect(c, "snapshot-expired");
  c = ready; c.local = false; expect(c, "local-pose-unavailable");
  c = ready; c.partner = false; expect(c, "partner-pose-unavailable");
  c = ready; c.same_level = false; expect(c, "different-level");
  c = ready; c.photo = true; expect(c, "photo-mode");
  c = ready; c.loading = true; expect(c, "load");
  c = ready; c.environment = false; expect(c, "world-level-or-hero-changed");
}
void PartnerSelection() {
  using namespace kk::coop::puppet;
  AutoCandidate spare;
  spare.actor = 100; spare.natural_h = spare.stable = spare.rig_valid = true;
  spare.script_evidence_complete = true; spare.distance = 80;
  Check(!AutoReject(spare), "distant stable human with script evidence may be borrowed");
  auto ally = spare; ally.actor = 101; ally.kind = HumanKind::Ally; ally.distance = 400;
  auto background = spare; background.actor = 102; background.kind = HumanKind::Background; background.distance = 0;
  const AutoCandidate ranked[]{ally, spare, background};
  Check(SelectAuto(ranked) == 2, "positive background evidence beats distant ally and unknown");
  Check(SelectAuto(std::span(ranked, 2)) == 1, "unknown human preferred over identified ally");
  auto farther = spare; farther.actor = 103; farther.distance = 90;
  const AutoCandidate distances[]{spare, farther};
  Check(SelectAuto(distances) == 1, "distance is a preference, never a maximum teleport range");
  auto tie = spare; tie.actor = 99;
  const AutoCandidate ties[]{spare, tie};
  Check(SelectAuto(ties) == 1, "equal scores use stable actor tie break");
  auto reject = [&](AutoCandidate c, const char* reason) {
    const AutoCandidate one[]{c};
    Check(AutoReject(c) && std::string_view(AutoReject(c)) == reason && !SelectAuto(one), reason);
  };
  auto c = spare; c.hero = true; reject(c, "hero-or-alias");
  c = spare; c.ann = true; reject(c, "Ann");
  c = spare; c.natural_h = false; reject(c, "not-natural-H");
  c = spare; c.actor = 0; reject(c, "not-natural-H");
  c = spare; c.stable = false; reject(c, "not-stable-one-second");
  c = spare; c.rig_valid = false; reject(c, "invalid-rig-or-membership");
  c = spare; c.dead = true; reject(c, "dead");
  c = spare; c.grabbed = true; reject(c, "grabbed-or-carried");
  c = spare; c.cinematic = true; reject(c, "cinematic");
  c = spare; c.following = true; reject(c, "following-Jack");
  c = spare; c.script_reference = true; reject(c, "mission-or-trigger-reference");
  c = spare; c.script_evidence_complete = false; reject(c, "script-evidence-unavailable");
  c = spare; c.distance = std::numeric_limits<float>::quiet_NaN(); reject(c, "invalid-distance");
  c = spare; c.distance = -1; reject(c, "invalid-distance");
  Check(!SelectAuto({}), "no candidates leaves marker only");
  std::array<AutoCandidate, 2> live{background, spare};
  Check(SelectAuto(live) == 0, "initial selection");
  live[0].script_reference = true;
  Check(SelectAuto(live) == 1, "new script wait releases first choice and permits another");
  live[1].dead = true;
  Check(!SelectAuto(live), "no replacement when remaining candidate dies");
  live = {background, spare};
  Check(SelectAuto(live) == 0, "new load observations permit fresh selection");

  auto resident = spare; resident.actor = 200; resident.registry_unclaimed = true;
  resident.kind = HumanKind::Ally; resident.distance = 0;
  const AutoCandidate tiers[]{background, resident, farther};
  Check(SelectAuto(tiers) == 1 && AutoTier(resident) == 0,
      "fully audited unclaimed resident precedes every scheduled kind and distance");
  c = resident; c.registry_unclaimed = false;
  Check(AutoTier(c) == 3 && AutoTier(background) == 1 && AutoTier(spare) == 2,
      "incomplete resident audit falls back to the existing scheduled kind tiers");
  auto dormant = resident; dormant.scheduled = false;
  reject(dormant, "unscheduled-movement-and-draw-unconfirmed");
  const AutoCandidate dormant_fallback[]{dormant, background};
  Check(SelectAuto(dormant_fallback) == 1, "unconfirmed dormant human cannot displace a scheduled body");
  dormant.registry_unclaimed = false;
  dormant.unscheduled_setters_and_draw_confirmed = true;
  reject(dormant, "unscheduled-not-unclaimed");
  dormant.registry_unclaimed = true;
  Check(AutoTier(dormant) == 0, "dormant policy requires both a full unclaimed audit and native evidence");
  dormant.script_reference = true;
  reject(dormant, "mission-or-trigger-reference");
  c = resident; c.cinematic = true; reject(c, "cinematic");
  c = resident; c.following = true; reject(c, "following-Jack");
  c = resident; c.dead = true; reject(c, "dead");
  c = resident; c.natural_h = false; reject(c, "not-natural-H");
  c = resident; c.stable = false; reject(c, "not-stable-one-second");
  c = resident; c.hero = true; reject(c, "hero-or-alias");
  c = resident; c.rig_valid = false; reject(c, "invalid-rig-or-membership");
  auto resident_tie = resident; resident_tie.actor = 199;
  const AutoCandidate resident_ties[]{resident, resident_tie};
  Check(SelectAuto(resident_ties) == 1, "resident tier ties keep deterministic actor ordering");
  resident_tie.script_reference = true;
  const AutoCandidate reclaimed[]{resident_tie, background};
  Check(SelectAuto(reclaimed) == 1, "new resident claim revokes tier zero before fallback selection");

  auto story = spare; story.script_reference = true; story.mission_distance = 10;
  Check(BodyTier(story) == 4, "referenced H is a tier A fallback");
  Check(PreferStoryCopy(story, false), "copy a referenced story human before moving its authored actor");
  Check(!PreferStoryCopy(story, true), "failed native copy preserves the last-resort story lease");
  Check(!PreferStoryCopy(spare, false), "safe spare humans keep normal borrowing priority");
  const AutoCandidate safe_first[]{story, spare};
  Check(SelectBody(safe_first) == 1, "safe tiers always precede story borrowing");
  auto animated=story; animated.actor=105; animated.animation_support=3; animated.distance=0;
  const AutoCandidate banks[]{story,animated};
  Check(SelectBody(banks)==1,"same safety prefers loaded movement and firing over distance");
  const AutoCandidate still_safe[]{animated,spare};
  Check(SelectBody(still_safe)==1,"animation bank never outranks a safer tier");
  animated.cinematic=true;
  const AutoCandidate still_quiet[]{animated,story};
  Check(SelectBody(still_quiet)==1,"animation bank never outranks an inactive story state");
  animated.cinematic=false; animated.mission_distance=9;
  const AutoCandidate still_later[]{animated,story};
  Check(SelectBody(still_later)==1,"animation bank never outranks distance from the current mission record");
  auto distant_record = story; distant_record.actor = 104; distant_record.mission_distance = 30;
  const AutoCandidate mission_rank[]{story, distant_record};
  Check(SelectBody(mission_rank) == 1, "tier A prefers references farther from the current record");
  distant_record.cinematic = true;
  const AutoCandidate state_rank[]{story, distant_record};
  Check(SelectBody(state_rank) == 0, "avoid a cinematic even if its mission reference is farther away");
  auto native = spare; native.natural_h = false; native.other_human = true;
  Check(!PreferStoryCopy(native, false), "PNJInd fallback is not sent through the H-only copy path");
  const AutoCandidate human_fallback[]{native, story};
  Check(BodyTier(native) == 5 && SelectBody(human_fallback) == 1, "PNJInd is tier B after story H");
  auto leased = story; leased.current_script_use = true;
  const AutoCandidate waiting[]{leased, native};
  Check(SelectBody(waiting) == 1, "current PRG or wait releases H to the next tier");
  leased.current_script_use = false;
  Check(BodyTier(leased) == 4, "story H may be borrowed again once free");
  for (auto excluded : {story, native}) {
    excluded.ann = true; Check(BodyTier(excluded) == -1, "Ann never enters fallback tiers");
    excluded.ann = false; excluded.hero = true;
    Check(BodyTier(excluded) == -1, "hero never enters fallback tiers");
    excluded.hero = false; excluded.grabbed = true;
    Check(BodyTier(excluded) == -1, "grabbed rigs never enter fallback tiers");
    excluded.grabbed = false; excluded.dead = true;
    Check(BodyTier(excluded) == -1, "dead rigs never enter fallback tiers");
  }
  native.other_human = false;
  Check(BodyTier(native) == -1, "a generic non-H rig is not proof of a human family");
}
void NameTags() {
  using namespace kk::coop::nametag;
  Camera c;
  c.view = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  c.projection = {1,0,0,0, 0,2,0,0, 0,0,1.01f,1, 0,0,-.1f,0};
  const Rect screen{0,0,800,600};
  auto p = Project(c,{0,0,10},screen,20);
  Check(p && p->on_screen && !p->behind && Near(p->x,400) && Near(p->y,300), "camera forward projects to centre");
  p = Project(c,{5,1,10},screen,20);
  Check(p && p->on_screen && Near(p->x,600) && Near(p->y,240), "known right/up point with horizontal and vertical FOV");
  // A translated camera looking along +world X, with +world Y as screen right.
  c.view = {0,0,1,0, 1,0,0,0, 0,1,0,0, -20,-30,-10,1};
  p = Project(c,{20,25,31},screen,20);
  Check(p && p->on_screen && Near(p->x,600) && Near(p->y,240), "world-to-view rotation and eye translation precede projection");
  c.view = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  p = Project(c,{2,0,1},screen,20);
  Check(p && !p->on_screen && !p->behind && Near(p->x,780) && Near(p->y,300), "right edge clamp leaves inset");
  p = Project(c,{2,2,1},screen,20);
  Check(p && !p->on_screen && Near(p->y,20) && p->x > 400 && p->x < 780, "diagonal clamp preserves aspect-correct bearing");
  p = Project(c,{1,0,-10},screen,20);
  Check(p && p->behind && !p->on_screen && Near(p->x,780) && p->dx > 0, "behind-camera right target never mirrors to left");
  p = Project(c,{0,0,-10},screen,20);
  Check(p && p->behind && Near(p->x,400) && Near(p->y,580), "exactly behind has deterministic bottom arrow");
  p = Project(c,{1,0,0},screen,20);
  Check(p && !p->on_screen && Near(p->x,780), "camera-plane point clamps without division by zero");
  const auto letterbox = Picture(1920,1200,16.f/9.f,true);
  Check(Near(letterbox.x,0) && Near(letterbox.y,60) && Near(letterbox.h,1080), "letterbox uses displayed picture");
  const auto stretch = Picture(1920,1200,16.f/9.f,false);
  Check(Near(stretch.h,1200) && Near(stretch.y,0), "stretch fills output regardless of render scale");
  c.projection[0] /= 2;  // Hor+ retains projection Y and doubles horizontal coverage.
  p = Project(c,{5,1,10},{100,50,1600,600},20);
  Check(p && Near(p->x,1100) && Near(p->y,290), "ultrawide projection and nonzero picture origin");
  c.projection[0] *= 2; c.projection[5] *= -1; c.view[5] = -1;
  p = Project(c,{5,1,10},screen,20);
  Check(p && Near(p->x,600) && Near(p->y,240), "signed guest camera/projection pair is retained");
  c.projection[0] = std::numeric_limits<float>::quiet_NaN();
  Check(!Project(c,{0,0,10},screen,20), "invalid camera cannot reach ImGui");
  c.projection[0] = 1;
  Check(!Project(c,{0,0,10},{0,0,10,10},20), "empty inset cannot clamp invalid bounds");
  Check(Opacity(.5f) == 0 && Near(Opacity(2),.65) && Near(Opacity(45),.65) &&
      Near(Opacity(52.5f),.325) && Opacity(60) == 0 && Opacity(100) == 0, "close/far name and arrow fades");
  Check(FontScale(40) < FontScale(5), "name shrinks with distance");

  const Point root{10,20,30};
  std::optional<Point> head;
  ConsiderHead(root,{10.1f,20,31.45f},head);  // Upper spine.
  ConsiderHead(root,{10.55f,20,31.6f},head);  // Lateral shoulder/attachment.
  Check(head && Near(head->z,31.45),"lateral joint cannot shift tag toward shoulder");
  ConsiderHead(root,{10.1f,20.05f,31.85f},head);
  ConsiderHead(root,{10,20,31.3f},head);
  ConsiderHead(root,{10,20,33},head);
  ConsiderHead(root,{10,20,std::numeric_limits<float>::quiet_NaN()},head);
  const auto anchor = AboveHead(root,head);
  Check(Near(anchor.x,10.1) && Near(anchor.y,20.05) && Near(anchor.z,32.15),
      "highest central joint retains head XY and adds 0.3 world units exactly once");
  const auto fallback = AboveHead(root,std::nullopt);
  Check(Near(fallback.x,10) && Near(fallback.y,20) && Near(fallback.z,32.1),
      "missing bones use body root plus human height and head clearance");
  // Guest world Z is up; a camera facing +Y must move the tag up, never sideways.
  c.view = {1,0,0,0, 0,0,1,0, 0,1,0,0, 0,0,0,1};
  c.projection = {1,0,0,0, 0,2,0,0, 0,0,1.01f,1, 0,0,-.1f,0};
  const auto skull = Project(c,{1,10,1.8f},screen,20);
  const auto overhead = Project(c,AboveHead({1,10,0},Point{1,10,1.8f}),screen,20);
  Check(skull && overhead && Near(skull->x,overhead->x) && Near(skull->y-overhead->y,18),
      "0.3 world-unit head clearance projects vertically with no horizontal bias");

  // Keep the known projection independent of output pixels and Windows DPI.
  c.view = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  c.projection = {1,0,0,0, 0,2,0,0, 0,0,1.01f,1, 0,0,-.1f,0};
  for (float dpi : {1.f,1.25f,1.5f,1.875f,2.f}) {
    for (float output_height : {1350.f,1500.f}) {
      for (bool box : {false,true}) {
        for (float aspect : {16.f/9.f,21.f/9.f,32.f/9.f}) {
          c.projection[0] = (16.f/9.f)/aspect;
          auto picture = Picture(2400/dpi,output_height/dpi,aspect,box);
          picture.x += 37; picture.y += 19;  // Nonzero viewport origin.
          const auto projected_head = Project(c,{.5f,.8f,10},picture,20/dpi);
          Check(projected_head && projected_head->on_screen,"head stays in fitted picture across DPI/aspects");
          for (float width : {25.f,70.f,210.f}) {
            const auto bounds = TextBounds(*projected_head,width/dpi,20/dpi,picture,8/dpi);
            Check(Near(bounds.x+bounds.w*.5f,projected_head->x) && Near(bounds.y+bounds.h,projected_head->y),
                "tag centre and bottom meet projected anchor for short/long names at any DPI");
            auto pixels = Picture(2400,output_height,aspect,box);
            pixels.x += 37*dpi; pixels.y += 19*dpi;
            const auto physical = Project(c,{.5f,.8f,10},pixels,20);
            Check(physical && physical->on_screen,"physical head projection is valid");
            const auto physical_bounds = TextBounds(*physical,width,20,pixels,8);
            Check(Near(bounds.x*dpi,physical_bounds.x) && Near(bounds.y*dpi,physical_bounds.y),
                "logical coordinates receive framebuffer DPI scale once");
          }
        }
      }
    }
  }
  Projection edge; edge.x=4; edge.y=100;
  const auto left = TextBounds(edge,80,20,screen,8);
  Check(Near(left.x,8),"left-edge text clamps full measured width");
  edge.x=796;
  Check(Near(TextBounds(edge,80,20,screen,8).x,712),"right-edge text clamps full measured width");
  Check(Near(TextBounds(edge,900,20,screen,8).x,8),"overwide text avoids reversed clamp bounds");
}
void DisplayNames() {
  Check(SanitizeDisplayName("") == "Partner", "empty name has neutral fallback");
  Check(SanitizeDisplayName(" \t  Host\n  Player \r") == "Host Player", "trim and collapse whitespace");
  Check(SanitizeDisplayName(std::string("A\0B",3)) == "AB", "embedded NUL removed");
  Check(SanitizeDisplayName("A\x7F\xC2\x85" "B") == "AB", "ASCII and Unicode controls removed");
  Check(SanitizeDisplayName("A\xE2\x80\xAE" "B\xE2\x81\xA6" "C\xEF\xBB\xBF") == "ABC", "bidi and invisible formatting removed");
  Check(SanitizeDisplayName("\xC0\xAF\xED\xA0\x80\xF4\x90\x80\x80") == "Partner", "overlong UTF-8, surrogate and above Unicode rejected");
  Check(SanitizeDisplayName("A\xE2\x82") == "A", "incomplete UTF-8 tail removed");
  const std::string unicode = "Ren\xC3\xA9 \xE7\x8E\x8B \xF0\x9F\xA6\x8D";
  Check(SanitizeDisplayName(unicode) == unicode, "valid multibyte persona preserved");
  Check(SanitizeDisplayName(std::string(31,'a')+"\xC3\xA9") == std::string(31,'a'), "32-byte limit never splits codepoint");
  Check(SanitizeDisplayName(std::string(30,'a')+"\xC3\xA9") == std::string(30,'a')+"\xC3\xA9", "exact 32-byte UTF-8 name retained");
  Message hello; hello.hello = Compatible(); hello.display_name = unicode;
  auto bytes = Packet(hello); Message out;
  Check(Decode(bytes,out) && out.display_name == unicode, "UTF-8 Hello round trip");
  // Mutate the final name bytes without changing packet framing: strict wire validation.
  bytes.back() = 0;
  Check(!Decode(bytes,out), "wire names with NUL are rejected");
  hello.display_name = "A"; bytes = Packet(hello);
  bytes[bytes.size()-2] = 33;
  Check(!Decode(bytes,out), "over-limit wire name rejected before any read");
  hello.display_name = std::string(33,'a');
  Check(!Encode(hello,bytes), "over-limit local Hello rejected");
  hello.display_name = "A\nB";
  Check(!Encode(hello,bytes), "unsanitised local Hello rejected");
  MemoryTransport transport;
  auto config = Config(Role::Host); config.display_name = " A\nB ";
  CoopSession session(config,transport); session.Tick(0);
  // With no peer, inspect the outbound Hello using a loopback receiver.
  transport.peer = &transport; session.Tick(.5);
  Check(!transport.incoming.empty() && Decode(transport.incoming.front(),out) && out.display_name == "A B",
      "session sanitises local identity before sending Hello");
}
}  // namespace
int main() {
  Protocol(); IntegratedMessageRouting(); CheatSets(); Handshake(); Timeouts(); Poses(); PoseReadingAndDisplay(); Disabled(); PuppetPolicies(); PartnerSelection(); NameTags(); DisplayNames();
  std::puts("co-op core tests passed");
}
