// Source-only handoff. No game memory, native calls, SDK, sockets or wall clock.
#include "coop_session.h"
#include "coop_stimulus_debug.h"
#include "coop_revive.h"
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
  static_assert(kProtocolVersion == 30);
  for (const auto& m : {Damage(), Status()}) {
    const auto bytes = Packet(m); Message out;
    Check(bytes.size() == (m.type == MessageType::DamageEvent ? 120u : 103u), "fixed bounded wire length");
    Check(bytes[4] == 0 && bytes[5] == kProtocolVersion && bytes[7] == uint16_t(m.type), "current protocol byte order");
    Check(Decode(bytes, out) && Packet(out) == bytes, "canonical combat round trip");
    for (size_t n = 0; n < bytes.size(); ++n) {
      out.type = MessageType::Bye;
      Check(!Decode(std::span<const uint8_t>(bytes.data(), n), out) && out.type == MessageType::Bye, "truncation preserves output");
    }
    auto extra = bytes; extra.push_back(0); extra[23]++;
    Check(!Decode(extra, out), "trailing combat payload refused");
  }
  {
    auto active=Status(); auto& p=active.player_status;
    p.grab_ready=true; p.grab_enemy={123,4,EnemyFamily::Scorpion}; p.grab_generation=9; p.grab_kind=25;
    p.peer_grab_incarnation=15; p.peer_grab_number=8; p.peer_grab_enemy={456,2,EnemyFamily::Scorpion}; p.peer_grab_generation=4;
    const auto wire=Packet(active); Message decoded;
    Check(Decode(wire,decoded) && Packet(decoded)==wire,"native holder values survive status codec");
    auto malformed=wire; malformed[53]=2;
    Check(!Decode(malformed,decoded),"grab readiness is a canonical boolean");
    auto bad=active; bad.player_status.grab_enemy.family=EnemyFamily::Raptor;
    Check(!Encode(bad,malformed),"unaudited grab family refused");
    bad=active; bad.player_status.grab_kind=24;
    Check(!Encode(bad,malformed),"unaudited native controller kind refused");
    bad=active; bad.player_status.condition=PlayerCondition::Dead;
    Check(!Encode(bad,malformed),"a dead incarnation cannot retain a grab or readiness");
    bad=Status(); bad.player_status.grab_kind=25;
    Check(!Encode(bad,malformed),"empty holder requires an empty native grab payload");
  }
  {
    auto previous=Packet(Damage()); previous[5]=23;
    Message decoded;
    Check(Decode(previous,decoded) && decoded.protocol==23,
        "decoder preserves protocol23 for the session version gate");
  }
  auto m = Damage(); std::vector<uint8_t> out{9};
  for (auto flags : {0u, 1u, 3u, 0xFFFFFFFFu, 0x4005u}) {
    m.damage.flags = flags; Check(!Encode(m, out) && out == std::vector<uint8_t>{9}, "unaudited flags refuse atomically");
  }
  m = Damage(); m.damage.magnitude = 1000; Check(!Encode(m, out), "flag magnitude pair must match producer");
  for (const auto& pair : {std::pair{4096u, 1u}, {16644u, 1000u}, {16388u, 10u}, {16386u, 6u}, {16388u, 6u}, {2048u,1u}, {2048u,2u}, {2048u,4u}}) {
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
  m.damage.direction = {0, 0, 0}; Check(Encode(m, out), "native consumer handles a zero direction");
  m.damage.direction = {2, 0, 0}; Check(Encode(m, out), "scaled native forward is retained for Jack's native normalization");
  m.damage.direction = {2001, 0, 0}; Check(!Encode(m, out), "unbounded direction refused");
  m = Damage(); m.damage.enemy.family = EnemyFamily::Prey; Check(!Encode(m, out), "nonattacking family refused");
  for (const auto [flags,magnitude] : {std::pair{18960u,1000u}, {8192u,1000u}, {2560u,1000u},
       {516u,1u}, {4u,0u}, {256u,0u}}) {
    auto grab=Damage(); grab.damage.flags=flags; grab.damage.magnitude=magnitude;
    Message decoded;
    Check(Decode(Packet(grab),decoded) && decoded.damage.flags==flags && decoded.damage.magnitude==magnitude,
        "native raptor grab damage and release reactions survive transport unchanged");
    grab.damage.magnitude=magnitude+1;
    Check(!Encode(grab,out),"grab flags cannot carry a scalar from a different native attack");
  }
  for (const auto flags : {256u,512u}) {
    auto release=Damage(); release.damage.enemy.family=EnemyFamily::Scorpion;
    release.damage.flags=flags; release.damage.magnitude=0;
    Message decoded;
    Check(Decode(Packet(release),decoded) && decoded.damage.flags==flags && !decoded.damage.magnitude,
        "native scorpion zero scalar hold and release reactions survive transport");
    release.damage.projectile=true;
    Check(!Encode(release,out),"scorpion zero scalar reactions cannot borrow a projectile route");
    release.damage.projectile=false; release.damage.flags=1;
    Check(!Encode(release,out),"other zero scalar scorpion attacks stay refused");
  }
  {
    auto contact=Damage(); contact.damage.enemy.family=EnemyFamily::Marine;
    contact.damage.flags=512; contact.damage.magnitude=0; contact.damage.part=64;
    Message decoded;
    Check(Decode(Packet(contact),decoded) && !decoded.damage.magnitude && decoded.damage.flags==512,
        "native marine flag driven contact retains its zero scalar");
    contact.damage.flags=0; Check(!Encode(contact,out),"zero scalar does not admit an empty hit");
  }
  for (auto flags : {1u,33u}) {
    auto impulse=Damage(); impulse.damage.enemy.family=EnemyFamily::VRex;
    impulse.damage.flags=flags; impulse.damage.magnitude=0; impulse.damage.projectile=true;
    impulse.damage.point={1,2,3};
    Message decoded;
    Check(Decode(Packet(impulse),decoded) && decoded.damage.flags==flags && !decoded.damage.magnitude &&
        decoded.damage.projectile && decoded.damage.point.z==3,
        "native V Rex neighbor impulse preserves zero scalar and contact point");
    impulse.damage.projectile=false; impulse.damage.point={};
    Check(!Encode(impulse,out),"zero V Rex scalar needs the audited impact route");
    impulse.damage.projectile=true; impulse.damage.flags=2;
    Check(!Encode(impulse,out),"other zero scalar V Rex flags remain refused");
    impulse.damage.flags=flags; impulse.damage.enemy.family=EnemyFamily::Bat;
    Check(!Encode(impulse,out),"neighbor impulse cannot borrow another family");
  }
  for (auto family : {EnemyFamily::Crab, EnemyFamily::Worm, EnemyFamily::Scorpion, EnemyFamily::Bat,
       EnemyFamily::Centipede, EnemyFamily::Native, EnemyFamily::NativeKna, EnemyFamily::Marine, EnemyFamily::VRex,
       EnemyFamily::SwampCrawler, EnemyFamily::SwampCrawlerSmall, EnemyFamily::Spider, EnemyFamily::Pny}) {
    auto hit = Damage(); hit.damage.enemy.family = family; hit.damage.flags = 4096;
    hit.damage.magnitude = 10; hit.damage.direction = {3, 4, 0}; hit.damage.part = 64; hit.damage.duration = .025f;
    Message decoded;
    Check(Decode(Packet(hit), decoded) && decoded.damage.part == 64 && decoded.damage.duration == .025f &&
        decoded.damage.direction.x == 3 && decoded.damage.enemy.family == family,
        "native family hit retains raw vector, part and lifetime");
    hit.damage.duration = 0; Check(!Encode(hit, out), "zero lifetime refused");
    hit.damage.duration = 11; Check(!Encode(hit, out), "excess lifetime refused");
    hit.damage.duration = .001f; hit.damage.part = 0x40000000;
    Check(!Encode(hit, out), "native address cannot cross as a hit part");
    hit.damage.part = 64; hit.damage.flags = UINT32_MAX;
    Check(!Encode(hit, out), "unbounded native flags refused");
    hit.damage.flags = 4096; hit.damage.magnitude = 1000001;
    Check(!Encode(hit, out), "native damage magnitude bounded");
  }
  m = Damage(); m.damage.enemy.source = 0; Check(!Encode(m, out), "missing enemy identity refused");
  for (auto family : {EnemyFamily::Native,EnemyFamily::Raptor,EnemyFamily::VRex}) {
    auto hit=Damage(); hit.damage.enemy.family=family; hit.damage.projectile=true;
    hit.damage.flags=0x400400; hit.damage.magnitude=10; hit.damage.surface=19; hit.damage.point={12,34,56};
    Message decoded; Check(Decode(Packet(hit),decoded) && decoded.damage.projectile && decoded.damage.point.z==56 &&
        decoded.damage.surface==19 && decoded.damage.generation==0,"native projectile retains impact and initial owner generation");
    auto bytes=Packet(hit); bytes[bytes.size()-2]=2; Check(!Decode(bytes,decoded),"malformed projectile discriminator refused");
    hit.damage.surface=0x40000000; Check(!Encode(hit,out),"projectile selector cannot carry a native pointer");
    hit.damage.surface=19; hit.damage.projectile=false; Check(!Encode(hit,out),"melee cannot smuggle unused impact data");
  }
  {
    auto sting=Damage(); sting.damage.enemy.family=EnemyFamily::Scorpion;
    sting.damage.flags=5; sting.damage.magnitude=10; sting.damage.attack_sound=true;
    Message decoded; Check(Decode(Packet(sting),decoded) && decoded.damage.attack_sound,"observed sting cue retained");
    auto bytes=Packet(sting); bytes.back()=2; Check(!Decode(bytes,decoded),"malformed sound cue refused");
    sting.damage.flags=3; Check(!Encode(sting,out),"grab start cannot invent a sting sound");
    sting.damage.flags=5; sting.damage.enemy.family=EnemyFamily::Native;
    Check(!Encode(sting,out),"other families cannot request scorpion sound");
    sting.damage.enemy.family=EnemyFamily::Scorpion; sting.damage.projectile=true;
    Check(!Encode(sting,out),"projectiles cannot request scorpion sting sound");
  }
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
  for (const auto track : {0x8258C5F8u,0x824DF570u}) {
    const auto layout=NativeProjectile(track);
    Check(layout.bytes && layout.source && !layout.Reference(layout.source),
        "projectile source is never an exempted victim or homing reference");
    for (const auto offset : track==0x8258C5F8u ? std::vector<uint32_t>{264,364,428} :
         std::vector<uint32_t>{76,176,228}) {
      Check(layout.Reference(offset),"native collision and homing target fields are combat references");
      Check(!layout.Reference(offset-4) && !layout.Reference(offset+4),
          "adjacent projectile words cannot exempt an authored story reference");
    }
    Check(!NativeProjectile(track+4).Reference(264) && !NativeProjectile(0).bytes,
        "unknown and adjacent tracks have no projectile reference permission");
  }
  for (const auto caller : {0x82473380u,0x8248C9A4u,0x8248D10Cu,0x824866F0u,0x82487D9Cu,
       0x824884B8u,0x82468C40u,0x8246B024u,0x8248FC34u}) {
    Check(NativeImpactProducer(EnemyFamily::VRex,caller),"audited direct native V Rex contact producer admitted");
    Check(!NativeImpactProducer(EnemyFamily::Raptor,caller) && !NativeImpactProducer(EnemyFamily::Pny,caller),
        "a direct hit producer cannot borrow another family's identity");
    Check(!NativeImpactProducer(EnemyFamily::VRex,caller+4),"adjacent unaudited native caller refused");
  }
  Check(RemoteOwner(EnemyOwner::Host, EnemyOwner::Guest, 4, 4, false) &&
      RemoteOwner(EnemyOwner::Guest, EnemyOwner::Host, 7, 7, false) &&
      RemoteOwner(EnemyOwner::Host, EnemyOwner::Guest, 0, 0, false), "initial host and either committed simulation owner may hit remote player");
  Check(!RemoteOwner(EnemyOwner::Host, EnemyOwner::Host, 4, 4, false) &&
      !RemoteOwner(EnemyOwner::Host, EnemyOwner::Guest, 5, 4, false) &&
      !RemoteOwner(EnemyOwner::Guest, EnemyOwner::Host, 4, 5, false) &&
      !RemoteOwner(EnemyOwner::Host, EnemyOwner::Guest, 4, 4, true) &&
      !RemoteOwner(EnemyOwner::Host, EnemyOwner::Guest, 1, 0, false) &&
      !RemoteOwner(EnemyOwner(2), EnemyOwner::Guest, 0, 0, false), "replica, old generation and frozen-owner hits cannot damage Jack");
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
  for (auto family : {EnemyFamily::Raptor, EnemyFamily::Worm, EnemyFamily::Scorpion, EnemyFamily::Spider, EnemyFamily::Centipede}) {
    const auto table = Candidates(family);
    Check(CandidateReference(table, table.actors, 1) &&
        CandidateReference(table, table.actors+4*(table.limit-1), table.limit), "occupied family candidate span allowed");
    Check(!CandidateReference(table, table.actors, 0) && !CandidateReference(table, table.actors, table.limit+1) &&
        !CandidateReference(table, table.actors+1, table.limit) && !CandidateReference(table, table.actors+4*table.limit, table.limit),
        "family candidate alias, overflow and unoccupied guards retained");
    Check(!CandidateReference(table, 19812, table.limit), "mission cursor never exempted as combat reference");
  }
}
void Deaths() {
  using namespace revive;
  Recovery r;
  Check(r.Step(false,false,false,false,1)==Action::None,"living player never reloads for missing status");
  Check(r.Step(true,true,false,false,10)==Action::Wait && r.Downed(),"lone death starts delay");
  Check(r.Step(true,true,false,true,12.99)==Action::Wait,"native reset cannot shorten three seconds");
  Check(r.Step(true,true,false,false,13)==Action::LoadLocal,"reset latch permits one private load at delay");
  Check(r.Step(true,true,false,true,13.1)==Action::Wait,"repeated reset cannot request another load");
  r.Loaded(); Check(r.Step(false,true,false,false,14)==Action::Place,"native load alone is not a completed revive");
  Check(r.Downed(),"announce down until placement completes");
  r.Placed(); Check(!r.Downed() && r.Step(false,true,false,false,15)==Action::None,"placement completes life");
  Check(r.Step(true,true,false,true,20)==Action::Wait,"second death has independent delay");
  Check(r.Step(true,true,true,true,23)==Action::ReloadBoth,"both dead wins exactly when revive is due");
  r={}; r.Step(true,true,false,true,1); r.Step(true,true,false,false,4); r.Loaded();
  Check(r.Step(false,true,true,false,5)==Action::ReloadBoth,"peer dying during private load prevents placement");
  for (bool reset : {false,true}) {
    r={}; Check(r.Step(true,false,false,reset,1)==Action::ReloadBoth,"away, stale, disconnected or different-level peer reloads both");
  }
  r={}; r.Step(true,true,false,false,1);
  Check(r.Step(true,true,false,false,17)==Action::ReloadBoth,"missing native death completion has bounded fallback");
  r={}; r.Step(true,true,false,true,1); r.Step(true,true,false,false,4);
  Check(r.Step(false,true,false,false,125)==Action::ReloadBoth,"load/placement timeout cannot strand Jack");
  r={}; r.Step(true,true,false,false,10);
  Check(r.Step(true,true,false,false,9)==Action::ReloadBoth,"clock rollback fails closed");
  r={}; Check(r.Step(true,true,false,true,std::numeric_limits<double>::quiet_NaN())==Action::None && !r.Downed(),"NaN cannot mutate recovery");
  PlayerPose p; p.x=10; p.y=20; p.z=7; p.facing=0;
  auto position=Beside(p); Check(position.x==8.5f && position.y==20 && position.z==7,"1.5 units behind at peer height");
  p.facing=1.57079632679f; position=Beside(p);
  Check(std::abs(position.x-10)<.001f && std::abs(position.y-18.5f)<.001f && position.z==7,"offset follows facing, not local dead camera");
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
  {
    Pair reverse;
    Check(reverse.guest.QueueCombat(Damage(), .1) && reverse.host.QueueCombat(Status(), .1), "guest owner sends hit and host publishes independent life");
    reverse.Tick(.11); reverse.Tick(.12);
    Check(reverse.host.TakeCombat().size() == 1 && reverse.guest.TakeCombat().size() == 1, "reverse owner hit and life status cross reliable transport");
  }
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
  // Both directions reach the game-thread owner/generation validator.
  p.b.incoming.push_back(Packet(Status())); p.a.incoming.push_back(Packet(Damage())); p.Tick(3.02);
  Check(p.host.TakeCombat().size() == 1 && p.guest.TakeCombat().size() == 1, "both directions reach ownership validation");
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
