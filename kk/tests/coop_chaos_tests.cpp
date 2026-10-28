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
#include "coop_event_budget.h"
#include "coop_puppet.h"
#include "coop_rebirth.h"
#include "coop_revive.h"
#include "coop_support.h"
#include "coop_partner_selection.h"
#include <algorithm>
#include <map>
#include <vector>
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
// C101: native event pool model (8271E4E8 allocate, 8271E600 age, 8271E450
// visit) and the bat vision list of 8262FE20, to replay the 26 Oct 21:03 crash:
// a co-op publisher kept making 0.001 s type 1 events while the game's pause
// stopped all ageing; after Resume a bat appended them all to a list of 74.
namespace eb=kk::coop::event_budget;
struct PoolModel {
  struct Entry { uint32_t flags=4, owner=0, generation=0; float time=0; };
  std::vector<Entry> entries; std::vector<uint32_t> free_list;
  uint32_t capacity=512, generation=0; bool paused=false;
  bool Allocate(uint32_t owner,float time,uint32_t& index) {
    if (free_list.empty() && entries.size()==capacity) return false;
    if (!free_list.empty()) { index=free_list.back(); free_list.pop_back(); }
    else { index=uint32_t(entries.size()); entries.push_back({}); }
    entries[index]={1,owner,++generation,time}; return true;
  }
  void Age(float dt) {
    if (paused) return;
    for (uint32_t i=0;i<entries.size();++i) {
      auto& e=entries[i]; if (e.flags&4) continue;
      if ((e.flags&1) && !(e.flags&2)) { e.flags&=~1u; continue; }
      e.time-=dt; if (e.time<=0) e.flags|=2;
      if (e.flags&2) { e.generation=0; e.flags|=4; free_list.push_back(i); }
    }
  }
  // Guest memory image read through the shipping reader (coop_stimulus_debug.h).
  eb::BucketCount Count(uint32_t owner) const {
    constexpr uint32_t base=0x50000000;
    std::map<uint32_t,uint32_t> m;
    const uint32_t table=kk::coop::stimulus_debug::kPools+16;
    m[table]=base; m[table+8]=uint32_t(entries.size()); m[table+12]=capacity;
    for (uint32_t i=0;i<entries.size();++i) {
      const auto a=base+i*kk::coop::stimulus_debug::kStride;
      m[a]=entries[i].flags; m[a+8]=entries[i].owner; m[a+112]=entries[i].generation;
    }
    return eb::Count([&](uint32_t p) { const auto it=m.find(p); return it==m.end() ? 0u : it->second; },1,owner);
  }
  // 8262FE20 appends each visible event at 2492+8n with no bound (n from 2484).
  uint32_t BatScan(std::vector<uint32_t>& vars) const {
    uint32_t n=0;
    for (uint32_t i=0;i<entries.size();++i) {
      if (entries[i].flags&5) continue;
      const auto at=(eb::kVisionListFirst+8*n)/4;
      if (at+1<vars.size()) { vars[at]=(1u<<24)|i; vars[at+1]=entries[i].generation; }
      ++n;
    }
    return n;
  }
};
struct PauseRun {
  uint32_t publishes=0, paused_publishes=0, undispatched_publishes=0, max_visible=0, max_owner=0, scans=0, skipped=0;
  bool player_intact=true, overflow=false;
};
enum class Phase { Play, Pause, PauseExempt, Frozen, ZeroDt };
constexpr uint32_t kBody=0x43566000, kPlayer=0x82001234;
// legacy=true is the 26a publisher: one event every frame, no gates.
PauseRun RunPhases(std::initializer_list<std::pair<Phase,unsigned>> phases,bool legacy,bool guard=false) {
  PoolModel pool; PauseRun r;
  std::vector<uint32_t> vars(8800/4,0);
  for (const auto& [phase,frames]:phases) for (unsigned f=0;f<frames;++f) {
    pool.paused=phase==Phase::Pause || phase==Phase::PauseExempt;
    const bool body_runs=phase==Phase::Play || phase==Phase::PauseExempt || phase==Phase::ZeroDt;
    const bool bat_runs=phase==Phase::Play || phase==Phase::ZeroDt;
    const float dt=phase==Phase::ZeroDt ? 0.f : 1.f/120;
    if (bat_runs) {
      // The bat's reflex runs inside the AI pass, before this frame's ageing.
      vars[3108/4]=kPlayer;
      const auto visible=pool.Count(0).enumerable;
      r.max_visible=std::max(r.max_visible,visible); ++r.scans;
      if (guard && !eb::VisionScanSafe(visible)) ++r.skipped;
      else { const auto n=pool.BatScan(vars); r.overflow|=n>eb::kVisionListRoom; }
      r.player_intact&=vars[3108/4]==kPlayer;
    }
    // The co-op stand-in publisher runs after the AI pass on every frame.
    const auto owned=pool.Count(kBody).owner_live;
    r.max_owner=std::max(r.max_owner,owned);
    const bool publish=legacy || eb::DecidePublish({pool.paused,body_runs,owned})==eb::Decision::Publish;
    uint32_t index=0;
    if (publish && pool.Allocate(kBody,.001f,index)) {
      ++r.publishes; if (pool.paused) ++r.paused_publishes; if (!body_runs) ++r.undispatched_publishes;
    }
    pool.Age(dt);
  }
  return r;
}
// 26 Oct timeline: play, 3.79 s pause page, Resume.
constexpr unsigned kPauseFrames=455;
void EventPause() {
  const auto legacy=RunPhases({{Phase::Play,600},{Phase::Pause,kPauseFrames},{Phase::Play,120}},true);
  Check(legacy.overflow && !legacy.player_intact && legacy.max_visible>77,
      "model reproduces the 21:03 overflow: the 26a publisher floods the bat list past the player pointer");
  const auto fixed=RunPhases({{Phase::Play,600},{Phase::Pause,kPauseFrames},{Phase::Play,120}},false);
  Check(fixed.publishes>=700,"gated publisher still publishes on every played frame");
  Check(fixed.paused_publishes==0,"nothing is published while the world pause stops ageing");
  Check(!fixed.overflow && fixed.player_intact && fixed.max_visible<=eb::kOwnerLiveLimit,
      "after Resume the bat sees at most the native live count and its player pointer survives");
  // A body exempt from the freeze (flag 08000000) is still dispatched while paused.
  const auto exempt=RunPhases({{Phase::Play,120},{Phase::PauseExempt,kPauseFrames},{Phase::Play,120}},false);
  Check(exempt.paused_publishes==0,"a dispatched body publishes nothing while the pool is paused");
  std::printf("event_pause legacy visible=%u fixed publishes=%u visible=%u exempt paused-publishes=%u\n",
      legacy.max_visible,fixed.publishes,fixed.max_visible,exempt.paused_publishes);
}
void EventDispatch() {
  // A scripted freeze with ageing still on: the native publisher would not run.
  const auto r=RunPhases({{Phase::Play,120},{Phase::Frozen,240},{Phase::Play,120}},false);
  Check(r.undispatched_publishes==0,"no stand-in publish on frames the scheduler did not dispatch the body");
  Check(r.publishes>=230,"dispatched frames still publish");
}
void EventOwnerCap() {
  // Time scale zero: ageing runs but nothing expires, body and bats dispatched.
  const auto r=RunPhases({{Phase::Play,60},{Phase::ZeroDt,300},{Phase::Play,60}},false);
  Check(r.max_owner<=eb::kOwnerLiveLimit,"live partner events never exceed the native count");
  Check(!r.overflow && r.player_intact,"bat list stays inside its room with zero frame time");
}
void EventVisionGuard() {
  Check(eb::VisionScanSafe(eb::kVisionListRoom) && !eb::VisionScanSafe(eb::kVisionListRoom+1),
      "vision guard boundary is the list room of 74");
  // Second line: the 26a publisher with the guard alone keeps the player pointer.
  const auto r=RunPhases({{Phase::Play,120},{Phase::Pause,kPauseFrames},{Phase::Play,60}},true,true);
  Check(r.skipped>0 && !r.overflow && r.player_intact,"guard skips an overflowing scan and the player pointer survives");
}
void SupportMember() {
  using kk::coop::objects::ClassifySupportMember; using kk::coop::objects::SupportMember;
  // C105: a personal pickup or human member keeps the whole support root out of sync.
  Check(ClassifySupportMember(true,true,false)==SupportMember::Refuse,"personal or human member refuses the root");
  Check(ClassifySupportMember(true,false,false)==SupportMember::Refuse,"personal member without physics still refuses");
  Check(ClassifySupportMember(false,true,true)==SupportMember::Drive,"rigid unit body follows host poses");
  Check(ClassifySupportMember(false,false,false)==SupportMember::Local,"member without physics keeps native motion");
  Check(ClassifySupportMember(false,true,false)==SupportMember::Local,"scaled or disabled body keeps native motion");
}
void SupportDelayedWatch() {
  using namespace kk::coop::objects;
  // C105: the guest may only run the ODE delayed watch for the native show/hide
  // and activity trigger actions; any other authored callback keeps the root native.
  Check(SupportDelayedWatchAllowed(0),"a root without a delayed watch stays eligible");
  Check(SupportDelayedWatchAllowed(kTrigSetActiveAndVisible) && SupportDelayedWatchAllowed(kTrigSetActivity) &&
      SupportDelayedWatchAllowed(kTrigActifVisibleMultiple),
      "show, hide and activity trigger actions are allowed");
  Check(!SupportDelayedWatchAllowed(0x82538AF0),"object reinit callback keeps the root native");
  Check(!SupportDelayedWatchAllowed(0x8250B9A0),"NPC order callback keeps the root native");
}
void EventNetworkDefer() {
  // Peer shots and relayed damage arriving during a pause wait in their queue.
  PoolModel pool; pool.paused=true; unsigned queued=0,published=0;
  const auto drain=[&] {
    if (eb::DeferWhilePaused(pool.paused)) return;
    uint32_t i=0; while (queued && pool.Allocate(kBody,.0001f,i)) { --queued; ++published; }
  };
  for (unsigned f=0;f<kPauseFrames;++f) { ++queued; drain(); }  // worst case: one peer shot per frame
  Check(published==0,"network events wait while the pool does not age");
  pool.paused=false; drain();
  Check(published>0,"deferred network events run after Resume");
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
void DigestRetired() {
  // C108 201/3: host retired centipede 62D3D063/0 at 08:13:02; the guest applied the
  // retirement but its actor stayed bound with life 10, and the old compare called
  // that structural drift (guest move, then a private reload 20 s later).
  Check(!desync::GuestOnlyAliveIsDrift(false,true,true),"a locally retired replica is never structural drift");
  Check(desync::GuestOnlyAliveIsDrift(false,false,true),"a living unretired replica the host lacks is drift");
  Check(!desync::GuestOnlyAliveIsDrift(true,false,true),"an enemy the host lists is not guest only");
  Check(!desync::GuestOnlyAliveIsDrift(false,false,false),"a dead replica is not drift");
}
void UnarmedGait() {
  // An armed crew donor loads gun walks beside its empty hand walk.
  const puppet::Gait crew[]={{{150,0,2.2f},0,false},{{165,0,2.4f},0,false},{{11,0,1.2f},0,false}};
  for(const float v:{.6f,1.2f,2.2f,2.4f,4.f}) {
    const auto g=puppet::ChooseGait(v,0,0,false,crew,UINT32_MAX,true);
    Check(g && !puppet::HeldItemAction(g->clip.action),"empty hands never walk with a gun grip or raised arms");
  }
}
void RebirthAdoption() {
  // C107: host private reload at level 101/4. Six living crabs of one spawner
  // template (sequences 1 to 6; 3 is a corpse) and one of another template.
  using F=EnemyFamily;
  const uint64_t spawner=0x9B70C2F92FA57564ull, crab_template=0x1111ull, other_template=0x2222ull;
  enemies::RebirthAdoption adoption;
  for(uint32_t n=1;n<=6;++n) {
    EnemyState e; e.id=enemies::SpawnId(spawner,crab_template,n,F::Crab); e.life=n==3 ? 0.f:12.f;
    e.spawn_x=float(n); adoption.Remember(e);
  }
  EnemyState other; other.id=enemies::SpawnId(spawner,other_template,7,F::Crab); other.life=5; adoption.Remember(other);
  Check(adoption.Pending()==6,"only living spawned identities are remembered");
  // The reloaded spawner's native births would be sequences 8, 9, ...
  std::vector<uint32_t> taken;
  for(uint32_t next=8;next<14;++next) {
    const auto a=adoption.Take(enemies::SpawnId(spawner,crab_template,next,F::Crab));
    if(!a) break;
    Check(a->id.source==enemies::SpawnId(spawner,crab_template,1,F::Crab).source && a->id.family==F::Crab,
        "a rebirth adopts only its own spawner template");
    taken.push_back(a->id.sequence);
  }
  Check((taken==std::vector<uint32_t>{1,2,4,5,6}),"each living identity is adopted once, in sequence order, corpses skipped");
  Check(!adoption.Take(enemies::SpawnId(spawner,crab_template,9,F::Crab)),"extra rebirths keep fresh sequences");
  Check(!adoption.Take(enemies::SpawnId(spawner,crab_template,1,F::Raptor)),"another family never adopts");
  Check(!adoption.Take(enemies::SpawnId(spawner+1,crab_template,1,F::Crab)),"another spawner never adopts");
  const auto o=adoption.Take(enemies::SpawnId(spawner,other_template,8,F::Crab));
  Check(o && o->id==other.id && !adoption.Pending(),"the other template adopts its own identity");
  adoption.Remember(other); adoption.Clear();
  Check(!adoption.Pending(),"a new world forgets every identity");
}
void ArmedCopySource() {
  // Twin armed_copy_source keeps unarmed Carl as the copy source.
  puppet::AutoCandidate carl; carl.actor=110; carl.natural_h=true; carl.rig_valid=true; carl.stable=true;
  carl.script_evidence_complete=true; carl.script_reference=true; carl.animation_support=4;
  auto crew=carl; crew.actor=111; crew.animation_support=5;
  const puppet::AutoCandidate sources[]{carl,crew};
  const uint8_t copyable[]{1,1};
  Check(puppet::PreferArmedCopySource(sources,0,copyable)==1,"partner copy uses a body that can show the gun");
}
void InvisibleGun() {
  // Twin invisible_gun lets a body without gun geometry (V-Rex slot 8) aim an unseen gun.
  Check(puppet::BodyGun(0,2)==0 && puppet::BodyGun(1u<<2,2)==2,"gun clips only on a body that draws the gun");
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
// C104: revive placement and the void safety net.
revive::Probe GoodSpot(float z) { revive::Probe p; p.floor=true; p.floor_z=z; p.normal_z=1; p.headroom=true; p.sight=true; p.nav=2; return p; }
void PlacementFloor() {
  using revive::Verdict;
  Check(revive::Judge(GoodSpot(5.2f),5,2)==Verdict::Ok,"flat floor within a step by the partner is taken");
  auto p=GoodSpot(5); p.floor=false;
  Check(revive::Judge(p,5,2)==Verdict::NoFloor,"a spot with no native floor (void) is refused");
  Check(revive::Judge(GoodSpot(3.5f),5,2)==Verdict::Step,"a floor 1.5 under the partner (ledge) is refused");
  p=GoodSpot(5); p.normal_z=.5f; Check(revive::Judge(p,5,2)==Verdict::Steep,"a steep face is refused");
  p=GoodSpot(5); p.headroom=false; Check(revive::Judge(p,5,2)==Verdict::Head,"a spot inside rock is refused");
  p=GoodSpot(5); p.nav=int8_t(0x42); Check(revive::Judge(p,5,2)==Verdict::Nav,"blocked nav bits refuse");
  Check(revive::Judge(p,5,int8_t(0x40))==Verdict::Ok,"a mark the partner stands on too is taken (302 start cell 0x40)");
  p.nav=int8_t(0x82); Check(revive::Judge(p,5,int8_t(0x40))==Verdict::Nav,"a mark the partner does not stand on refuses");
  p=GoodSpot(5); p.nav=0; Check(revive::Judge(p,5,2)==Verdict::Nav && revive::Judge(p,5,0)==Verdict::Ok,"off grid cell only where the partner is off grid too");
  p=GoodSpot(5); p.nav=3; Check(revive::Judge(p,5,2)==Verdict::Nav && revive::Judge(p,5,3)==Verdict::Ok,"water kind only where the partner stands on it");
  p=GoodSpot(5); p.nav=1; Check(revive::Judge(p,5,2)==Verdict::Nav && revive::Judge(p,5,1)==Verdict::Ok,"native blocked cell 1 is refused unless the partner stands on it");
  PlayerPose peer; peer.x=10; peer.y=20; peer.z=7; peer.facing=0;
  const auto c=revive::Candidate(peer,6,0);
  Check(std::abs(c.x-10)<.001f && std::abs(c.y-21.5f)<.001f && c.z==6,"first candidate is the partner's left side, on the partner floor");
  const auto behind=revive::Candidate(peer,6,4);
  Check(std::abs(behind.x-8.5f)<.001f && std::abs(behind.y-20)<.001f,"the old spot behind is still a candidate");
  Check(std::abs(revive::Candidate(peer,6,8).y-21)<.001f,"second ring is closer");
}
// C111 (runs c111-beach-b/c, 101/4): the partner faces the crabs it fights; a
// revived player 1.5 straight behind was shielded by the partner body and the
// crabs stalled out of strike range. The first spots must leave the line from
// the partner's front to the revived player clear of the partner.
void PlacementSide() {
  PlayerPose peer; peer.x=0; peer.y=0; peer.z=0; peer.facing=0;
  for (unsigned i=0;i<2;++i) {
    const auto s=revive::Candidate(peer,0,i);
    // An enemy 6 ahead of the partner, heading for the revived player.
    const float ex=6-s.x, ey=0-s.y, len=std::hypot(ex,ey);
    // Closest approach of that line (spot to enemy) to the partner at the origin.
    const float t=std::clamp(-(s.x*ex+s.y*ey)/(len*len),0.f,1.f);
    const float gap=std::hypot(s.x+t*ex,s.y+t*ey);
    Check(gap>1.f,"an early spot is not shielded by the partner from enemies in front");
  }
}
void PlacementSight() {
  auto p=GoodSpot(5); p.sight=false;
  Check(revive::Judge(p,5,2)==revive::Verdict::Sight,"a wall between the spot and the partner refuses the spot");
}
void PlacementWait() {
  revive::Placement w;
  Check(w.Step(false,10)==revive::Place::Wait && w.Step(false,10.9)==revive::Place::Wait &&
      w.Step(false,11.8)==revive::Place::Wait,"no spot yet keeps waiting");
  Check(w.Step(false,12)==revive::Place::Stay,"two seconds without a spot keeps the native respawn spot");
  Check(w.Step(false,20)==revive::Place::Wait,"a new attempt starts a new wait");
  Check(w.Step(true,20.1)==revive::Place::Spot && w.Step(false,30)==revive::Place::Wait,"a found spot ends the attempt");
  Check(w.Step(false,std::numeric_limits<double>::quiet_NaN())==revive::Place::Wait,"NaN clock waits");
}
void VoidGuard() {
  revive::VoidGuard g; const ShotVector home{1,2,3};
  g.Arm(home,true,10);
  Check(!g.Check(false,10.5) && g.Armed(),"no void, no rescue");
  const auto r=g.Check(true,11);
  Check(r && r->x==1 && r->z==3 && !g.Armed(),"void after a placement puts Jack back once on the old ground");
  g.Arm(home,true,10); Check(!g.Check(true,13.5) && !g.Armed(),"guard ends after three seconds");
  g.Arm(home,false,10); Check(!g.Check(true,10.2),"an airborne old spot is never a rescue target");
}
void Profiles() {
  // 40 seeds per profile, 200 real core scenarios. Virtual time, real loopback
  // sockets, real framing/ACKs/nonces, independent peer clocks, no game process.
  for(const auto name:{"lan","home","bad_wifi","mobile","hell"})
    for(uint32_t n=1;n<=40;++n) ProfileScenario(name,n);
}
// C114, 303 checkpoint 5: the host arms an NPC order detector (run 2) and the
// native program is cancelled before its start. The guest detector was
// cancelled before the hint arrived, so the guest stays on run 1. The ordered
// host stream crosses a seeded bad_wifi style link (35 +/- 25 ms, 2% loss
// resent after 200 ms, reordering). Then the host arms again (run 3), either
// natively or for a guest request, and that run really starts. The guest
// detector is scheduled again before that new hint arrives, so it must not
// follow the stalled run 2 hint.
struct CancelRun {
  unsigned moves=0, reloads=0;
  uint32_t guest_run=0, guest_started=0, host_active=0, guest_active=0, phase_a_host=0, phase_a_guest=0;
  bool requested=false, granted=false;
};
CancelRun CancelledRun(uint32_t seed,bool rearm_by_request,bool started_before) {
  uint32_t rng=seed*2654435761u+1;
  const auto next=[&] { rng=rng*1664525u+1013904223u; return rng>>8; };
  struct Wire { double at; StoryEvent e; };
  std::vector<Wire> wire;
  uint32_t number=0;
  story::Progress host; host.Baseline(0);
  const auto emit=[&](double t,StoryPhase phase,uint32_t record) {
    StoryEvent e{1,303,++number,0x2EC85D5319B53E6Eull,0x11A2EDA4476DE437ull,host.run,record,phase};
    double delay=.035+(double(next()%5001)/5000.0-.5)*.05;
    if (next()%100<2) delay+=.2;
    wire.push_back({t+delay,e});
  };
  story::Ordered q; q.Reset(1,303);
  story::ActivationGate gate;
  story::Progress guest; guest.Baseline(0);
  story::Lag lag;
  CancelRun out;
  bool guest_detect=false; double guest_start_at=-1;
  bool host_armed=false; double host_start_at=-1, host_checksum=3, request_at=-1;
  // Phase A: host arms run 2 at 1.0 s, cancelled with no start (or, for the
  // control, really started and the guest missed it).
  for (double t=0;t<150;t+=.016) {
    if (t>=1 && !host_armed) {
      host_armed=true; host.Arm(2); emit(t,StoryPhase::Armed,kStoryRecords);
      if (started_before) { host.started=2; emit(t+.1,StoryPhase::Started,kStoryRecords); }
    }
    // Phase B at 70 s: the detector is scheduled again on both peers.
    if (t>=70 && !guest_detect) {
      guest_detect=true;
      if (rearm_by_request) { out.requested=true; request_at=t+.04+double(next()%50)/1000.0; }
      else { host.Arm(host.run+1); emit(t,StoryPhase::Armed,kStoryRecords); host_start_at=t+.1; }
    }
    if (request_at>0 && t>=request_at) {
      request_at=-1;
      // The guest asked for its own next run (local+1).
      if (host.Requestable(guest.run+1)) {
        out.granted=true; host.Arm(host.run+1); emit(t,StoryPhase::Armed,kStoryRecords); host_start_at=t+.1;
      }
    }
    if (host_start_at>0 && t>=host_start_at) {
      host_start_at=-1; host.started=host.run; emit(t,StoryPhase::Started,kStoryRecords);
      for (uint32_t i=0;i<3;++i) emit(t+.01*i,StoryPhase::Advance,i);
      host.record=3;
    }
    if (t>=host_checksum) { host_checksum+=3; emit(t,StoryPhase::Checksum,host.record); }
    for (auto it=wire.begin();it!=wire.end();)
      if (it->at<=t) { q.Push(it->e,t); it=wire.erase(it); } else ++it;
    while (const auto* front=q.Front()) {
      const auto e=*front; q.Applied(t);
      gate.Observe(e,t);
      if (e.phase!=StoryPhase::Checksum) continue;
      auto stage=lag.Sample({gate.HostActive(e.run),e.record},guest,t);
      if (stage==story::Lag::Stage::Move) { ++out.moves; lag.Moved(t); }
      if (stage==story::Lag::Stage::Reload) { ++out.reloads; lag={}; }
    }
    // The guest's own detector only runs again in phase B; it follows the host.
    if (guest_detect && guest_start_at<0 && gate.CanArm(guest.run,t)) {
      guest.Arm(gate.armed); guest_start_at=t+.1;
    }
    if (guest_start_at>0 && t>=guest_start_at && !(guest.started>=guest.run)) {
      guest.started=guest.run; guest.record=3;
    }
    if (t>=60 && t<60.02) { out.phase_a_host=host.Active(); out.phase_a_guest=guest.Active(); }
  }
  Check(q.Error().reason==story::Ordered::Failure::None && !q.Behind(),"ordered stream survives the bad link");
  out.guest_run=guest.run; out.guest_started=guest.started;
  out.host_active=host.Active(); out.guest_active=guest.Active();
  return out;
}
void CancelledActivation() {
  for (uint32_t seed=1;seed<=24;++seed) {
    for (bool request : {false,true}) {
      const auto r=CancelledRun(seed,request,false);
      Check(r.phase_a_host==1 && r.phase_a_guest==1,"cancelled host run is not an executed activation on either peer");
      Check(r.moves==0 && r.reloads==0,"cancelled host run causes no soft move and no catch up reload");
      if (request) Check(r.requested && r.granted,"guest request after a cancelled host run is granted");
      Check(r.guest_run==3 && r.guest_started==3,"later host arm (run 3) is followed and started by the guest");
      Check(r.host_active==3 && r.guest_active==3,"world comparison agrees after the later start");
    }
    // Control: a host run that really started and the guest missed is still lag.
    const auto miss=CancelledRun(seed,false,true);
    Check(miss.moves>=1,"a missed started host run still offers the soft move");
    Check(miss.guest_run==2,"a started host run is followed, never skipped");
  }
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
  else if(name=="cancelled_activation") CancelledActivation();
  else if(name=="digest_compare") DigestCompare();
  else if(name=="false_quiet") FalseQuiet();
  else if(name=="unarmed_gait") UnarmedGait();
  else if(name=="rebirth_adoption") RebirthAdoption();
  else if(name=="armed_copy_source") ArmedCopySource();
  else if(name=="invisible_gun") InvisibleGun();
  else if(name=="digest_retired") DigestRetired();
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
  else if(name=="placement_floor") PlacementFloor();
  else if(name=="placement_sight") PlacementSight();
  else if(name=="placement_side") PlacementSide();
  else if(name=="placement_wait") PlacementWait();
  else if(name=="void_guard") VoidGuard();
  else if(name=="udp_duplicate" || name=="udp_order") Reliable();
  else if(name=="event_pause") EventPause();
  else if(name=="event_dispatch") EventDispatch();
  else if(name=="event_owner_cap") EventOwnerCap();
  else if(name=="event_vision_guard") EventVisionGuard();
  else if(name=="event_network_defer") EventNetworkDefer();
  else if(name=="support_member") SupportMember();
  else if(name=="support_delayed_watch") SupportDelayedWatch();
  else { std::fprintf(stderr,"unknown test\n"); return 2; }
  std::printf("PASS [%s]\n",test_name); return 0;
}
