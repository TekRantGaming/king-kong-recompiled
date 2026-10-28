// Actual KK UDP/protocol/session/flow/story/ownership tests, with no native engine.
// Diagnostic/test method: coop-anything b877c9e, see tools/coop-chaos/LICENSE.
#include "coop_direct_udp.h"
#include "coop_session.h"
#include "coop_flow.h"
#include "coop_ownership.h"
#include "coop_story.h"
#include "coop_desync.h"
#include "coop_combat.h"
#include "coop_stimulus_debug.h"
#include <map>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

using namespace kk::coop;
namespace {
const char* test_name="all";
void Check(bool ok,const char* why) {
  if (!ok) { std::fprintf(stderr,"FAIL [%s]: %s\n",test_name,why); std::exit(1); }
}
Compatibility Compatible() { return {kProtocolVersion,"chaos","actual-core","none"}; }
SessionConfig Config(bool host) {
  SessionConfig c{true,host ? Role::Host : Role::Guest,host ? 1234567ull : 0,Compatible()};
  c.join_code="CHAOS7"; return c;
}
std::unique_ptr<DirectUdpTransport> Host(UdpSimulation sim={}) {
  // Private loopback only. Search bounded ports so other test jobs may coexist.
  for (unsigned p=33480;p<33580;++p) {
    auto t=CreateDirectUdp({true,"127.0.0.1",uint16_t(p),"CHAOS7",sim});
    if (t->State()==TransportState::Ready) return t;
  }
  Check(false,"bind private UDP host"); return {};
}
struct Pair {
  std::unique_ptr<DirectUdpTransport> a,b;
  CoopSession host,guest;
  double now=100;
  static constexpr double offset=12345;
  Pair(UdpSimulation sim={}) : a(Host(sim)),
      b(CreateDirectUdp({false,"127.0.0.1",a->LocalPort(),"CHAOS7",sim})),
      host(Config(true),*a),guest(Config(false),*b) {
    Check(b->State()==TransportState::Ready,"open guest");
  }
  void Step() {
    host.Tick(now,PlayerPose{now,101,1,2,3});
    guest.Tick(now+offset,PlayerPose{now+offset,101,4,2,3}); now+=.01;
  }
  void Pump(unsigned n) { while(n--) Step(); }
  bool Connected() const { return host.State()==SessionState::Connected && guest.State()==SessionState::Connected; }
  void Connect() {
    for (unsigned n=0;n<1900 && !Connected();++n) Step();
    Check(Connected(),"actual UDP session handshake within 19 seconds");
    Pump(100);
  }
};
StoryEvent Event(uint32_t n=1) { return {1,101,n,0x123456789abcdef0ull,0xfedcba9876543210ull,1,n,StoryPhase::Advance}; }
EnemyState Enemy() {
  EnemyState e; e.id={0x123456789abcdef0ull,1,EnemyFamily::Raptor}; e.life=100; return e;
}
void ProjectileSource() {
  // Sparse native memory fixture. at() makes any unexpected dereference a
  // failed process, never a successful mutation catch.
  constexpr uint32_t actor=0x41000000,ext=0x41001000,ai=0x41002000,vars=0x41003000,
      block=0x41004000,source=0x42000000;
  std::map<uint32_t,uint32_t> words{{actor+24,ext},{ext+4,ai},{ai+64,vars},
      {ai+80,2},{ai+82,2},{ai+32,block},{block+20,0x8258C5F8},{vars+232,source}};
  bool bound=true,owned=true,retained=true;
  const auto check=[&](uint32_t offset) {
    return combat::ProjectileReference(actor,ai,vars,offset,
        [&](uint32_t p){return words.at(p);},[&](uint32_t p){return uint16_t(words.at(p));},
        [](uint32_t p,uint32_t bytes){return stimulus_debug::Address(p,bytes);},
        [&](uint32_t p){return p==source && bound && owned && retained;});
  };
  Check(check(264) && check(364) && check(428),"owned spear keeps actual contact and homing references");
  Check(!check(232) && !check(268),"thrower and adjacent words retain story protection");
  bound=false; Check(!check(264),"unbound projectile source cannot excuse a story reference"); bound=true;
  owned=false; Check(!check(264),"replica projectile cannot excuse a story reference"); owned=true;
  retained=false; Check(!check(264),"destroyed or recycled enemy source cannot excuse a story reference"); retained=true;
  words[vars+232]=source+4; Check(!check(264),"different source is not authenticated by a valid neighbor");
  words[vars+232]=source;
  words[ext+4]=ai+4; Check(!check(264),"projectile AI reuse refuses old binding"); words[ext+4]=ai;
  words[ai+64]=vars+4; Check(!check(264),"projectile vars reuse refuses old binding"); words[ai+64]=vars;
  words[block+20]=0x8258C5FC; Check(!check(264),"unknown track cannot borrow a projectile layout");
  words[block+20]=0x8258C5F8; words[ai+82]=4097;
  Check(!check(264),"out of range track table is refused before any extra read");
}
void ImpactSelector() {
  Message m; m.type=MessageType::DamageEvent; m.epoch=1;
  m.damage.world={204,3,1}; m.damage.incarnation=1; m.damage.number=1;
  m.damage.enemy={0x1234567812345678ull,0,EnemyFamily::Native};
  m.damage.flags=0x400400; m.damage.magnitude=11; m.damage.direction={1,0,0};
  m.damage.projectile=true; m.damage.point={28,70,24};
  std::vector<uint8_t> bytes;
  // Native Javelin launch seeds this integer word with float tolerance bits
  // on its H collision path. The native impact builder uses its low byte.
  for (const auto word : {0x38D1B717u,0x3A83126Fu,0x3DCCCCCDu,0x40000000u,0xFFFFFFFEu,0x0000FFFFu}) {
    m.damage.surface=word;
    if (word>65535 && word!=UINT32_MAX)
      Check(!Encode(m,bytes),"raw native high bits cannot be accepted as a wire selector");
    m.damage.surface=combat::ImpactChild(word);
    Check(Encode(m,bytes),"effective native projectile child selector permits the real hit to be encoded");
    Message out;
    Check(Decode(bytes,out) && out.damage.surface==(word&255) && out.damage.flags==0x400400 &&
        out.damage.magnitude==11 && out.damage.point.z==24,
        "selector normalization preserves impact and damage while discarding unused high bits");
  }
  Check(combat::ImpactChild(UINT32_MAX)==UINT32_MAX && combat::ImpactChild(0)==0 && combat::ImpactChild(255)==255,
      "native no-child sentinel and actual byte selectors are retained");
}

void Model() {
  for (const auto name:{"lan","home","bad_wifi","mobile","hell"}) {
    UdpSimulation p; Check(UdpProfile(name,p) && ValidUdpSimulation(p),"registered valid profile");
    UdpImpairment a,b,other; a.Reset(p,true); b.Reset(p,true); other.Reset(p,false);
    bool different=false;
    for (unsigned n=0;n<20000;++n) {
      const double at=n*.003;
      const auto x=a.Decide(at,1000),y=b.Decide(at,1000),z=other.Decide(at,1000);
      Check(x.copies==y.copies && x.reason==y.reason && x.delay==y.delay,"same seed and schedule repeat bit-exactly");
      different|=x.copies!=z.copies || x.reason!=z.reason || x.delay!=z.delay;
    }
    Check(different,"directions use independent random streams");
    const auto s=a.Stats(); Check(s.seen==20000 && s.seen==s.dropped+s.forwarded-s.duplicated,"model accounts all original datagrams");
    if (std::string_view(name)=="bad_wifi" || std::string_view(name)=="hell")
      Check(s.dropped && s.duplicated && s.reordered && s.reasons[size_t(UdpDrop::Burst)],"profile really exercises burst loss, copies and reorder");
    if (std::string_view(name)=="mobile" || std::string_view(name)=="hell")
      Check(s.reasons[size_t(UdpDrop::Bandwidth)]>0,"cap really tail-drops saturated traffic");
    std::printf("model profile=%s seed=%u seen=%llu drops=%llu duplicates=%llu reorder=%llu bandwidth=%llu\n",name,p.seed,
        (unsigned long long)s.seen,(unsigned long long)s.dropped,(unsigned long long)s.duplicated,
        (unsigned long long)s.reordered,(unsigned long long)s.reasons[size_t(UdpDrop::Bandwidth)]);
  }
  UdpSimulation p; Check(!UdpProfile("unknown",p),"unknown profile does not silently select lan");
  p.duplicate_percent=101; Check(!ValidUdpSimulation(p),"invalid percentage rejected");
  p={}; p.delay_ms=std::numeric_limits<double>::quiet_NaN(); Check(!ValidUdpSimulation(p),"NaN rejected");
  p={}; p.jitter_ms=100; Check(ValidUdpSimulation(p),"jitter may exceed base delay and remains clamped");
  p={}; p.burst_enter_percent=100; p.burst_exit_percent=0;
  UdpImpairment burst; burst.Reset(p,true);
  for(int i=0;i<20;++i) Check(burst.Decide(i,100).reason==UdpDrop::Burst,"forced contiguous loss burst");
  p={}; p.delay_ms=100; p.reorder_percent=100; p.duplicate_percent=100;
  UdpImpairment copy; copy.Reset(p,true); auto v=copy.Decide(0,100);
  Check(v.copies==2 && v.delay[0]==0 && v.delay[1]==0,"explicit reorder skips latency and duplicate has two copies");
  p={}; p.bandwidth_bps=8000; p.buffer_ms=0;
  UdpImpairment cap; cap.Reset(p,true); v=cap.Decide(0,1000);
  Check(v.copies==1 && v.delay[0]==1 && cap.Decide(0,1).reason==UdpDrop::Bandwidth,"cap uses bits/sec with bounded tail drop");
  Check(cap.Decide(1,1000).copies==1,"bandwidth refills after serialization");
  p.duplicate_percent=100; p.reorder_percent=100; p.buffer_ms=5000;
  cap.Reset(p,true); v=cap.Decide(0,1000);
  Check(v.copies==2 && v.delay[0]==1 && v.delay[1]==2,"duplicates consume bandwidth and reorder cannot bypass serialization");
  p={}; p.delay_ms=100; p.jitter_ms=50;
  cap.Reset(p,true); bool early=false,late=false;
  for(int n=0;n<1000;++n) {
    v=cap.Decide(n,100);
    Check(v.copies==1 && v.delay[0]>=.05 && v.delay[0]<=.15,"latency and symmetric jitter have bounded units");
    early|=v.delay[0]<.1; late|=v.delay[0]>.1;
  }
  Check(early && late,"jitter varies on both sides of base delay");
  p={}; p.one_way_at=2; p.one_way_seconds=3; p.one_way_roles=1;
  UdpImpairment host,guest; host.Reset(p,true); guest.Reset(p,false);
  Check(host.Decide(1,1).copies==1 && host.Decide(2,1).reason==UdpDrop::OneWay &&
      guest.Decide(2,1).copies==1 && host.Decide(5,1).copies==1,"one way interval has exact boundaries and role");
  p={}; p.disconnect_at=2; p.disconnect_seconds=4; host.Reset(p,true); guest.Reset(p,false);
  Check(host.Decide(2,1).reason==UdpDrop::Disconnect && guest.Decide(2,1).reason==UdpDrop::Disconnect &&
      host.Decide(6,1).copies==1 && guest.Decide(6,1).copies==1,"full outage heals in both directions");
}

void OwnershipGeneration() {
  enemies::Authority a; auto e=Enemy(); e.generation=4; a.generation=4; a.last=e;
  Check(a.Observe(e),"current owner state is admitted");
  e.generation=3; Check(!a.Observe(e) && a.last.generation==4,"stale ownership generation never wins");
  e.generation=4; e.owner=EnemyOwner::Guest; Check(!a.Observe(e),"wrong owner never wins");
}
void StoryDuplicate() {
  story::Ordered q; q.Reset(1,101); const auto e=Event(); unsigned applications=0;
  Check(q.Push(e,1) && q.Front(),"first story action accepted"); q.Applied(1); ++applications;
  if(q.Push(e,2) && q.Front()) { q.Applied(2); ++applications; }
  Check(applications==1 && q.Next()==2,"an applied story event has exactly one effect");
}
void StoryWorld() {
  story::Ordered q; q.Reset(2,101);
  Check(!q.Push(Event(),1) && !q.Front(),"old load cannot alter new world");
  auto e=Event(); e.generation=2; e.level=102;
  Check(!q.Push(e,1) && !q.Front(),"wrong level cannot alter world");
}
void DigestCompare() {
  story::Checksums q; auto e=Event(); e.phase=StoryPhase::Checksum;
  Check(q.Remember(e,1),"remember captured boundary");
  auto other=e; ++other.record;
  Check(q.Compare(other)==story::Checksums::Result::Mismatch,"different digest must be detected");
  Check(q.Compare(other)==story::Checksums::Result::Ignored,"reply consumed once");
  Check(q.Remember(e,2) && q.Compare(e)==story::Checksums::Result::Match,"equal digests match");
}
void FalseQuiet() {
  desync::Guard g; desync::Evidence quiet;
  for(unsigned n=0;n<100;++n)
    Check(g.Observe(1,101,1,quiet,n*3)==desync::Tier::None,"equal worlds and silence cause no repair");
  quiet.quiet=true;
  Check(g.Observe(1,101,1,quiet,300)==desync::Tier::None &&
      g.Observe(1,101,1,quiet,303)==desync::Tier::None &&
      g.Observe(1,101,1,quiet,306)==desync::Tier::Quiet,"repeated actual drift reaches quiet repair");
}
void ShotValidation() {
  const EnemyWorld w{101,1,1}; InventoryState inventory{w,100,1}; inventory.ammo[0]=50;
  const PlayerPose peer{100,101,0,0,0};
  ReceivedShot s{{w,1,1,100,{10,0,0},{1,0,0},{}},100,inventory,peer};
  ShotValidator validator;
  Check(validator.Validate(s,w,peer,100)==ShotReject::Origin,"remote shot cannot originate far from actual shooter");
  Check(validator.Validate(s,w,peer,100)==ShotReject::Sequence,"rejected shot cannot replay later");
}
void SessionEpoch() {
  Pair p; p.Connect();
  Message m; m.type=MessageType::StoryEvent; m.story=Event(); m.epoch=p.host.Epoch()+1;
  std::vector<uint8_t> bytes; Check(Encode(m,bytes) && p.a->Send(bytes,true),"inject valid stale-epoch wire below session");
  p.Pump(200); Check(p.guest.TakeStory().empty(),"stale session epoch never reaches application");
  m.epoch=p.host.Epoch(); Check(Encode(m,bytes) && p.a->Send(bytes,true),"inject matching epoch");
  p.Pump(100); Check(p.guest.TakeStory().size()==1,"positive control reaches same application guard");
}
void Reliable() {
  Pair p; p.Connect();
  UdpSimulation faults{150,100,5,77}; faults.duplicate_percent=100; faults.reorder_percent=50;
  p.a->SetSimulation(faults); faults.seed=78; p.b->SetSimulation(faults);
  for (unsigned n=0;n<40;++n) {
    Message m; m.type=MessageType::SaveChunk; m.epoch=p.host.Epoch(); m.save_chunk={n,{uint8_t(n)}};
    std::vector<uint8_t> wire; Check(Encode(m,wire) && p.a->Send(wire,true),"queue bounded real reliable frame");
  }
  unsigned received=0;
  for(unsigned step=0;step<3000;++step,p.now+=.01) {
    p.a->Poll(p.now); p.b->Poll(p.now+Pair::offset);
    std::vector<uint8_t> wire;
    while(p.b->Receive(wire)) {
      Message m; Check(Decode(wire,m),"reliable application packet decodes");
      if(m.type!=MessageType::SaveChunk) continue;
      Check(m.save_chunk.offset==received && m.save_chunk.bytes==std::vector<uint8_t>{uint8_t(received)},
          "reliable delivery is once and in original send order"); ++received;
    }
    while(p.a->Receive(wire)) {}
  }
  Check(received==40,"every reliable message arrives after retry");
  const auto f=p.a->SimulationStats();
  Check(f.dropped && f.duplicated && f.reordered && p.a->Stats().resent,"negative controls reach actual ACK/retry/duplicate/reorder paths");
  Check(!f.queue_full,"normal fault scenario stays within delayed queue bounds");
}

// No native-world stubs pretend to apply objects/pickups: C71 and pickup hooks
// are absent on be30d5dd. Future guards plug in alongside these real core tests.
void ProfileScenario(const char* name,uint32_t seed) {
  UdpSimulation sim; Check(UdpProfile(name,sim),"profile resolves"); sim.seed=seed;
  Pair p(sim); p.Connect();
  FlowMailbox hf,gf;
  SessionStart start; start.chapter=1; start.level=101; start.start=1;
  const std::vector<uint8_t> save(kSessionSaveBytes,0x7b); start.save_bytes=uint32_t(save.size()); start.save_hash=SessionSaveHash(save);
  Check(hf.Begin(start,save),"private session/save begins");
  auto step=[&] { p.Step(); hf.Exchange(p.host,p.now); gf.Exchange(p.guest,p.now+Pair::offset); };
  for(unsigned n=0;n<2500 && !gf.Snapshot().start_ready;++n) step();
  Check(gf.Snapshot().start_ready && gf.Snapshot().save==save,"real save chunks converge exactly");
  Message loaded; loaded.type=MessageType::Loaded; loaded.flow=hf.Snapshot().world;
  Check(hf.Submit(loaded) && gf.Submit(loaded),"both native load observations submitted");
  for(unsigned n=0;n<2500 && (hf.Snapshot().phase!=FlowPhase::Playing || gf.Snapshot().phase!=FlowPhase::Playing);++n) step();
  Check(hf.Snapshot().phase==FlowPhase::Playing && gf.Snapshot().phase==FlowPhase::Playing,"both peers pass load barrier under faults");
  story::Ordered ordered; ordered.Reset(1,101); unsigned applied=0;
  for(unsigned n=1;n<=32;++n) { Message m; m.type=MessageType::StoryEvent; m.epoch=p.host.Epoch(); m.story=Event(n); Check(p.host.QueueStory(m),"queue shared story action"); }
  // Handshake is already complete; cut only the guest's outgoing direction for
  // four seconds, including its ACKs. Host reliable duplicates must be harmless.
  const double outage_started=p.now;
  sim.one_way_at=0; sim.one_way_seconds=4; sim.one_way_roles=2;
  p.b->SetSimulation(sim);
  for(unsigned n=0;n<2000;++n) {
    step();
    for(const auto& m:p.guest.TakeStory()) {
      Check(m.type==MessageType::StoryEvent && ordered.Push(m.story,p.now),"story event accepted once in scoped world");
      while(const auto* front=ordered.Front()) {
        Check(front->number==applied+1,"story applies in causal order"); ++applied; ordered.Applied(p.now);
      }
    }
    if(p.now-outage_started>5 && applied==32 && p.Connected()) break;
  }
  Check(applied==32 && p.Connected(),"one-way outage heals and shared story converges within 20 seconds");
  Check(p.b->SimulationStats().reasons[size_t(UdpDrop::OneWay)]>0,"one-way blackout really dropped outgoing traffic");
  // Exercise retained Prepare/Ready/Commit machinery only. Native gameplay still
  // keeps host authority, and this test does not enable proximity handoffs.
  auto e=Enemy(); enemies::Authority owner,replica; owner.matched=replica.matched=true;
  owner.last=replica.last=e;
  auto prepare=owner.Begin({101,1,1},EnemyOwner::Guest,e);
  Check(bool(prepare),"begin retained ownership handshake");
  Message ownership; ownership.type=MessageType::OwnershipChange; ownership.epoch=p.host.Epoch(); ownership.ownership=*prepare;
  Check(p.host.QueueOwnership(ownership),"send Prepare through actual session");
  for(unsigned n=0;n<2000 && replica.generation!=1;++n) {
    step();
    for(const auto& in:p.guest.TakeOwnership()) {
      if(in.ownership.phase==OwnershipPhase::Prepare) {
        auto ready=replica.Prepare(in.ownership,e); Check(bool(ready),"guest freezes and returns Ready");
        auto out=in; out.ownership=*ready; Check(p.guest.QueueOwnership(out),"send Ready");
      } else Check(replica.Commit(in.ownership),"guest accepts exact Commit once");
    }
    for(const auto& in:p.host.TakeOwnership()) {
      if(in.ownership.phase==OwnershipPhase::Commit) {
        Check(owner.Commit(in.ownership),"host applies local echo only after actual reliable send");
        continue;
      }
      auto commit=owner.Ready(in.ownership);
      if(!commit) std::fprintf(stderr,"ownership diagnostic profile=%s seed=%u phase=%u gen=%u enemy-gen=%u owner=%u enemy-owner=%u pending=%d current-gen=%u\n",
          name,seed,unsigned(in.ownership.phase),in.ownership.generation,in.ownership.enemy.generation,
          unsigned(in.ownership.owner),unsigned(in.ownership.enemy.owner),bool(owner.pending),owner.generation);
      Check(bool(commit),"host validates Ready final state");
      auto out=in; out.ownership=*commit;
      Check(p.host.QueueOwnership(out),"queue Commit without granting authority early");
    }
  }
  Check(owner.generation==1 && replica.generation==1 && owner.owner==replica.owner &&
      !owner.Frozen() && !replica.Frozen(),"retained ownership state converges after faults");
  ownership.ownership={{101,1,1},OwnershipPhase::Matched,0,EnemyOwner::Guest,e};
  ownership.ownership.enemy.owner=EnemyOwner::Guest;
  Check(p.guest.QueueOwnership(ownership),"send delayed old-generation ownership evidence");
  bool stale_owner=false;
  for(unsigned n=0;n<1500 && !stale_owner;++n) {
    step(); for(const auto& in:p.host.TakeOwnership()) {
      Check(!owner.Observe(in.ownership.enemy),"stale generation from same owner never wins"); stale_owner=true;
    }
  }
  Check(stale_owner,"stale ownership oracle sees actual delivered packet");
  // Replay old load and session traffic through the actual reliable adapter.
  auto stale=Event(33);
  ordered.Reset(2,101); stale.generation=1;
  Message m; m.type=MessageType::StoryEvent; m.story=stale; m.epoch=p.host.Epoch();
  std::vector<uint8_t> wire; Check(Encode(m,wire) && p.a->Send(wire,true),"send stale world through actual UDP");
  bool rejected=false;
  for(unsigned n=0;n<1500 && !rejected;++n) {
    step(); for(const auto& in:p.guest.TakeStory()) { Check(!ordered.Push(in.story,p.now),"late old-world story rejected"); rejected=true; }
  }
  Check(rejected,"stale-world oracle has nonzero coverage");
  UdpSimulation lan; Check(UdpProfile("lan",lan),"heal profile");
  p.a->SetSimulation(lan); p.b->SetSimulation(lan); p.Pump(200);
  Check(p.Connected() && p.host.Poses().received && p.guest.Poses().received,"bidirectional live session survives heal");
  std::printf("actual-core profile=%s seed=%u story=%u poses=%llu/%llu converged=yes native-hooks=absent\n",name,seed,applied,
      (unsigned long long)p.host.Poses().received,(unsigned long long)p.guest.Poses().received);
}
void RaptorBiteSound() {
  Check(combat::RaptorBiteSound(EnemyFamily::Raptor,0x8256D578,7,0),"native bite geometry cue admitted");
  Check(!combat::RaptorBiteSound(EnemyFamily::Raptor,0x8256D388,7,0),"scripted animation without contact excluded");
  Check(!combat::RaptorBiteSound(EnemyFamily::Bat,0x8256D578,7,0),"other family cannot use a raptor cue");
  Check(!combat::RaptorBiteSound(EnemyFamily::Raptor,0x8256D578,9,0),"other resources excluded");
  Check(!combat::RaptorBiteSound(EnemyFamily::Raptor,0x8256D578,7,19),"other attachment excluded");
  DamageEvent d{{305,3,5},9,7,{123,4,EnemyFamily::Raptor},0x4104,1000,{1,0,0}};
  d.attack_sound=true; d.generation=2; d.time=10;
  Check(ValidDamageEvent(d) && combat::NativeAttackSoundResource(d)==7,"actual large dinosaur bite can carry native resource7");
  Check(combat::AttackSoundObserved(d,d.enemy,2,9.95,10,false),"recent successful cue matches exact attack source");
  Check(!combat::AttackSoundObserved(d,d.enemy,2,9.95,10,true),"consumed bite cue cannot replay");
  Check(!combat::AttackSoundObserved(d,d.enemy,3,9.95,10,false),"cue cannot cross an authority generation");
  Check(!combat::AttackSoundObserved(d,d.enemy,2,9.8,10,false),"later bite cannot borrow old cue");
  Message m; m.type=MessageType::DamageEvent; m.epoch=9; m.damage=d;
  std::vector<uint8_t> wire; Message decoded;
  Check(Encode(m,wire) && Decode(wire,decoded) && decoded.damage.attack_sound &&
      combat::NativeAttackSoundResource(decoded.damage)==7,"native bite cue survives the real damage codec");
  d.flags=4096; d.magnitude=1; Check(!ValidDamageEvent(d),"other raptor attack forms require their own cue audit");
  d.flags=0x4104; d.magnitude=1000; d.projectile=true;
  Check(!ValidDamageEvent(d),"projectile cannot request a bite sound");
}
void RaptorCamera() {
  const ShotVector foot{1,2,3},view{1.1f,2.1f,4.6f};
  const auto height=[&](EnemyFamily family=EnemyFamily::Raptor,uint32_t kind=16,uint32_t caller=0x82464320,
                        ShotVector camera=ShotVector{1.1f,2.1f,4.6f},bool local=false,bool peer=false) {
    return combat::RaptorViewHeight(family,kind,caller,foot,camera,local,peer);
  };
  Check(height() && std::abs(*height()-1.6f)<.001f,"native view height retained for peer camera sight ray");
  for (const auto caller : {0x82464320u,0x82464374u,0x824643E0u,0x8246446Cu})
    Check(height(EnemyFamily::Raptor,16,caller).has_value(),"each native camera-derived read admitted");
  Check(!height(EnemyFamily::Raptor,16,0x824642B4),"physical target position read is not a camera");
  Check(!height(EnemyFamily::Raptor,16,0x8246430C),"jaw anchor position is not a camera");
  Check(!height(EnemyFamily::Scorpion),"other enemy models do not use raptor camera offsets");
  Check(!height(EnemyFamily::Raptor,14),"small raptor behavior unchanged");
  Check(!height(EnemyFamily::Raptor,16,0x82464320,view,false,true),"different crouch states cannot borrow local camera height");
  Check(!height(EnemyFamily::Raptor,16,0x82464320,{10,2,4.6f}),"detached camera excluded");
  Check(!height(EnemyFamily::Raptor,16,0x82464320,{1,2,20}),"cinematic height excluded");
  Check(!height(EnemyFamily::Raptor,16,0x82464320,{1,2,std::numeric_limits<float>::quiet_NaN()}),"nonfinite camera excluded");
}
void BatContactSound() {
  DamageEvent d{{302,9,5},9,7,{123,4,EnemyFamily::Bat},2,2,{1,0,0}};
  d.generation=2; d.time=10;
  Check(combat::NativeAttackSoundResource(d)==10,"native bat contact selects its own resource");
  Check(combat::BatContactSound(d,d.enemy,2,0x82654038,10,10.01),"successful contact cue can annotate actual unsent hit");
  auto other=d.enemy; ++other.source;
  Check(!combat::BatContactSound(d,other,2,0x82654038,10,10.01),"other source cannot lend contact sound");
  Check(!combat::BatContactSound(d,d.enemy,3,0x82654038,10,10.01),"new owner cannot annotate old hit");
  Check(!combat::BatContactSound(d,d.enemy,2,0x8265408C,10,10.01),"grab cue cannot annotate a player strike");
  Check(!combat::BatContactSound(d,d.enemy,2,0x82654038,14,10.01),"approach cue is not contact sound");
  Check(!combat::BatContactSound(d,d.enemy,2,0x82654038,10,10.2),"old hit cannot acquire later sound");
  Check(!combat::BatContactSound(d,d.enemy,2,0x82654038,10,9.99),"future hit refused");
  d.attack_sound=true;
  Check(ValidDamageEvent(d),"observed bat contact sound fits damage wire");
  Check(!combat::BatContactSound(d,d.enemy,2,0x82654038,10,10.01),"same cue cannot annotate the hit twice");
  d.flags=512; d.magnitude=99;
  Check(!ValidDamageEvent(d) && !combat::NativeAttackSoundResource(d),"bat feeding is not a player strike cue");
}
void NativeScriptTarget() {
  Check(combat::NativePlayerScriptTarget(10,20,10,20),"native authored player may follow accepted peer");
  Check(combat::NativePlayerScriptTarget(20,10,10,20),"reverse player choice retained");
  Check(!combat::NativePlayerScriptTarget(30,20,10,20),"authored story actor never replaced");
  Check(!combat::NativePlayerScriptTarget(10,30,10,20),"nonplayer selection never aliased");
  Check(!combat::NativePlayerScriptTarget(0,20,10,20),"absent authored target never created");
  Check(!combat::NativePlayerScriptTarget(10,10,10,0),"missing peer cannot alias a script");
  Check(!combat::NativePlayerScriptTarget(10,10,10,10),"local hero cannot also be peer");
}
void GrabStart() {
  DamageEvent d{{204,3,5},9,7,{123,4,EnemyFamily::Scorpion},3,10,{1,0,0}};
  Check(combat::NativeScorpionGrabStart(d),"player body native start recognized");
  d.flags=256; d.magnitude=0;
  Check(combat::NativeScorpionGrabStart(d) && ValidDamageEvent(d),"NPC body zero scalar start recognized without new damage");
  d.flags=5; d.magnitude=10; Check(!combat::NativeScorpionGrabStart(d),"sting cannot invent a new grab");
  d.flags=512; d.magnitude=0; Check(!combat::NativeScorpionGrabStart(d),"timeout cannot invent a new grab");
  d.flags=256; d.magnitude=1; Check(!combat::NativeScorpionGrabStart(d),"unknown NPC start magnitude refused");
  d.flags=3; d.magnitude=10; d.projectile=true;
  Check(!combat::NativeScorpionGrabStart(d),"projectile is not a grab start");
  d.projectile=false; d.enemy.family=EnemyFamily::Raptor;
  Check(!combat::NativeScorpionGrabStart(d),"other families require their own native grab adapter");
}
void AttackSound() {
  DamageEvent d{{204,3,5},9,7,{123,4,EnemyFamily::Scorpion},5,10,{1,0,0}};
  d.attack_sound=true;
  Check(ValidDamageEvent(d),"audited sting sound tuple accepted");
  Check(combat::AttackSoundObserved(d,d.enemy,2,9.95,10,false)==false,"cue ownership generation must match");
  d.generation=2;
  Check(combat::AttackSoundObserved(d,d.enemy,2,9.95,10,false),"actual recent cue belongs to exact hit source");
  auto other=d.enemy; ++other.source;
  Check(!combat::AttackSoundObserved(d,other,2,9.95,10,false),"other source cannot lend a cue");
  Check(!combat::AttackSoundObserved(d,d.enemy,2,9.8,10,false),"old sting is not replayed by a later attack");
  Check(!combat::AttackSoundObserved(d,d.enemy,2,10.1,10,false),"future cue refused");
  Check(!combat::AttackSoundObserved(d,d.enemy,2,9.95,10,true),"one observation is consumed once");
  d.flags=3; Check(!ValidDamageEvent(d),"grab start has no sting cue");
  d.flags=5; d.enemy.family=EnemyFamily::Native;
  Check(!ValidDamageEvent(d),"native cannot request a scorpion resource");
}
void GrabFeedback() {
  using F=combat::GrabFeedback;
  const EnemyWorld world{204,3,5}; const EnemyId source{123,4,EnemyFamily::Scorpion};
  PlayerStatus p{world,9,1,0,PlayerCondition::Alive}; p.grab_ready=true;
  const auto feedback=[&](const PlayerStatus& status,double now=10.) {
    return combat::NativeGrabFeedback(status,world,9,source,2,7,9.8,9.9,now);
  };
  Check(feedback(p)==F::Pending,"unprocessed native request waits briefly for actual acceptance");
  p.grab_enemy=source; p.grab_generation=2; p.grab_kind=25;
  Check(feedback(p)==F::Accepted,"matching native holder and ownership generation confirm the claim");
  auto changed=p; changed.grab_generation=3;
  Check(feedback(changed)==F::Release,"a different ownership generation cannot confirm a grab");
  changed=p; changed.world.serial++;
  Check(feedback(changed)==F::Release,"a different world cannot confirm a grab");
  changed=p; changed.incarnation++;
  Check(feedback(changed)==F::Release,"a replacement life cannot inherit a native grab");
  changed=p; changed.grab_enemy.source++;
  Check(feedback(changed)==F::Release,"another native holder does not acknowledge this source");
  Check(feedback(p,11.1)==F::Release,"stale confirmation cannot keep a native attachment alive");
  p.grab_enemy={}; p.grab_kind=0; p.grab_generation=0; p.last_damage=7;
  Check(feedback(p)==F::Release,"processed first damage without a native holder releases the host claim");
  p.last_damage=0;
  Check(combat::NativeGrabFeedback(p,world,9,source,2,7,8.8,9.9,10)==F::Release,
      "an unacknowledged request has a bounded lifetime");
  p.grab_ready=false;
  Check(feedback(p)==F::Release,"native unavailability ends the offer");
  p.grab_ready=true; p.peer_grab_incarnation=9; p.peer_grab_number=6;
  Check(!combat::NativeGrabOwnerReleased(p,world,9,source,2,7),"old owner release cannot end a later native episode");
  p.peer_grab_number=7;
  Check(combat::NativeGrabOwnerReleased(p,world,9,source,2,7),"owner release ends the exact incoming episode");
  p.peer_grab_enemy=source; p.peer_grab_generation=2;
  Check(!combat::NativeGrabOwnerReleased(p,world,9,source,2,7),"live owner claim is retained");
  p.peer_grab_generation=3;
  Check(combat::NativeGrabOwnerReleased(p,world,9,source,2,7),"owner generation replacement ends the old claim");
  p.peer_grab_incarnation=8;
  Check(!combat::NativeGrabOwnerReleased(p,world,9,source,2,7),"release from a prior local life is ignored");
}
void CentipedeContactSound() {
  using F=EnemyFamily;
  for (const uint32_t caller:{0x825BCE30u,0x825BD000u,0x825BD358u})
    Check(combat::CentipedeContactSound(F::Centipede,caller,8),"leap sound8 directly before a contact builder admitted");
  for (const uint32_t caller:{0x825BBE78u,0x825BC0B8u,0x825BC0DCu,0x825BE0A0u})
    Check(!combat::CentipedeContactSound(F::Centipede,caller,8),"other centipede cues have no contact after them");
  Check(!combat::CentipedeContactSound(F::Centipede,0x825BCE30,5),"other resources excluded");
  Check(!combat::CentipedeContactSound(F::Scorpion,0x825BCE30,8),"other family cannot use a centipede cue");
  DamageEvent d{{303,4,5},9,7,{123,4,F::Centipede},0x1001,10,{1,0,0}};
  d.attack_sound=true; d.generation=2; d.time=10;
  Check(ValidDamageEvent(d) && combat::NativeAttackSoundResource(d)==8,"actual leap contact4097/10 can carry native resource8");
  d.magnitude=5; Check(combat::NativeAttackSound(d),"leap contact4097/5 is a native cue pair");
  d.flags=3; d.magnitude=10; Check(combat::NativeAttackSound(d),"leap contact3/10 is a native cue pair");
  d.flags=0x1001; d.magnitude=6; Check(!combat::NativeAttackSound(d) && !ValidDamageEvent(d),"other centipede pairs cannot carry a cue");
  d.magnitude=10; d.projectile=true; d.surface=0;
  Check(!combat::NativeAttackSound(d),"a projectile impact is not the leap contact");
  d.projectile=false; d.surface=UINT32_MAX;
  Check(combat::AttackSoundObserved(d,d.enemy,2,9.95,10,false),"recent leap cue matches exact attack source");
  Check(!combat::AttackSoundObserved(d,d.enemy,2,9.8,10,false),"a later contact cannot borrow an old cue");
  Message m; m.type=MessageType::DamageEvent; m.epoch=9; m.damage=d;
  std::vector<uint8_t> wire; Message decoded;
  Check(Encode(m,wire) && Decode(wire,decoded) && decoded.damage.attack_sound &&
      combat::NativeAttackSoundResource(decoded.damage)==8,"leap cue survives the real damage codec with its hit");
}
void AfterContactSound() {
  using F=EnemyFamily;
  DamageEvent crab{{101,1,5},9,7,{55,2,F::Crab},0x1000,5,{1,0,0}};
  crab.generation=3; crab.time=10; crab.part=64;
  Check(combat::CueAfterContact(F::Crab) && combat::CueAfterContact(F::SwampCrawlerSmall) &&
      !combat::CueAfterContact(F::Scorpion) && !combat::CueAfterContact(F::Centipede),"only cues after contact defer the hit");
  Check(combat::AfterContactSound(crab,crab.enemy,3,0x8265CA8C,8,10.05),"crab fight cue right after its contact annotates it");
  Check(!combat::AfterContactSound(crab,crab.enemy,3,0x8265CBB0,8,10.05),"other crab sound sites cannot annotate");
  Check(!combat::AfterContactSound(crab,crab.enemy,3,0x8265CA8C,9,10.05),"other crab resources excluded");
  Check(!combat::AfterContactSound(crab,crab.enemy,4,0x8265CA8C,8,10.05),"cue cannot cross an authority generation");
  Check(!combat::AfterContactSound(crab,crab.enemy,3,0x8265CA8C,8,10.2),"a late cue cannot annotate an older hit");
  auto other=crab; other.enemy.source++;
  Check(!combat::AfterContactSound(other,crab.enemy,3,0x8265CA8C,8,10.05),"cue belongs to its exact source");
  other=crab; other.magnitude=10;
  Check(!combat::AfterContactSound(other,other.enemy,3,0x8265CA8C,8,10.05),"other crab hit pairs cannot carry a cue");
  other=crab; other.attack_sound=true;
  Check(!combat::AfterContactSound(other,other.enemy,3,0x8265CA8C,8,10.05),"an annotated hit is not annotated twice");
  DamageEvent small{{702,1,5},9,8,{77,1,F::SwampCrawlerSmall},0x1000,6,{0,1,0}};
  small.generation=1; small.time=20;
  for (const uint32_t caller:{0x825FBC50u,0x825FBD24u})
    Check(combat::AfterContactSound(small,small.enemy,1,caller,7,20.01),"small crawler cue right after its contact annotates it");
  Check(!combat::AfterContactSound(small,small.enemy,1,0x8265CA8C,8,20.01),"a crab site cannot annotate a crawler hit");
  small.attack_sound=true;
  Check(ValidDamageEvent(small) && combat::NativeAttackSoundResource(small)==7,"crawler hit carries native resource7");
  crab.attack_sound=true;
  Check(ValidDamageEvent(crab) && combat::NativeAttackSoundResource(crab)==8,"crab hit carries native resource8");
  Message m; m.type=MessageType::DamageEvent; m.epoch=9; m.damage=crab;
  std::vector<uint8_t> wire; Message decoded;
  Check(Encode(m,wire) && Decode(wire,decoded) && decoded.damage.attack_sound &&
      combat::NativeAttackSoundResource(decoded.damage)==8,"crab cue survives the real damage codec");
}
void BodyHitClass() {
  using B=combat::BodyHit;
  Check(combat::ClassifyBodyHit(true,true,true)==B::Relayed,"audited owned enemy hit is relayed");
  Check(combat::ClassifyBodyHit(true,true,false)==B::EnemyHere,"unaudited producer of an owned enemy is counted, not relayed");
  Check(combat::ClassifyBodyHit(true,false,false)==B::EnemyRemote,"replica source is counted, not relayed");
  Check(combat::ClassifyBodyHit(false,false,false)==B::World,"world hazard on the body stays in this world");
}
void Profiles() {
  // 40 seeds per profile, 200 real core scenarios. Virtual time, real loopback
  // sockets, real framing/ACKs/nonces, independent peer clocks, no game process.
  for(const auto name:{"lan","home","bad_wifi","mobile","hell"})
    for(uint32_t n=1;n<=40;++n) ProfileScenario(name,n);
}
void NativeStopScope() {
  StoryEvent stopped{2,203,1,123,456,3,7,StoryPhase::Stopped};
  const auto check=[&](const StoryEvent& e) {return story::NativeStopMatches(e,2,203,123,456,3,7,0x8250B9D8);};
  Check(check(stopped),"same native stopped wait is eligible");
  auto old=stopped; --old.generation;
  Check(!check(old),"old world cannot retire a local live script");
  old=stopped; ++old.run; Check(!check(old),"another activation cannot retire this wait");
  old=stopped; ++old.record; Check(!check(old),"retirement cannot skip callbacks");
  Check(!story::NativeStopMatches(stopped,2,203,123,456,3,7,0x8250C2C8),"NPC orders retain their native side effects");
}
void Outages() {
  for(unsigned role:{1u,2u}) {
    Pair p; p.Connect(); UdpSimulation cut; cut.one_way_roles=role; cut.one_way_seconds=5;
    (role==1 ? p.a : p.b)->SetSimulation(cut); p.Pump(300);
    Check(p.host.State()!=SessionState::Ended && p.guest.State()!=SessionState::Ended,"short one-way does not end session");
    p.Pump(500); Check(p.Connected(),"each one-way direction reconnects after automatic heal");
  }
  Pair short_cut; short_cut.Connect(); UdpSimulation cut; cut.disconnect_seconds=5;
  short_cut.a->SetSimulation(cut); short_cut.b->SetSimulation(cut); short_cut.Pump(400);
  Check(short_cut.host.State()==SessionState::Lost && short_cut.guest.State()==SessionState::Lost,"full short outage reaches Lost");
  short_cut.Pump(500); Check(short_cut.Connected(),"full short disconnect reconnects");
  Pair long_cut; long_cut.Connect(); cut.disconnect_seconds=25;
  long_cut.a->SetSimulation(cut); long_cut.b->SetSimulation(cut); long_cut.Pump(2300);
  Check(long_cut.host.State()==SessionState::Ended && long_cut.guest.State()==SessionState::Ended,"long disconnect cleanly ends both sessions");
  Check(long_cut.a->SimulationStats().dropped && long_cut.b->SimulationStats().dropped,"both disconnect directions exercised");
}
}
int main(int argc,char** argv) {
  if(argc!=2) { std::fprintf(stderr,"choose one named chaos test\n"); return 2; }
  test_name=argv[1]; const std::string_view name(test_name);
  if(name=="model") Model();
  else if(name=="profiles") Profiles();
  else if(name=="outages") Outages();
  else if(name=="ownership_generation") OwnershipGeneration();
  else if(name=="story_duplicate") StoryDuplicate();
  else if(name=="story_world") StoryWorld();
  else if(name=="story_native_stop") NativeStopScope();
  else if(name=="digest_compare") DigestCompare();
  else if(name=="false_quiet") FalseQuiet();
  else if(name=="shot_validator") ShotValidation();
  else if(name=="session_epoch") SessionEpoch();
  else if(name=="projectile_source") ProjectileSource();
  else if(name=="impact_selector") ImpactSelector();
  else if(name=="grab_feedback") GrabFeedback();
  else if(name=="attack_sound") AttackSound();
  else if(name=="grab_start") GrabStart();
  else if(name=="native_script_target") NativeScriptTarget();
  else if(name=="bat_contact_sound") BatContactSound();
  else if(name=="raptor_camera") RaptorCamera();
  else if(name=="raptor_bite_sound") RaptorBiteSound();
  else if(name=="centipede_contact_sound") CentipedeContactSound();
  else if(name=="body_hit_class") BodyHitClass();
  else if(name=="after_contact_sound") AfterContactSound();
  else if(name=="udp_duplicate" || name=="udp_order") Reliable();
  else { std::fprintf(stderr,"unknown test\n"); return 2; }
  std::printf("PASS [%s]\n",test_name); return 0;
}
