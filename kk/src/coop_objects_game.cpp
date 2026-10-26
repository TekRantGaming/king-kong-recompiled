// Native breakable, door, static fire and bounded raft state. Pickup families
// remain independent. All memory and native calls stay on the AI thread.
#include "coop_objects_game.h"
#include "coop_objects.h"
#include "coop_runtime.h"
#include "coop_pose.h"
#include "coop_enemies_game.h"
#include "coop_observer.h"
#include "hfr.h"
#include "settings.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <chrono>
#include <array>
#include <cstdlib>
#include <cstring>
#include <set>
#include <atomic>

REX_EXTERN(__imp__sub_824ADD48);
REX_EXTERN(__imp__sub_82448E08);
REX_EXTERN(__imp__sub_8256C3F8);
REX_EXTERN(__imp__sub_82552200);
REX_EXTERN(__imp__sub_82515CF0);
REX_EXTERN(__imp__sub_82516128);
REX_EXTERN(__imp__sub_8271CCB8);
REX_EXTERN(__imp__sub_8270FA58);
REX_EXTERN(__imp__sub_8270F918);
REX_EXTERN(__imp__sub_8270FCE0);
REX_EXTERN(__imp__sub_8271A6F8);
REX_EXTERN(__imp__sub_82543478);
REX_EXTERN(__imp__sub_82574D80);
REX_EXTERN(__imp__sub_826F9DC0);
REX_EXTERN(__imp__sub_826F9E70);
REX_EXTERN(__imp__sub_82634428);
REX_EXTERN(__imp__sub_825FAF58);
REX_EXTERN(__imp__sub_8260F858);
REX_EXTERN(__imp__sub_8260C988);
REX_EXTERN(__imp__sub_825EF890);
REX_EXTERN(__imp__sub_825E7348);
REX_EXTERN(__imp__sub_827117E0);
REX_EXTERN(__imp__sub_82711328);
#if defined(KK_DEV_TOOLS)
namespace kk {
double DevSecondsSinceMenu();
bool DevTeleportJack(PPCContext&,uint8_t*,uint32_t,coop::ShotVector,float,float);
}
#endif

namespace kk::coop {
namespace {
using namespace hfr;
// 82546870 resolves model 3D0098B3's vars into this cache; 8256C3F8
// uses it for the held-item/lever-mode fields. Use the native door cache
// directly rather than assuming another library's globals alias it.
constexpr uint32_t kDoorGamePtr=0x82CC8F40;
struct Binding : objects::Binding {
  uint64_t hold_tick=0;
  uint64_t damage_tick=0;
  float x=0,y=0,z=0;
  struct Hit { ObjectState raw; double until=0; };
  std::deque<Hit> pending_hits;
  uint32_t local_levers=0;
  uint32_t sent_holds=0,observed_hold=0;
  double next_hold=0;
  std::array<objects::LeverHold,8> local_holds,remote_holds;
  double local_lever_until=0;
  float driven_position=-1;
  ObjectState last;
  bool sent=false,lever_logged=false;
  bool fire_request=false;
  bool fire_path_refused=false;
  double next_ignite=0;
  uint64_t fire_tick=0;
  struct RaftRoute { std::vector<uint32_t> nodes; uint32_t key=0; uint64_t signature=0; } route;
  RaftState raft;
  bool raft_ready=false,raft_driven=false,raft_classified=false;
  double next_route=0;
#if defined(KK_DEV_TOOLS)
  uint64_t raft_tick=0;
  uint32_t init_calls=0,wait_calls=0,loop_calls=0,update_calls=0,hold_calls=0,context_misses=0;
#endif
};
struct State {
  uint64_t epoch=0;
  uint64_t ai_tick=0;
  uint32_t generation=0,level=0,world=0,number=0,request=0;
  bool host=false,ready=false,census_started=false;
  double census_until=0,next_census=0,next_sample=0,next_full=0,next_log=0;
  uint64_t requested=0,accepted=0,applied=0,rejected=0,received=0;
  std::map<uint32_t,Binding> bindings;
  std::map<uint32_t,uint32_t> file_keys;
  std::set<uint32_t> retired;
  std::map<uint64_t,ObjectState> pending;
  std::map<uint64_t,ObjectState> outcomes;
  std::deque<Message> outgoing;
  objects::Replica replica;
  objects::Requests requests;
  objects::Identities identities;
#if defined(KK_DEV_TOOLS)
  struct Plane { float x,y,z,nx,ny,nz; double next=0; };
  std::map<uint64_t,Plane> planes; // Immutable values survive native door retirement.
  struct Pickup { uint32_t actor=0,ai=0,vars=0,matrix=0; uint64_t source=0; bool rack=false; };
  std::map<uint32_t,Pickup> pickups;
  std::set<uint64_t> debug_sources;
  unsigned scan_pass=0,scan_rows=0;
  bool observed_change=false;
  uint64_t raft_fixture=0;
  double raft_withdraw_until=0;
  double scan_next=0;
  std::set<uint32_t> scan_seen;
#endif
} state;
std::atomic<double> use_until{0};
thread_local uint32_t stop_actor=0,stop_ai=0;
struct DoorStopGuard {
  uint32_t actor=stop_actor,ai=stop_ai;
  DoorStopGuard(const Binding& o,const ObjectState& s) {
    stop_actor=(s.value&kDoorScheduled) ? o.actor : 0; stop_ai=stop_actor ? o.ai : 0;
  }
  ~DoorStopGuard() { stop_actor=actor; stop_ai=ai; }
};
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void Store16(uint8_t* b,uint32_t address,uint16_t value) {
  b[address]=uint8_t(value>>8); b[address+1]=uint8_t(value);
}
bool Trace() {
#if defined(KK_DEV_TOOLS)
  static const bool enabled=std::getenv("KK_DEV_OBJECTS")!=nullptr;
  return enabled;
#else
  return false;
#endif
}
uint32_t Ptr(const uint8_t* b,uint32_t p,uint32_t offset,uint32_t bytes) {
  if (!PoseAddress(p,offset+4)) return 0;
  const auto q=Load32(b,p+offset); return PoseAddress(q,bytes) ? q : 0;
}
std::optional<ShotVector> NativePoint(PPCContext& ctx,uint8_t* b,uint32_t actor,
    void (*get)(PPCContext&,uint8_t*)=__imp__sub_8270FCE0) {
  const auto sp=ctx.r1.u32;
  if (!PoseAddress(actor,32) || !Ptr(b,actor,12,64) || (sp&15) || sp<0x40014000 || sp>=0xA0000000) return {};
  uint8_t scratch[16384]; const auto start=sp-sizeof(scratch); const PPCContext saved=ctx;
  std::memcpy(scratch,b+start,sizeof(scratch));
  ctx.r1.u64=start+12288; std::memset(b+start+12288,0,4096); Store32(b,start+12288,sp);
  const auto out=ctx.r1.u32+256;
  ctx.r3.u64=actor; ctx.r4.u64=out; get(ctx,b);
  const ShotVector point{LoadF(b,out),LoadF(b,out+4),LoadF(b,out+8)};
  std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved;
  if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
      std::abs(point.x)>100000 || std::abs(point.y)>100000 || std::abs(point.z)>100000) return {};
  return point;
}
bool Track(const uint8_t* b,uint32_t ai,uint32_t fn) {
  if (!PoseAddress(ai,88)) return false;
  const auto first=Load16(b,ai+80),last=Load16(b,ai+82);
  if (first>last || last>4096 || last-first>64 || !PoseAddress(ai,(last+7)*4)) return false;
  for (uint32_t t=first;t<=last;++t) {
    const auto block=Ptr(b,ai,(t+6)*4,24);
    if (block && Load32(b,block+20)==fn) return true;
  }
  return false;
}
ObjectKind Family(const uint8_t* b,uint32_t ai) {
  if (Track(b,ai,0x82507798) || Track(b,ai,0x82516128) || Track(b,ai,0x82515CF0)) return ObjectKind::Breakable;
  if (Track(b,ai,0x82551ED8) || Track(b,ai,0x82574D80) || Track(b,ai,0x8256C3F8) || Track(b,ai,0x82543478)) return ObjectKind::Door;
  if (Track(b,ai,0x825FAF58) || Track(b,ai,0x8260F858)) return ObjectKind::Fire;
  if (Track(b,ai,0x825FD388) || Track(b,ai,0x825E7348) || Track(b,ai,0x8260C988) || Track(b,ai,0x82604E68)) return ObjectKind::Raft;
  return ObjectKind(0);
}
bool DoorScheduled(const uint8_t* b,uint32_t ai) {
  return Track(b,ai,0x8256C3F8) || Track(b,ai,0x82543478) || Track(b,ai,0x82574D80);
}
uint32_t Vars(const uint8_t* b,uint32_t ai,ObjectKind kind) {
  const uint32_t needed=kind==ObjectKind::Breakable ? 580 : kind==ObjectKind::Fire ? 884 : kind==ObjectKind::Raft ? 1312 : 1340;
  const auto definition=Ptr(b,Ptr(b,ai,0,4),0,8);
  if (!definition || Load32(b,definition+4)<needed || Load32(b,definition+4)>1024*1024) return 0;
  return Ptr(b,ai,64,needed);
}
bool FireContract(const uint8_t* b,uint32_t vars) {
  // Decompiled825FAF58/8260F858: static mode0 paths only. The authored
  // completion callback is deliberately excluded, as are moving/restarted paths.
  // The native effect table has100 entries, with one section per1.4 units.
  const float total=LoadF(b,vars+424),timer=LoadF(b,vars+388);
  return objects::StaticFireParameters(Load32(b,vars+364),Load32(b,vars+372),Load32(b,vars+188),total,timer);
}
bool Components(const uint8_t* b,const Binding& o) {
  return o.actor!=Load32(b,0x82CCBF7C) && Ptr(b,Ptr(b,o.actor,24,8),4,88)==o.ai &&
      Ptr(b,o.actor,12,64)==o.matrix && Vars(b,o.ai,o.kind)==o.vars;
}
bool Valid(const uint8_t* b,const Binding& o) {
  return Components(b,o) && Family(b,o.ai)==o.kind;
}
Binding* Match(uint64_t source,ObjectKind kind) {
  if (!state.identities.Allows(kind,source)) return nullptr;
  return objects::Match(source,kind,state.bindings);
}
Binding* Current(const uint8_t* b,ObjectKind kind) {
  const auto it=state.bindings.find(Load32(b,0x82CCBF6C));
  if (it==state.bindings.end()) return nullptr;
  auto& o=it->second;
  return o.kind==kind && o.ai==Load32(b,0x82CCBF64) && Valid(b,o) && Match(o.source,kind)==&o ? &o : nullptr;
}
bool Shared() {
  if (!REXCVAR_GET(kk_coop_enabled) || !SessionInProcess()) return false;
  const auto v=SessionFlowSnapshot();
  const auto link=EnemyLinkSnapshot();
  return v.active && v.policy==ChapterPolicy::Together && v.phase==FlowPhase::Playing &&
      v.world.generation==state.generation && v.world.level==state.level &&
      link.epoch==state.epoch && link.SessionConnected();
}
void WithdrawRaft(const uint8_t* b,Binding& o);
void Census(const uint8_t* b) {
  const auto w=Load32(b,0x828E8388);
  if (!PoseAddress(w,44)) return;
  const auto begin=Load32(b,w+40),end=Load32(b,w+28);
  if (!PoseAddress(begin) || !PoseAddress(end) || end<begin || (end-begin)%8 || end-begin>8*20000) return;
  std::set<uint32_t> members;
  for (auto p=begin;p<end;p+=8) {
    const auto actor=Load32(b,p);
    if (!PoseAddress(actor,32) || !members.insert(actor).second || actor==Load32(b,0x82CCBF7C)) continue;
#if defined(KK_DEV_TOOLS)
    if (Trace() || std::getenv("KK_DEV_OBJECT_PICKUP")) {
      const auto ai=Ptr(b,Ptr(b,actor,24,8),4,88),matrix=Ptr(b,actor,12,64);
      const bool rack=Track(b,ai,0x8251C540) || Track(b,ai,0x8251BA60) || Track(b,ai,0x8251B800);
      const auto vars=Ptr(b,ai,64,rack ? 256 : 32);
      if (matrix && vars && (rack || Track(b,ai,0x824F35E0) || Track(b,ai,0x824F3820))) {
        const auto source=enemies::AuthoredKey(rack ? 0x5241434b : 0x414d4d4f,LoadF(b,matrix+48),LoadF(b,matrix+52),LoadF(b,matrix+56));
        if (source && !state.pickups.contains(actor) && state.pickups.size()<objects::kCensus)
          state.pickups[actor]={actor,ai,vars,matrix,source,rack};
      }
    }
#endif
    const auto it=state.bindings.find(actor);
    if (it!=state.bindings.end()) {
      if (!Valid(b,it->second)) { WithdrawRaft(b,it->second); state.retired.insert(actor); state.bindings.erase(it); }
      continue; // Component reuse never acquires an old authored identity.
    }
    if (state.retired.contains(actor) || Now()>state.census_until || state.bindings.size()>=objects::kCensus) continue;
    const auto ai=Ptr(b,Ptr(b,actor,24,8),4,88),matrix=Ptr(b,actor,12,64);
    const auto kind=Family(b,ai);
    if (!matrix || uint8_t(kind)==0) continue;
    const auto vars=Vars(b,ai,kind); if (!vars) continue;
    if (kind==ObjectKind::Fire && !FireContract(b,vars)) continue;
    Binding o; o.actor=actor; o.ai=ai; o.vars=vars; o.matrix=matrix; o.kind=kind;
    o.x=LoadF(b,matrix+48); o.y=LoadF(b,matrix+52); o.z=LoadF(b,matrix+56);
    const auto asset=state.file_keys.find(actor);
    const uint64_t shape=uint8_t(kind);
    o.source=asset==state.file_keys.end() ? objects::Source(kind,o.x,o.y,o.z,shape) : objects::AssetSource(kind,asset->second);
    if (o.source) {
      state.identities.Record(o);
      state.bindings.emplace(actor,o);
#if defined(KK_DEV_TOOLS)
      if (kind==ObjectKind::Door && state.planes.size()<objects::kCensus)
        state.planes.try_emplace(o.source,State::Plane{o.x,o.y,o.z,-LoadF(b,matrix+16),-LoadF(b,matrix+20),-LoadF(b,matrix+24)});
#endif
      if (Trace()) REXLOG_INFO("KK dev object: role={} kind={} id={:016X} actor={:08X} vars={:08X} pos=({:.3f},{:.3f},{:.3f}) damage={} maximum={} levers={} type={} asset={:08X}",
          state.host ? "host" : "guest",uint8_t(kind),o.source,actor,vars,o.x,o.y,o.z,
          kind==ObjectKind::Breakable ? Load32(b,vars+552) : 0,kind==ObjectKind::Breakable ? Load32(b,vars+556) : 0,
          kind==ObjectKind::Door ? Load32(b,vars+612) : 0,Load32(b,vars),asset==state.file_keys.end() ? 0 : asset->second);
    }
  }
  // Keep terminal breakable bindings as tombstones while still registered.
  // Their final cumulative state is resent, including to a privately reloaded guest.
  for (auto it=state.bindings.begin();it!=state.bindings.end();) {
    if (members.contains(it->first)) { ++it; continue; }
    WithdrawRaft(b,it->second); state.retired.insert(it->first); it=state.bindings.erase(it);
  }
#if defined(KK_DEV_TOOLS)
  std::erase_if(state.pickups,[&](const auto& entry){ return !members.contains(entry.first); });
#endif
  state.world=w; state.next_census=Now()+1;
}
uint32_t Lever(const uint8_t* b,const Binding& o,unsigned slot) {
  const auto count=Load32(b,o.vars+612);
  // Keep paired620, marker640 and capability660 slots disjoint from the fixed
  // endpoint field676. Larger authored counts need a recovered layout; the
  // wider wire mask is not permission to read through adjacent native fields.
  if (count>4 || slot>=count) return 0;
  return Ptr(b,Load32(b,o.vars+640+slot*4),24,42);
}
ObjectState Sample(const uint8_t* b,const Binding& o) {
  ObjectState s{state.generation,state.level,1,o.source,o.kind};
  if (o.kind==ObjectKind::Breakable) { s.value=Load32(b,o.vars+552); s.position=float(Load32(b,o.vars+560)); }
  else if (o.kind==ObjectKind::Fire) {
    if (Track(b,o.ai,0x825FAF58)) return s;
    s.position=LoadF(b,o.vars+416); s.seconds=LoadF(b,o.vars+388);
    s.value=(s.seconds==0 || (Load32(b,o.actor+4)&0x200000)) ? 2 : 1;
    if (s.value==2) s.seconds=0;
  }
  else if (o.kind==ObjectKind::Raft) {
    if (!o.raft_ready) { s.value=o.sent || o.raft_classified ? 2 : UINT32_MAX; return s; }
    s.value=Load32(b,o.vars+376);
    s.position=LoadF(b,o.vars+420); s.seconds=LoadF(b,o.vars+4); s.raft=o.raft;
  }
  else {
    for (unsigned i=0;i<8;++i) if (const auto ext=Lever(b,o,i)) {
      if (Load16(b,ext+40)&1) s.value|=1u<<i;
    }
    s.position=LoadF(b,o.vars+1084); s.seconds=LoadF(b,o.vars+552);
    if (DoorScheduled(b,o.ai)) s.value|=kDoorScheduled;
  }
  return s;
}
void Observe(const uint8_t* b,Binding& o) {
  auto s=Sample(b,o);
  if (!ValidObjectState(s) || (o.sent && o.last==s) || state.number==UINT32_MAX) return;
  if (!state.outcomes.contains(s.source) && state.outcomes.size()>=objects::kCensus) { ++state.rejected; return; }
  o.last=s; o.sent=true; s.number=++state.number;
  state.pending[s.source]=state.outcomes[s.source]=s;
  if (s.kind==ObjectKind::Raft && s.value==2 && Trace())
    REXLOG_INFO("KK dev object raft authority withdrawn: role=host id={:016X} generation={}",s.source,state.generation);
#if defined(KK_DEV_TOOLS)
  if (state.debug_sources.size()<8 && (s.kind==ObjectKind::Breakable ? s.value>0 : s.position>0 || (s.value&255)))
    state.debug_sources.insert(s.source);
#endif
}
void WithdrawRaft(const uint8_t* b,Binding& o) {
  if (!state.host || o.kind!=ObjectKind::Raft) return;
  o.raft_ready=false; o.raft_classified=true;
  Observe(b,o); // Canonical withdrawal reads no retired components, even before the first pose.
}
void Flush() {
  for (unsigned n=0;n<16 && !state.outgoing.empty();++n) {
    if (!SubmitObjects(state.outgoing.front())) break;
    state.outgoing.pop_front();
  }
  for (unsigned n=0;n<16 && !state.pending.empty();++n) {
    const auto it=state.pending.begin(); Message m; m.type=MessageType::ObjectState; m.epoch=state.epoch; m.object=it->second;
    if (!SubmitObjects(m)) break;
    state.pending.erase(it);
  }
}
bool Request(const Binding& o,uint32_t value,float position=0,float seconds=0) {
  if (state.request==UINT32_MAX || state.outgoing.size()>=objects::kQueue) { ++state.rejected; return false; }
  Message m; m.type=MessageType::ObjectRequest; m.epoch=state.epoch;
  m.object={state.generation,state.level,++state.request,o.source,o.kind,value,position,seconds};
  if (!ValidObjectRequest(m.object)) { ++state.rejected; return false; }
  state.outgoing.push_back(m); ++state.requested;
  return true;
}
void Holds(const uint8_t* b) {
  const auto now=Now();
  const auto hero=Load32(b,0x82CCBF7C);
  const auto ai=Ptr(b,Ptr(b,hero,24,8),4,88),matrix=Ptr(b,hero,12,64);
  const auto human=Track(b,ai,0x826718B8) ? Ptr(b,ai,64,13388) : 0;
  const auto paired=human ? Load32(b,human+13376) : 0,marker=human ? Load32(b,human+13380) : 0;
  const auto paired_ext=Ptr(b,paired,24,42);
  Binding* use_door=nullptr; unsigned use_slot=0; float nearest=3;
  const auto condition=LocalPlayerCondition(b);
  if (human && matrix && use_until.load(std::memory_order_relaxed)>=now &&
      condition && *condition!=PlayerCondition::Dead && !observer::Active() && !observer::InCutscene(b)) {
    // Permitted fallback for player paths that never enter H_ETAT_Ia_porte:
    // hold native Use (RT without aim) close to, and facing, one installed marker.
    // Only own filtered input and own pose are observed. No human field is set.
    for (auto& [actor,o] : state.bindings) {
      if (o.kind!=ObjectKind::Door || !Valid(b,o) || Match(o.source,o.kind)!=&o ||
          Load32(b,o.vars)!=2 || LoadF(b,o.vars+1084)>=1) continue;
      for (unsigned i=0;i<8;++i) if (const auto ext=Lever(b,o,i)) {
        const auto m=Ptr(b,Load32(b,o.vars+640+4*i),12,64);
        if (!m || !(Load16(b,ext+40)&1)) continue;
        const auto dx=LoadF(b,m+48)-LoadF(b,matrix+48),dy=LoadF(b,m+52)-LoadF(b,matrix+52),dz=LoadF(b,m+56)-LoadF(b,matrix+56);
        if (!objects::NearLever(dx,dy,dz,-LoadF(b,matrix+16),-LoadF(b,matrix+20))) continue;
        const auto distance=std::hypot(dx,dy,dz);
        if (distance<nearest) { nearest=distance; use_door=&o; use_slot=i; }
      }
    }
  }
  for (auto& [actor,o] : state.bindings) {
    if (o.kind!=ObjectKind::Door || !Valid(b,o) || Match(o.source,o.kind)!=&o || Load32(b,o.vars)!=2) continue;
    uint32_t mask=use_door==&o ? 1u<<use_slot : 0;
    if (matrix && paired_ext && (Load16(b,paired_ext+40)&0x80) && LoadF(b,o.vars+1084)<1) {
      for (unsigned i=0;i<8;++i) if (const auto ext=Lever(b,o,i)) {
        const auto m=Ptr(b,Load32(b,o.vars+640+4*i),12,64);
        if (!m || !(Load16(b,ext+40)&1) || marker!=Load32(b,o.vars+640+4*i) ||
            paired!=Load32(b,o.vars+620+4*i)) continue;
        const auto distance=std::hypot(LoadF(b,m+48)-LoadF(b,matrix+48),LoadF(b,m+52)-LoadF(b,matrix+52),LoadF(b,m+56)-LoadF(b,matrix+56));
        if (std::isfinite(distance) && distance<=6) mask|=1u<<i;
      }
    }
    if (state.host) {
      for (unsigned i=0;i<8;++i) o.local_holds[i].Touch(mask&(1u<<i),now);
    } else {
      const bool renew=now>=o.next_hold;
      for (unsigned i=0;i<8;++i) {
        const auto bit=1u<<i;
        if ((mask&bit) && (!(o.sent_holds&bit) || renew)) {
          if (Request(o,i,0,1)) o.sent_holds|=bit;
        } else if (!(mask&bit) && (o.sent_holds&bit)) {
          if (Request(o,i,0,2)) o.sent_holds&=~bit;
        }
      }
      if (renew) o.next_hold=now+.2;
    }
    if (Trace() && mask!=o.observed_hold) {
      // No human state is written. The log also establishes whether the named
      // IA hold contract is actually reached by the local player at runtime.
      static uint64_t logged=0;
      if (logged++<128) REXLOG_INFO("KK dev object native hold: role={} id={:016X} mask={} human-mode={} origin={}",
          state.host ? "host" : "guest",o.source,mask,human ? Load32(b,human+2712) : 0,use_door==&o ? "filtered-use" : "native-paired");
    }
    o.observed_hold=mask;
  }
}
void Receive(const uint8_t* b) {
  const auto link=EnemyLinkSnapshot();
  for (const auto& m : TakeObjects()) {
    const auto& r=m.object;
    if (m.epoch!=state.epoch || r.generation!=state.generation || r.level!=state.level) { ++state.rejected; continue; }
    if (!state.host) {
      if (m.type==MessageType::ObjectState && state.replica.Observe(r)) {
        ++state.received;
#if defined(KK_DEV_TOOLS)
        if (state.debug_sources.size()<8 && (r.kind==ObjectKind::Breakable ? r.value>0 : r.position>0 || (r.value&255)))
          state.debug_sources.insert(r.source);
#endif
      }
      else ++state.rejected;
      continue;
    }
    auto* o=Match(r.source,r.kind);
    if (m.type!=MessageType::ObjectRequest || !o || !Valid(b,*o) || !link.partner || !link.connected || link.partner->level!=state.level) {
      ++state.rejected; continue;
    }
    auto matrix=o->matrix;
    if (r.kind==ObjectKind::Door) {
      if (Load32(b,o->vars)!=2 || r.value>=8 || !Lever(b,*o,r.value)) { ++state.rejected; continue; }
      matrix=Ptr(b,Load32(b,o->vars+640+r.value*4),12,64);
      if (!matrix) { ++state.rejected; continue; }
    }
    const auto& p=*link.partner;
    const double distance=std::hypot(std::hypot(double(p.x)-LoadF(b,matrix+48),double(p.y)-LoadF(b,matrix+52)),double(p.z)-LoadF(b,matrix+56));
    if (!state.requests.Admit(r,Now(),distance)) { ++state.rejected; continue; }
    if (r.kind==ObjectKind::Breakable) {
      if (o->pending_hits.size()>=16) { ++state.rejected; continue; }
      o->pending_hits.push_back({r,Now()+.5});
    }
    else if (r.kind==ObjectKind::Fire) {
      if (!FireContract(b,o->vars) || Sample(b,*o).value!=0) { ++state.rejected; continue; }
      o->fire_request=true;
    }
    else if (r.seconds!=0) {
      // Installed lever holds never tag a host marker or change host Jack.
      // Require the native installed bit and paired actor on this same door.
      if (r.seconds==1 && (!(Load16(b,Lever(b,*o,r.value)+40)&1) ||
          !Ptr(b,Load32(b,o->vars+620+4*r.value),24,42))) { ++state.rejected; continue; }
      o->remote_holds[r.value].Touch(r.seconds==1,Now());
    }
    else { // Handle installation has no replay that avoids host inventory state.
      ++state.rejected; continue;
    }
#if defined(KK_DEV_TOOLS)
    if (state.debug_sources.size()<8) state.debug_sources.insert(r.source);
#endif
    ++state.accepted;
  }
}
bool NativeHit(PPCContext& ctx,uint8_t* b,const Binding& o,uint32_t hero,uint32_t ai,uint32_t flags,uint32_t magnitude,float seconds=.5f) {
  const auto sp=ctx.r1.u32;
  if ((sp&15) || sp<0x40014000 || sp>=0xA0000000) return false;
  constexpr uint32_t pool=0x82C8EF30;
  const auto count=Load32(b,pool+12),used=Load32(b,pool+8);
  if (!count || count>4096 || used>=count || !Ptr(b,pool,0,count*116)) return false;
  uint8_t scratch[16384]; const auto start=sp-sizeof(scratch); const PPCContext saved=ctx;
  std::memcpy(scratch,b+start,sizeof(scratch)); ctx.r1.u64=start+12288;
  std::memset(b+start+12288,0,4096); Store32(b,start+12288,sp);
  constexpr uint32_t globals[]{0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
  uint32_t values[4]; for (unsigned i=0;i<4;++i) values[i]=Load32(b,globals[i]);
  Store32(b,globals[0],ai); Store32(b,globals[1],ai); Store32(b,globals[2],hero); Store32(b,globals[3],hero);
  const auto result=ctx.r1.u32+0x100;
  Store32(b,result,UINT32_MAX); Store32(b,result+4,0);
  StoreF(b,ctx.r1.u32+80,0); StoreF(b,ctx.r1.u32+84,0); StoreF(b,ctx.r1.u32+88,1);
  StoreF(b,ctx.r1.u32+96,LoadF(b,o.matrix+48)); StoreF(b,ctx.r1.u32+100,LoadF(b,o.matrix+52)); StoreF(b,ctx.r1.u32+104,LoadF(b,o.matrix+56));
  ctx.r3.u64=result; ctx.r4.s64=-1; ctx.r5.u64=flags; ctx.r6.u64=hero; ctx.r7.u64=0;
  ctx.r8.u64=o.actor; ctx.r9.s64=-1; ctx.r10.u64=magnitude; ctx.f1.f64=seconds;
  __imp__sub_82634428(ctx,b);
  const bool queued=Load32(b,result)!=UINT32_MAX && Load32(b,result+4)!=0;
  for (unsigned i=0;i<4;++i) Store32(b,globals[i],values[i]);
  std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved; return queued;
}
void Damage(PPCContext& ctx,uint8_t* b) {
  const auto output=ctx.r4.u32;
  // A private loader can run native prop callbacks before the new registry is
  // rebound. Never let raw guest hits remove geometry in that interval, since
  // an absolute damage snapshot cannot rebuild already-removed parts.
  if (!state.ready && REXCVAR_GET(kk_coop_enabled) && SessionInProcess()) {
    const auto v=SessionFlowSnapshot();
    const auto ai=Load32(b,0x82CCBF64);
    if (v.active && !v.host && v.policy==ChapterPolicy::Together && Family(b,ai)==ObjectKind::Breakable &&
        PoseAddress(output,4) && PoseAddress(ctx.r3.u32,12)) {
      Store32(b,output,0);
      StoreF(b,ctx.r3.u32,0); StoreF(b,ctx.r3.u32+4,0); StoreF(b,ctx.r3.u32+8,0);
      return;
    }
  }
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Breakable) : nullptr;
  const auto weak_before=o ? Load32(b,o->vars+560) : 0;
  if (o && state.host) {
    while (!o->pending_hits.empty()) {
      const auto pending=o->pending_hits.front();
      if (Now()>pending.until) { o->pending_hits.pop_front(); ++state.rejected; continue; }
      const auto& hit=pending.raw;
      // Use a local obstacle source, never host Jack's interaction state.
      // The original collector owns classification, weak count and part effects.
      if (!NativeHit(ctx,b,*o,o->actor,o->ai,hit.value,uint32_t(hit.position),hit.seconds)) break;
      o->pending_hits.pop_front(); ++state.applied;
      if (Trace()) REXLOG_INFO("KK dev object raw replay: id={:016X} flags={:08X} magnitude={} lifetime={:.3f} weak-before={}",
          o->source,hit.value,hit.position,hit.seconds,Load32(b,o->vars+560));
    }
  }
  if (!o || state.host) {
    __imp__sub_824ADD48(ctx,b);
    if (o && Trace() && PoseAddress(output,4) && (Load32(b,output) || weak_before!=Load32(b,o->vars+560)))
      REXLOG_INFO("KK dev object classification: id={:016X} class={} weak={}->{} damage-before={}",
          o->source,Load32(b,output),weak_before,Load32(b,o->vars+560),Load32(b,o->vars+552));
    return;
  }
  if (!PoseAddress(output,4) || !PoseAddress(ctx.r3.u32,12)) return;
  // Each successfully enqueued local hit is forwarded at the pool hook. Never
  // infer ownership from a per-object time latch or forward a classified delta.
  const auto* authority=state.replica.Find(o->kind,o->source);
  // Do not run the guest's raw collector: its special-hit branch can mutate
  // physics before returning a damage class. Native Go still consumes the
  // host's absolute delta and owns geometry, grid and capability updates.
  StoreF(b,ctx.r3.u32,0); StoreF(b,ctx.r3.u32+4,0); StoreF(b,ctx.r3.u32+8,0);
  Store32(b,o->vars+560,authority ? uint32_t(authority->position) : 0);
  const auto delta=authority ? objects::DamageDelta(Load32(b,o->vars+552),authority->value) : 0;
  Store32(b,output,delta);
  if (delta) ++state.applied;
}

void ApplyDoor(uint8_t* b,const Binding& o,const ObjectState& s) {
  for (unsigned i=0;i<8;++i) if (const auto ext=Lever(b,o,i)) {
    auto flags=Load16(b,ext+40);
    flags=uint16_t((flags&~1u)|((s.value>>i)&1));
    Store16(b,ext+40,flags);
  }
  StoreF(b,o.vars+1084,s.position);
  StoreF(b,o.vars+552,s.seconds);
}
void FinishLocalLever(uint8_t* b,Binding& o,uint32_t mask) {
  // The guest withheld its native activation until authority arrived. Finish
  // only that same Jack/marker interaction using the verified inventory-return
  // branch in 8256C3F8 (C858..C888). Otherwise the guest stays holding item10.
  const auto hero=Load32(b,0x82CCBF7C),ai=Ptr(b,Ptr(b,hero,24,8),4,88);
  const auto vars=Ptr(b,ai,64,13384),game=Load32(b,kDoorGamePtr);
  if (state.host || !vars || !Track(b,ai,0x826718B8) || !PoseAddress(game,17804)) return;
  bool same=false;
  for (unsigned i=0;i<8;++i) if ((mask&(1u<<i)) && Lever(b,o,i))
    same|=Load32(b,vars+13380)==Load32(b,o.vars+640+4*i);
  if (same && Load32(b,game+17800)) {
    Store32(b,game+17800,0);
    if (Load32(b,game+13128)==10) { Store32(b,game+13128,Load32(b,game+13204)); Store32(b,game+13204,0); }
    if (Load32(b,game+13204)==10) Store32(b,game+13204,0);
    if (Load32(b,game+13280)==10) Store32(b,game+13280,0);
  }
  o.local_levers&=~mask;
}
// 826F9DC0 consumes only the current AI and track index. When servicing an
// inactive door, select only its own recovered door track, never the outer AI's.
void EndDoorTracks(PPCContext& ctx,uint8_t* b,const Binding& o) {
  for (const auto slot : objects::DoorSlots([&](uint32_t a){ return Load32(b,a); },
      [&](uint32_t a){ return Load16(b,a); },o.ai)) {
    Store32(b,0x82CCBF58,slot);
    __imp__sub_826F9DC0(ctx,b);
  }
}
bool PrepareDoorGeometry(PPCContext& ctx,uint8_t* b,const Binding& o) {
  if (!Load32(b,o.vars+592)) Store32(b,o.vars+592,o.actor);
  if (!Load32(b,o.vars+564)) return true;
  const auto length=[&](uint32_t p) { return std::hypot(LoadF(b,p),LoadF(b,p+4),LoadF(b,p+8)); };
  const auto f=length(o.vars+1116),u=length(o.vars+1128);
  if (std::isfinite(f) && std::isfinite(u) && f>.9f && f<1.1f && u>.9f && u<1.1f) return true;
  const auto target=Load32(b,o.vars+364),pivot=Load32(b,o.vars+368);
  if (!Ptr(b,target,12,64) || (pivot && !Ptr(b,pivot,12,64))) return false;
  // Exact local transform setup from82574D80 (755CC..756B8), without its
  // authored predicate or participant setup. Dormant init left these vectors0.
  const auto vector=ctx.r1.u32+256,origin=vector+16;
  ctx.r3.u64=o.actor; ctx.r4.u64=vector; __imp__sub_8270FA58(ctx,b);
  for (unsigned i=0;i<3;++i) Store32(b,o.vars+1116+4*i,Load32(b,vector+4*i));
  ctx.r3.u64=o.actor; ctx.r4.u64=vector; __imp__sub_8270F918(ctx,b);
  for (unsigned i=0;i<3;++i) Store32(b,o.vars+1128+4*i,Load32(b,vector+4*i));
  if (pivot) {
    ctx.r3.u64=o.actor; ctx.r4.u64=origin; __imp__sub_8270FCE0(ctx,b);
    ctx.r3.u64=pivot; ctx.r4.u64=vector; __imp__sub_8270FCE0(ctx,b);
    for (unsigned i=0;i<3;++i) StoreF(b,vector+4*i,LoadF(b,vector+4*i)-LoadF(b,origin+4*i));
    ctx.r3.u64=o.actor; ctx.r4.u64=vector; ctx.r5.u64=origin; __imp__sub_8271A6F8(ctx,b);
    for (unsigned i=0;i<3;++i) Store32(b,o.vars+1140+4*i,Load32(b,origin+4*i));
  }
  if (Trace()) REXLOG_INFO("KK dev object door geometry initialized: role={} id={:016X} forward={} up={} pivot={:08X} offset=({:.3f},{:.3f},{:.3f})",
      state.host ? "host" : "guest",o.source,length(o.vars+1116),length(o.vars+1128),pivot,
      LoadF(b,o.vars+1140),LoadF(b,o.vars+1144),LoadF(b,o.vars+1148));
  return true;
}
void RefreshDoor(PPCContext& ctx,uint8_t* b,Binding& o,const ObjectState& s,bool own_track=true) {
  if (Load32(b,0x82CCBF64)!=o.ai || Load32(b,0x82CCBF6C)!=o.actor) return;
  const bool ended=!(s.value&kDoorScheduled) && DoorScheduled(b,o.ai);
  if (!ended && LoadF(b,o.vars+1084)==s.position && o.driven_position==s.position &&
      (s.position!=1 || Load32(b,o.vars+1096)==1)) return;
  // The caller supplies this validated door's AI/actor context. An isolated
  // side call has no live door track index and must not stop the outer track.
  // Retain native geometry/grid/capability work and protect the hook caller's
  // volatile registers, stack scratch and dispatcher globals from the extra call.
  const auto sp=ctx.r1.u32;
  if ((sp&15) || sp<0x40014000 || sp>=0xA0000000) return;
  uint8_t scratch[16384]; const auto start=sp-sizeof(scratch); const PPCContext saved=ctx;
  std::memcpy(scratch,b+start,sizeof(scratch));
  constexpr uint32_t globals[]{0x82CCBF58,0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
  uint32_t values[7]; for (unsigned i=0;i<7;++i) values[i]=Load32(b,globals[i]);
  ctx.r1.u64=start+12288; std::memset(b+start+12288,0,4096); Store32(b,start+12288,sp);
  const auto restore=[&] {
    for (unsigned i=0;i<7;++i) Store32(b,globals[i],values[i]);
    std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved;
  };
  if (!PrepareDoorGeometry(ctx,b,o)) { restore(); return; }
  ApplyDoor(b,o,s);
  {
    auto guarded=s;
    if (!own_track) guarded.value|=kDoorScheduled;
    DoorStopGuard guard(o,guarded); __imp__sub_82552200(ctx,b);
    if (ended && DoorScheduled(b,o.ai)) {
      if (own_track) __imp__sub_826F9DC0(ctx,b);
      else EndDoorTracks(ctx,b,o);
    }
  }
  restore(); o.driven_position=s.position;
#if defined(KK_DEV_TOOLS)
  if (Trace() && s.position==1) REXLOG_INFO("KK dev object door endpoint: role={} id={:016X} fraction={} endpoint={} scheduled={}",
      state.host ? "host" : "guest",o.source,LoadF(b,o.vars+1084),Load32(b,o.vars+1096),DoorScheduled(b,o.ai));
  if (Trace() && s.position==1) REXLOG_INFO("KK dev object door final geometry: role={} id={:016X} pos=({:.3f},{:.3f},{:.3f}) forward={} up={} pivot={:08X}",
      state.host ? "host" : "guest",o.source,LoadF(b,o.matrix+48),LoadF(b,o.matrix+52),LoadF(b,o.matrix+56),
      std::hypot(LoadF(b,o.vars+1116),LoadF(b,o.vars+1120),LoadF(b,o.vars+1124)),
      std::hypot(LoadF(b,o.vars+1128),LoadF(b,o.vars+1132),LoadF(b,o.vars+1136)),Load32(b,o.vars+368));
  if (!state.host && s.position==1 && Load32(b,o.vars+1096)==1) state.observed_change=true;
#endif
}
void OpenHeldDoor(PPCContext& ctx,uint8_t* b,Binding& o,bool own_track=true) {
  if (!state.host || !Components(b,o) || LoadF(b,o.vars+1084)>=1) return;
  const auto now=Now();
  bool active=false;
  for (unsigned i=0;i<8;++i) active|=o.local_holds[i].Active(now) || o.remote_holds[i].Active(now);
  if (!active) return;
  if (Load32(b,0x82CCBF64)!=o.ai || Load32(b,0x82CCBF6C)!=o.actor) {
#if defined(KK_DEV_TOOLS)
    ++o.context_misses;
#endif
    return;
  }
  // A native wait-to-loop transition may invoke both callbacks in one AI pass.
  if (o.hold_tick==state.ai_tick) return;
  o.hold_tick=state.ai_tick;
#if defined(KK_DEV_TOOLS)
  ++o.hold_calls;
#endif
  // The wait track has not necessarily normalized its authored opening time.
  // Verified native leaf: only door timing fields and its extension flag change.
  if (LoadF(b,o.vars+1088)<=0) {
    const PPCContext saved=ctx; __imp__sub_82448E08(ctx,b); ctx=saved;
  }
  bool open=false;
  const auto dt=LoadF(b,0x82C636E0),duration=LoadF(b,o.vars+1088);
  for (unsigned i=0;i<8;++i) {
    open|=o.local_holds[i].Step(now,dt,duration);
    open|=o.remote_holds[i].Step(now,dt,duration);
  }
  const auto game=Load32(b,kDoorGamePtr);
  constexpr uint32_t offsets[]{13128,13204,13280,17800};
  if (open && Components(b,o)) {
    const auto hero=Load32(b,0x82CCBF7C),hero_ai=Ptr(b,Ptr(b,hero,24,8),4,88);
    const auto human=Ptr(b,hero_ai,64,13388);
    const auto host_input=[&] {
      std::array<uint32_t,8> fields{};
      if (human) { fields[0]=Load32(b,human+2712); fields[1]=Load32(b,human+13376);
        fields[2]=Load32(b,human+13380); fields[3]=Load32(b,human+13384); }
      if (PoseAddress(game,17804)) for (unsigned i=0;i<4;++i) fields[4+i]=Load32(b,game+offsets[i]);
      return fields;
    };
    const auto before=host_input();
    auto endpoint=Sample(b,o); endpoint.position=1; endpoint.value&=~kDoorScheduled;
    RefreshDoor(ctx,b,o,endpoint,own_track);
    Observe(b,o);
    REXLOG_INFO("KK co-op objects: installed lever opened id={:016X} native-duration={} fraction={} endpoint={} host-jack-readable={} host-jack-unchanged={}",
        o.source,LoadF(b,o.vars+1088),LoadF(b,o.vars+1084),Load32(b,o.vars+1096),human!=0 && PoseAddress(game,17804),before==host_input());
  }
}
void IdleDoors(PPCContext& ctx,uint8_t* b) {
  // Some authored doors remain dormant outside their story activation zone.
  // A validated installed-lever hold may still complete their own one-way
  // endpoint. The native update retains transforms, collision, grid and caps.
  for (auto& [actor,o] : state.bindings) {
    if (o.kind!=ObjectKind::Door || !Valid(b,o) || Match(o.source,o.kind)!=&o ||
        Load32(b,o.vars)!=2 || Load32(b,o.vars+568)!=1 || Load32(b,o.vars+572)!=0 ||
        !Lever(b,o,0)) continue;
    bool ready=true;
    for (unsigned slot=0;slot<Load32(b,o.vars+612);++slot)
      ready&=Lever(b,o,slot)!=0 && Ptr(b,Load32(b,o.vars+620+4*slot),24,42)!=0;
    if (!ready) continue;
    const auto* authority=state.host ? nullptr : state.replica.Find(o.kind,o.source);
    if (!state.host && (!authority || authority->position!=1)) continue;
    const auto ai=Load32(b,0x82CCBF64),current_actor=Load32(b,0x82CCBF6C);
    Store32(b,0x82CCBF64,o.ai); Store32(b,0x82CCBF6C,o.actor);
    if (state.host) OpenHeldDoor(ctx,b,o,false);
    else RefreshDoor(ctx,b,o,*authority,false);
    Store32(b,0x82CCBF64,ai); Store32(b,0x82CCBF6C,current_actor);
  }
}
void Door(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Door) : nullptr;
  if (!o) { __imp__sub_8256C3F8(ctx,b); return; }
#if defined(KK_DEV_TOOLS)
  ++o->loop_calls;
#endif
  const auto* s=state.host ? nullptr : state.replica.Find(o->kind,o->source);
  if (!state.host && o->local_levers) {
    const auto done=s ? (s->position==1 ? o->local_levers : o->local_levers&s->value) : 0;
    if (done) FinishLocalLever(b,*o,done);
    if (Now()>o->local_lever_until) FinishLocalLever(b,*o,o->local_levers);
  }
  for (unsigned i=0;i<8;++i) if (const auto ext=Lever(b,*o,i)) {
    const auto flags=Load16(b,ext+40);
    if (!state.host && (flags&0x8000) && !(flags&1)) {
      if (s && ((s->value&(1u<<i)) || s->position==1)) FinishLocalLever(b,*o,1u<<i);
      else if (!(o->local_levers&(1u<<i))) {
        if (Request(*o,i)) { o->local_levers|=1u<<i; o->local_lever_until=Now()+3; }
        else FinishLocalLever(b,*o,1u<<i);
      }
      Store16(b,ext+40,flags&0x7fff);
    }
  }
  if (s) ApplyDoor(b,*o,*s);
  __imp__sub_8256C3F8(ctx,b);
  if (s && Valid(b,*o)) RefreshDoor(ctx,b,*o,*s);
  OpenHeldDoor(ctx,b,*o);
  if (state.host && Components(b,*o) && !DoorScheduled(b,o->ai)) Observe(b,*o);
}
void Breakable(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Breakable) : nullptr;
  if (o) o->damage_tick=state.ai_tick;
  __imp__sub_82515CF0(ctx,b);
  // Capture the final native damage in the same call, before a later frame
  // retires an exploded obstacle from the full registry.
  if (o && state.host && Components(b,*o)) Observe(b,*o);
#if defined(KK_DEV_TOOLS)
  if (o && !state.host && Components(b,*o)) if (const auto* s=state.replica.Find(o->kind,o->source))
    if (s->value && Load32(b,o->vars+552)>=s->value) state.observed_change=true;
#endif
}
void ReplayBreakables(PPCContext& ctx,uint8_t* b) {
  if (state.host) return;
  for (auto& [actor,o] : state.bindings) {
    if (o.kind!=ObjectKind::Breakable || !Valid(b,o) || Match(o.source,o.kind)!=&o ||
        o.damage_tick==state.ai_tick) continue;
    const auto* s=state.replica.Find(o.kind,o.source);
    if (!s || !objects::DamageDelta(Load32(b,o.vars+552),s->value)) continue;
    const auto track=objects::BreakableConsumer([&](uint32_t a){ return Load32(b,a); },
        [&](uint32_t a){ return Load16(b,a); },o.ai);
    if (!track || (!track->init && !Load32(b,o.vars+556))) continue;
    // A privately reloaded prop can remain outside the native active AI list.
    // Wake only its own init/Go callback through the native track dispatcher.
    // Init advances its own scheduler block and registers parts/grid/capability.
    // A later initialized Go consumes only the missing absolute host damage.
    const auto sp=ctx.r1.u32;
    if ((sp&15) || sp<0x40014000 || sp>=0xA0000000) continue;
    uint8_t scratch[16384]; const auto start=sp-sizeof(scratch); const PPCContext saved=ctx;
    std::memcpy(scratch,b+start,sizeof(scratch));
    constexpr uint32_t globals[]{0x82CCBF58,0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
    uint32_t values[7]; for (unsigned i=0;i<7;++i) values[i]=Load32(b,globals[i]);
    ctx.r1.u64=start+12288; std::memset(b+start+12288,0,4096); Store32(b,start+12288,sp);
    Store32(b,0x82CCBF64,o.ai); Store32(b,0x82CCBF68,o.ai);
    Store32(b,0x82CCBF6C,o.actor); Store32(b,0x82CCBF70,o.actor);
    Store32(b,0x82CCBF58,track->slot);
    ctx.r3.u64=track->block; ctx.r4.u64=o.ai+(track->slot+1)*4; ctx.r5.u64=0;
    const auto before=Load32(b,o.vars+552);
    __imp__sub_8271CCB8(ctx,b);
    const auto result=ctx.r3.u32;
    // Checkpoint init can remove every part and the obstacle, then return1
    // before computing maximum556 or ever entering Go. Keep that native
    // geometry cleanup and restore only its absolute semantic damage counter.
    const bool terminal=track->init && result==1 && objects::BreakableCheckpointGone(
        [&](uint32_t a){ return Load32(b,a); },o.actor,o.vars);
    if (terminal) {
      Store32(b,o.vars+552,s->value); Store32(b,o.vars+560,uint32_t(s->position)); ++state.applied;
    }
    for (unsigned i=0;i<7;++i) Store32(b,globals[i],values[i]);
    std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved; o.damage_tick=state.ai_tick;
    if (Trace()) REXLOG_INFO("KK dev object private replay: id={:016X} init={} slot={} result={} terminal={} damage={}->{} host={} maximum={} flags={:08X} capability={}",
        o.source,track->init,track->slot,result,terminal,before,Load32(b,o.vars+552),s->value,Load32(b,o.vars+556),Load32(b,o.actor+4),Load32(b,o.vars+564));
#if defined(KK_DEV_TOOLS)
    if (Load32(b,o.vars+552)>=s->value) state.observed_change=true;
#endif
  }
}
void BreakableInit(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Breakable) : nullptr;
  if (o) o->damage_tick=state.ai_tick;
  __imp__sub_82516128(ctx,b);
  // The ordinary scheduler can reach checkpoint cleanup before the isolated
  // replay pass. Preserve the same semantic endpoint in that path too. Parts
  // and collision must already have been removed by the native initializer.
  if (o && !state.host && ctx.r3.u32==1 && Components(b,*o) &&
      objects::BreakableCheckpointGone([&](uint32_t a){ return Load32(b,a); },o->actor,o->vars)) {
    if (const auto* s=state.replica.Find(o->kind,o->source)) {
      Store32(b,o->vars+552,s->value); Store32(b,o->vars+560,uint32_t(s->position));
#if defined(KK_DEV_TOOLS)
      if (s->value) state.observed_change=true;
#endif
    }
  }
}
bool GuestFireScope() {
  if (!REXCVAR_GET(kk_coop_enabled) || !SessionInProcess()) return false;
  const auto v=SessionFlowSnapshot();
  return v.active && !v.host && v.policy==ChapterPolicy::Together;
}
bool PrepareFirePath(PPCContext& ctx,uint8_t* b,const Binding& o) {
  //825FAF58 selects the first authored target group;825F88F8 follows that
  // SAME key on each local node. Preflight the whole bounded chain because
  //8240C5B8 otherwise reads group[count] when a key is absent.
  const auto read=[&](uint32_t a){ return Load32(b,a); };
  const auto first=objects::FirstPathTarget(read,o.actor);
  if (!first) return false;
  const auto key=first->key;
  auto point=NativePoint(ctx,b,o.actor); if (!point) return false;
  std::set<uint32_t> seen{o.actor}; uint32_t actor=o.actor; float total=0;
  for (unsigned n=0;n<100;++n) {
    const auto target_entry=objects::FirstPathTarget(read,actor,key);
    if (!target_entry) return false;
    const auto next=target_entry->actor;
    if (!next) {
      if (n==0) return false; // Init unconditionally reads its first target.
      break;
    }
    const auto target=NativePoint(ctx,b,next); if (!target) return false;
    const float distance=std::hypot(target->x-point->x,target->y-point->y,target->z-point->z);
    if (!std::isfinite(distance) || total+distance>138.6f) return false;
    total+=distance;
    if (next==o.actor) break;
    if (!seen.insert(next).second || n==99) return false;
    actor=next; point=target;
  }
  const auto authored=LoadF(b,o.vars+424);
  // Zero extent is a legitimate native point fire, observed in level301.
  // Keep the authored extent even when it covers only part of the path.
  return total+.1f>=authored;
}
void RequestIgnition(const uint8_t* b,Binding& o) {
  if (state.host || Now()<o.next_ignite) return;
  const auto m=Ptr(b,Load32(b,0x82CCBF7C),12,64);
  const auto condition=LocalPlayerCondition(b);
  if (!m || !condition || *condition==PlayerCondition::Dead || observer::InCutscene(b) ||
      std::hypot(LoadF(b,m+48)-o.x,LoadF(b,m+52)-o.y,LoadF(b,m+56)-o.z)>6) return;
  if (Request(o,1)) o.next_ignite=Now()+.5;
}
void FireInit(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Fire) : nullptr;
  const auto* s=o && !state.host ? state.replica.Find(o->kind,o->source) : nullptr;
  const bool asked=o && FireContract(b,o->vars) && (state.host ? o->fire_request : s && s->value>0);
  if (o) o->fire_tick=state.ai_tick;
  // Native Init keeps its own path references, effect table and checkpoint bit.
  // Only a host outcome or a bounded native guest ignition bypasses its LOCAL
  // predicate. No predicate or completion callback address crosses the wire.
  __imp__sub_825FAF58(ctx,b);
  if (!o || !Components(b,*o)) return;
  // Let a normal native initializer finish first, including legitimate point
  // fires. Only a refused local predicate needs the stricter chain preflight.
  if (ctx.r3.u32==4 && asked) {
    if (PrepareFirePath(ctx,b,*o)) {
      const auto authored=Load32(b,o->vars);
      Store32(b,o->vars,0); __imp__sub_825FAF58(ctx,b); Store32(b,o->vars,authored);
    } else {
      o->fire_request=false;
      if (!o->fire_path_refused) {
        o->fire_path_refused=true; ++state.rejected;
        if (Trace()) REXLOG_WARN("KK dev object fire path: refused incomplete local chain id={:016X}",o->source);
      }
    }
  }
  const auto value=Sample(b,*o);
  if (state.host) { if (value.value) o->fire_request=false; Observe(b,*o); }
  else if (value.value && (!s || s->value==0)) RequestIgnition(b,*o);
}
void FireBurn(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Fire) : nullptr;
  // A native guest loader may initialize a path before the load barrier and
  // stable census exist. Keep effects frozen until an exact authority binds.
  if (!o) {
    const auto vars=Vars(b,Load32(b,0x82CCBF64),ObjectKind::Fire);
    if (GuestFireScope() && vars && FireContract(b,vars)) { ctx.r3.u64=0; return; }
    __imp__sub_8260F858(ctx,b); return;
  }
  o->fire_tick=state.ai_tick;
  if (state.host) {
    __imp__sub_8260F858(ctx,b);
    if (Components(b,*o)) Observe(b,*o);
    return;
  }
  const auto* s=state.replica.Find(o->kind,o->source);
  if (!s || !s->value) { RequestIgnition(b,*o); ctx.r3.u64=0; return; }
  if (!FireContract(b,o->vars) || s->position>LoadF(b,o->vars+424)+.001f) { ctx.r3.u64=0; return; }
  const float speed=LoadF(b,o->vars+380),dt=LoadF(b,0x82C636E0);
  if (!std::isfinite(dt) || dt<=0 || dt>.1f) { ctx.r3.u64=0; return; }
  StoreF(b,o->vars+416,s->position);
  StoreF(b,o->vars+388,s->seconds>0 ? s->seconds+dt : s->seconds);
  StoreF(b,o->vars+380,0); // Native walker creates only the host's reached sections.
  __imp__sub_8260F858(ctx,b); // Includes native effect/sound cleanup and destruction.
  if (Components(b,*o)) {
    StoreF(b,o->vars+380,speed);
    StoreF(b,o->vars+416,s->position); StoreF(b,o->vars+388,s->seconds);
#if defined(KK_DEV_TOOLS)
    state.observed_change=true;
#endif
  }
}
void ReplayFires(PPCContext& ctx,uint8_t* b) {
  for (auto& [actor,o] : state.bindings) {
    if (o.kind!=ObjectKind::Fire || !Valid(b,o) || Match(o.source,o.kind)!=&o ||
        !FireContract(b,o.vars) || o.fire_tick==state.ai_tick) continue;
    const auto* s=state.host ? nullptr : state.replica.Find(o.kind,o.source);
    if (state.host ? !o.fire_request : !s || !s->value) continue;
    // Use this object's own native state word and dispatcher, as for obstacle
    // replay. Refuse competing fire consumers instead of guessing a slot.
    const auto first=Load16(b,o.ai+80),last=Load16(b,o.ai+82);
    if (first>last || last>=5) continue;
    uint32_t block=0,slot=0; unsigned count=0;
    for (unsigned n=first;n<=last;++n) {
      const auto p=Ptr(b,o.ai,(n+6)*4,24);
      if (p && (Load32(b,p+20)==0x825FAF58 || Load32(b,p+20)==0x8260F858)) { block=p; slot=n; ++count; }
    }
    const auto sp=ctx.r1.u32;
    if (count!=1 || (sp&15) || sp<0x40014000 || sp>=0xA0000000) continue;
    uint8_t scratch[16384]; const auto start=sp-sizeof(scratch); const PPCContext saved=ctx;
    std::memcpy(scratch,b+start,sizeof(scratch));
    constexpr uint32_t globals[]{0x82CCBF58,0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
    uint32_t values[7]; for (unsigned n=0;n<7;++n) values[n]=Load32(b,globals[n]);
    ctx.r1.u64=start+12288; std::memset(b+start+12288,0,4096); Store32(b,start+12288,sp);
    Store32(b,globals[0],slot); Store32(b,globals[3],o.ai); Store32(b,globals[4],o.ai);
    Store32(b,globals[5],o.actor); Store32(b,globals[6],o.actor);
    ctx.r3.u64=block; ctx.r4.u64=o.ai+(slot+1)*4; ctx.r5.u64=0;
    __imp__sub_8271CCB8(ctx,b);
    for (unsigned n=0;n<7;++n) Store32(b,globals[n],values[n]);
    std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved;
  }
}
bool RaftRoute(PPCContext& ctx,uint8_t* b,Binding& o) {
  const auto start=Load32(b,o.vars+204),key=Load32(b,o.vars+224);
  if (!start || !key || Track(b,o.ai,0x825FD388)) return false;
  if (!o.route.nodes.empty() && o.route.nodes.front()==start && o.route.key==key) return true;
  o.route={}; o.raft_ready=o.raft_driven=false;
  if (Now()<o.next_route) return false;
  o.next_route=Now()+1;
  std::vector<uint32_t> nodes; std::set<uint32_t> seen;
  uint32_t node=start; uint64_t signature=14695981039346656037ull; bool loop=false;
  const auto read=[&](uint32_t a){ return Load32(b,a); };
  const auto half=[&](uint32_t a){ return Load16(b,a); };
  while (node) {
    if (nodes.size()>=128 || !seen.insert(node).second || !PoseAddress(node,32) ||
        (Load32(b,node)&0x10000001)) return false; // Static, unparented authored nodes only.
    const auto p=NativePoint(ctx,b,node); if (!p) return false;
    const auto id=enemies::AuthoredKey(0x5246544e,p->x,p->y,p->z); if (!id) return false;
    signature=enemies::Mix(signature,id); nodes.push_back(node);
    const auto target=objects::RaftPathTarget(read,half,node,key); if (!target) return false;
    node=target->actor;
    if (node==start) { loop=true; break; }
  }
  if (nodes.empty() || loop) return false; // Linear routes only; a loop needs an absolute lap counter.
  o.route={std::move(nodes),key,enemies::Mix(signature,uint64_t(seen.size())*2+loop)};
  return o.route.signature!=0;
}
bool ReadRaft(PPCContext& ctx,uint8_t* b,Binding& o) {
  o.raft_classified=Load32(b,o.vars+204) && Load32(b,o.vars+224) && !Track(b,o.ai,0x825FD388);
  if (!RaftRoute(ctx,b,o)) return false;
  const auto find=[&](uint32_t actor)->uint8_t {
    const auto it=std::find(o.route.nodes.begin(),o.route.nodes.end(),actor);
    return it==o.route.nodes.end() ? 255 : uint8_t(it-o.route.nodes.begin());
  };
  const auto current=find(Load32(b,o.vars+240)),next=find(Load32(b,o.vars+244));
  if (current==255 || (Load32(b,o.vars+244) && next==255)) return false;
  const auto position=NativePoint(ctx,b,o.actor),forward=NativePoint(ctx,b,o.actor,__imp__sub_8270FA58),up=NativePoint(ctx,b,o.actor,__imp__sub_8270F918);
  if (!position || !forward || !up) return false;
  const float fl=std::hypot(forward->x,forward->y,forward->z);
  const float raw_up=std::hypot(up->x,up->y,up->z);
  // Native82711328 rebuilds unit axes. Do not silently strip an authored
  // scale/shear from only the guest; normalize numerical drift only.
  if (std::abs(fl-1)>=.01f || std::abs(raw_up-1)>=.01f) return false;
  const ShotVector f{forward->x/fl,forward->y/fl,forward->z/fl};
  const float dot=f.x*up->x+f.y*up->y+f.z*up->z;
  if (std::abs(dot)>=.02f) return false;
  const ShotVector un{up->x-dot*f.x,up->y-dot*f.y,up->z-dot*f.z};
  const float ul=std::hypot(un.x,un.y,un.z); if (ul<.001f) return false;
  o.raft.route=o.route.signature; o.raft.current=current; o.raft.next=next;
  o.raft.pose={position->x,position->y,position->z,f.x,f.y,f.z,un.x/ul,un.y/ul,un.z/ul,
      LoadF(b,o.vars+1296),LoadF(b,o.vars+1300),LoadF(b,o.vars+1304)};
  o.raft_ready=true;
  if (!ValidObjectState(Sample(b,o))) { o.raft_ready=false; return false; }
  return true;
}
bool ApplyRaft(PPCContext& ctx,uint8_t* b,Binding& o,const ObjectState& s) {
  if (!RaftRoute(ctx,b,o) || s.raft.route!=o.route.signature || s.raft.current>=o.route.nodes.size() ||
      s.raft.next!=(s.raft.current+1<o.route.nodes.size() ? s.raft.current+1 : 255)) return false;
  const auto it=std::find(o.route.nodes.begin(),o.route.nodes.end(),Load32(b,o.vars+240));
  if (it==o.route.nodes.end()) return false;
  const auto previous=size_t(it-o.route.nodes.begin());
  if (previous>s.raft.current) return false; // Native node parameters cannot be rolled backwards safely.
  const bool was_driven=o.raft_driven;
  const auto sp=ctx.r1.u32;
  if ((sp&15) || sp<0x40014000 || sp>=0xA0000000) return false;
  uint8_t scratch[16384]; const auto start=sp-sizeof(scratch); const PPCContext saved=ctx;
  std::memcpy(scratch,b+start,sizeof(scratch));
  constexpr uint32_t globals[]{0x82CCBF58,0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
  uint32_t values[7]; for (unsigned n=0;n<7;++n) values[n]=Load32(b,globals[n]);
  ctx.r1.u64=start+12288; std::memset(b+start+12288,0,4096); Store32(b,start+12288,sp);
  Store32(b,globals[3],o.ai); Store32(b,globals[4],o.ai); Store32(b,globals[5],o.actor); Store32(b,globals[6],o.actor);
  // Reproduce each missed LOCAL node's movement parameters. Decompiled825EF890
  // calls824FE3C0/82507B00, which update speed/effect/link metadata, not arbitrary
  // peer callbacks. Skipping nodes loses persistent nonzero design values.
  for (size_t n=previous+1;n<=s.raft.current;++n) {
    Store32(b,o.vars+240,o.route.nodes[n]);
    __imp__sub_825EF890(ctx,b);
  }
#if defined(KK_DEV_TOOLS)
  if (previous<s.raft.current && Trace())
    REXLOG_INFO("KK dev object raft native nodes: id={:016X} from={} to={} steps={}",o.source,previous,s.raft.current,s.raft.current-previous);
#endif
  const auto p=ctx.r1.u32+256;
  for (unsigned n=0;n<9;++n) StoreF(b,p+16*(n/3)+4*(n%3),s.raft.pose[n]);
  ctx.r3.u64=o.actor; ctx.r4.u64=p; __imp__sub_827117E0(ctx,b);
  ctx.r3.u64=o.actor; ctx.r4.u64=p+16; ctx.r5.u64=p+32; __imp__sub_82711328(ctx,b);
  // Rebind route ordinals to LOCAL references. FollowNetwork is withheld only
  // after this exact route/pose has been applied; local rider reflex still runs.
  Store32(b,o.vars+240,o.route.nodes[s.raft.current]);
  Store32(b,o.vars+244,s.raft.next==255 ? 0 : o.route.nodes[s.raft.next]);
  Store32(b,o.vars+376,s.value); StoreF(b,o.vars+416,s.position); StoreF(b,o.vars+420,s.position); StoreF(b,o.vars+4,s.seconds);
  if (!o.raft_driven) for (unsigned n=0;n<3;++n) StoreF(b,o.vars+1284+4*n,s.raft.pose[n]);
  for (unsigned n=0;n<3;++n) StoreF(b,o.vars+1296+4*n,s.raft.pose[9+n]);
  for (unsigned n=0;n<7;++n) Store32(b,globals[n],values[n]);
  std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved;
#if defined(KK_DEV_TOOLS)
  if (o.raft_driven && std::hypot(o.raft.pose[0]-s.raft.pose[0],o.raft.pose[1]-s.raft.pose[1],o.raft.pose[2]-s.raft.pose[2])>.1f)
    state.observed_change=true;
#endif
  o.raft=s.raft; o.raft_ready=o.raft_driven=true;
  if (!was_driven && Trace())
    REXLOG_INFO("KK dev object raft authority bound: role={} id={:016X} route={:016X} current={}/{}",state.host ? "host" : "guest",o.source,s.raft.route,s.raft.current,s.raft.next);
  return true;
}
void UpdateRafts(PPCContext& ctx,uint8_t* b) {
  for (auto& [actor,o] : state.bindings) {
    if (o.kind!=ObjectKind::Raft || !Valid(b,o) || Match(o.source,o.kind)!=&o) continue;
    if (state.host) {
      o.raft_ready=false;
#if defined(KK_DEV_TOOLS)
      static const double unavailable_until=[] {
        const auto value=std::getenv("KK_DEV_OBJECT_RAFT_UNAVAILABLE"); if (!value || !*value) return -1.0;
        char* end=nullptr; const double t=std::strtod(value,&end);
        return end!=value && !*end && std::isfinite(t) && t>=0 && t<=300 ? t : -1.0;
      }();
      const double elapsed=kk::DevSecondsSinceMenu();
      if (elapsed>=0 && elapsed<unavailable_until) {
        o.raft_classified=true; continue; // Explicit initial unsupported-contract fixture, no native writes.
      }
      if (o.source==state.raft_fixture && Now()<state.raft_withdraw_until) continue;
#endif
      ReadRaft(ctx,b,o); // Regular10Hz sample publishes the pose.
    }
    else {
      const auto* s=state.replica.Find(o.kind,o.source);
      if (s && s->value==2) {
        if (o.raft_driven && Trace()) REXLOG_INFO("KK dev object raft authority withdrawn: role=guest id={:016X} generation={}",o.source,state.generation);
        o.raft_ready=o.raft_driven=false; continue;
      }
      if (s && !ApplyRaft(ctx,b,o,*s)) o.raft_driven=false;
    }
  }
}
void RaftFollow(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Raft) : nullptr;
#if defined(KK_DEV_TOOLS)
  if (o) o->raft_tick=state.ai_tick;
#endif
  if (o && !state.host && o->raft_driven) {
    // Preserve the native one-frame entry and pause/end transitions without
    // independently integrating movement or dereferencing an end-of-path target.
    if (Load32(b,o->vars)!=0) __imp__sub_8260C988(ctx,b);
    else if (Load32(b,o->vars+376)) {
      ctx.r3.u64=Load32(b,0x82CC8BF0); __imp__sub_826F9E70(ctx,b); ctx.r3.u64=5;
    } else ctx.r3.u64=0;
    return;
  }
  if (o && !state.host && RaftRoute(ctx,b,*o)) {
    const auto* s=state.replica.Find(o->kind,o->source);
    if (!s || (s->value!=2 && s->raft.route==o->route.signature)) {
      // Wait for the first exact pose, or let authority catch a local node that
      // was already ahead. Never race native integration against its snapshot.
      if (Load32(b,o->vars)!=0) __imp__sub_8260C988(ctx,b);
      else ctx.r3.u64=0;
      return;
    }
  }
  if (!state.ready && REXCVAR_GET(kk_coop_enabled) && SessionInProcess()) {
    const auto v=SessionFlowSnapshot();
    if (v.active && !v.host && v.policy==ChapterPolicy::Together) { ctx.r3.u64=0; return; }
  }
  __imp__sub_8260C988(ctx,b);
}
#if defined(KK_DEV_TOOLS)
void RaftWait(PPCContext& ctx,uint8_t* b) {
  if (auto* o=state.ready && Shared() ? Current(b,ObjectKind::Raft) : nullptr) o->raft_tick=state.ai_tick;
  __imp__sub_825E7348(ctx,b);
}
#endif
void DoorUpdate(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Door) : nullptr;
#if defined(KK_DEV_TOOLS)
  if (o) ++o->update_calls;
#endif
  const auto* s=o && !state.host ? state.replica.Find(o->kind,o->source) : nullptr;
  if (s) ApplyDoor(b,*o,*s);
  // Geometry, nav-grid, capability bits and completed-lever cleanup remain
  // native. Never copy transform or collision-component memory from the peer.
  if (s) { DoorStopGuard guard(*o,*s); __imp__sub_82552200(ctx,b); }
  else __imp__sub_82552200(ctx,b);
  if (s) o->driven_position=s->position;
#if defined(KK_DEV_TOOLS)
  if (s && s->position==1 && LoadF(b,o->vars+1084)==1 && Load32(b,o->vars+1096)==1) state.observed_change=true;
#endif
  if (o && state.host && Components(b,*o) && !DoorScheduled(b,o->ai)) Observe(b,*o);
}
void DoorStop(PPCContext& ctx,uint8_t* b) {
  // An authoritative closed fraction must not retire a still-active native
  // door merely because the guest's own predicate briefly wanted it open.
  // Keep all geometry/capability effects; hold only this exact door's stop call
  // until the host's snapshot says its track also ended.
  if (stop_actor && Load32(b,0x82CCBF6C)==stop_actor && Load32(b,0x82CCBF64)==stop_ai) return;
  __imp__sub_826F9DC0(ctx,b);
}
void DoorWait(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Door) : nullptr;
#if defined(KK_DEV_TOOLS)
  if (o) ++o->wait_calls;
#endif
  __imp__sub_82543478(ctx,b);
  if (o && Valid(b,*o)) {
    if (state.host) OpenHeldDoor(ctx,b,*o);
    else if (const auto* s=state.replica.Find(o->kind,o->source)) RefreshDoor(ctx,b,*o,*s);
  }
}
void DoorInit(PPCContext& ctx,uint8_t* b) {
#if defined(KK_DEV_TOOLS)
  if (auto* o=state.ready && Shared() ? Current(b,ObjectKind::Door) : nullptr) ++o->init_calls;
#endif
  __imp__sub_82574D80(ctx,b);
}
#if defined(KK_DEV_TOOLS)
void ScanInventory(const uint8_t* b,uint32_t w) {
  static const bool enabled=std::getenv("KK_DEV_OBJECTS_SCAN")!=nullptr;
  if (!enabled || state.scan_pass>=2 || Now()<state.scan_next) return;
  const auto v=SessionFlowSnapshot();
  if (!v.active || v.phase!=FlowPhase::Playing || w!=Load32(b,0x828E8388) || !PoseAddress(w,44)) return;
  const auto begin=Load32(b,w+40),end=Load32(b,w+28);
  if (!PoseAddress(begin) || !PoseAddress(end) || end<begin || (end-begin)%8 || end-begin>8*20000) return;
  ++state.scan_pass; state.scan_next=Now()+3;
  unsigned counts[13]{};
  constexpr const char* names[]{"unknown","iob","door","ode","tree","bamboo","rack","fire","raft","ammo","javelin","checkpoint","grab"};
  constexpr uint32_t offsets[13][3]{{},{552,556,560},{0,612,1096},{0,8,2444},{0x52C,0x7C4,0x930},
      {196,24,32},{192,196,228},{440,444,408},{204,224,644},{0,4,24},{0,232,536},{4,9540,9156},{24,20,84}};
  std::set<uint32_t> seen;
  for (auto p=begin;p<end;p+=8) {
    const auto actor=Load32(b,p);
    if (!PoseAddress(actor,32) || !seen.insert(actor).second) continue;
    const auto ai=Ptr(b,Ptr(b,actor,24,8),4,88),matrix=Ptr(b,actor,12,64);
    if (!ai || !matrix) continue;
    const auto first=Load16(b,ai+80),last=Load16(b,ai+82);
    if (first>last || last>4096 || last-first>64 || !PoseAddress(ai,(last+7)*4)) continue;
    unsigned kind=0; uint32_t selected=0;
    for (uint32_t t=first;t<=last;++t) {
      const auto block=Ptr(b,ai,(t+6)*4,24); if (!block) continue;
      const auto fn=Load32(b,block+20); unsigned next=0;
      switch (fn) {
        case 0x82507798: case 0x82516128: case 0x82515CF0: next=1; break;
        case 0x82551ED8: case 0x82574D80: case 0x8256C3F8: case 0x82543478: next=2; break;
        case 0x826EA1A0: case 0x826F0E10: case 0x826F1E08: case 0x826F4948: next=3; break;
        case 0x826E6390: case 0x826E78F0: case 0x826E7BF0: case 0x826D0770:
        case 0x826D0440: case 0x826D05D8: next=4; break; // ODE_BreakTree binding, separate from IG.
        case 0x826F88F0: case 0x826E58B8: case 0x826EAE60: case 0x826E4330: case 0x826DF6D8:
        case 0x826DE838: case 0x826DF778: case 0x826D59F8: next=12; break;
        case 0x8264EC58: case 0x8262A7A0: case 0x82644A60: next=5; break;
        case 0x8251C540: case 0x8251BA60: case 0x8251B800: next=6; break;
        case 0x825FAF58: case 0x8260F858: case 0x825F8CE8: next=7; break;
        case 0x825FD388: case 0x825E7348: case 0x8260C988: case 0x82604E68: next=8; break;
        case 0x824F35E0: case 0x824F3820: next=9; break;
        case 0x8257C560: case 0x8258E228: case 0x8258C5F8: case 0x825CE2F0: next=10; break;
        case 0x826437D0: next=11; break;
      }
      if (next) { kind=next; selected=fn; }
    }
    ++counts[kind];
    if (!kind || state.scan_rows>=96 || !state.scan_seen.insert(actor).second) continue;
    const auto definition=Ptr(b,Ptr(b,ai,0,4),0,8);
    const auto bytes=definition ? Load32(b,definition+4) : 0;
    const auto vars=bytes>=4 && bytes<=1024*1024 ? Ptr(b,ai,64,bytes) : 0;
    const auto word=[&](unsigned n) { return vars && offsets[kind][n]+4<=bytes ? Load32(b,vars+offsets[kind][n]) : UINT32_MAX; };
    ++state.scan_rows;
    const auto key=state.file_keys.find(actor);
    REXLOG_INFO("KK dev object inventory: role={} level={} family={} fn={:08X} actor={:08X} vars={:08X} bytes={} pos=({:.3f},{:.3f},{:.3f}) word+{}={:08X} word+{}={:08X} word+{}={:08X} asset={:08X}",
        v.host ? "host" : "guest",v.world.level,names[kind],selected,actor,vars,bytes,
        LoadF(b,matrix+48),LoadF(b,matrix+52),LoadF(b,matrix+56),offsets[kind][0],word(0),offsets[kind][1],word(1),offsets[kind][2],word(2),key==state.file_keys.end() ? 0 : key->second);
    if (kind==7 && vars && bytes>=884)
      REXLOG_INFO("KK dev object fire contract: role={} actor={:08X} supported={} mode={} repeat={} completion={:08X} timer={} spread={} total={}",
          v.host ? "host" : "guest",actor,FireContract(b,vars),Load32(b,vars+364),Load32(b,vars+372),
          Load32(b,vars+188),LoadF(b,vars+388),LoadF(b,vars+416),LoadF(b,vars+424));
    if (kind==3 && vars && bytes>=3376) {
      unsigned parts=0; bool terminated=false;
      for (unsigned i=0;i<=32;++i) {
        const auto part=Load32(b,vars+20+4*i);
        if (!part) { terminated=true; break; }
        if (i==32 || !PoseAddress(part,32)) break;
        ++parts;
      }
      REXLOG_INFO("KK dev object ode contract: role={} actor={:08X} mode={} game-flag={}/{} removal={} parts={} terminated={} bodies={:08X}/{:08X} watches={:08X}/{:08X}/{:08X}/{:08X}",
          v.host ? "host" : "guest",actor,Load32(b,vars),Load32(b,vars+4),Load32(b,vars+8),Load32(b,vars+2496),parts,terminated,
          Load32(b,vars+2500),Load32(b,vars+2504),Load32(b,vars+1704),Load32(b,vars+1888),Load32(b,vars+2072),Load32(b,vars+2256));
    }
    if (kind==4 && vars && bytes>=0xA20)
      REXLOG_INFO("KK dev object btode contract: role={} actor={:08X} mode={} body={:08X} fall={} watches={:08X}/{:08X}/{:08X}",
          v.host ? "host" : "guest",actor,Load32(b,vars+0x52C),Load32(b,vars+0x7C4),Load32(b,vars+0x930),
          Load32(b,vars+0x570),Load32(b,vars+0x628),Load32(b,vars+0x6E0));
  }
  REXLOG_INFO("KK dev object inventory totals: role={} level={} pass={} iob={} door={} ode={} tree={} bamboo={} rack={} fire={} raft={} ammo={} javelin={} checkpoint={} grab={} unknown={} bounded-rows={}",
      v.host ? "host" : "guest",v.world.level,state.scan_pass,counts[1],counts[2],counts[3],counts[4],counts[5],counts[6],counts[7],counts[8],counts[9],counts[10],counts[11],counts[12],counts[0],state.scan_rows);
}
// Explicit native-input fixtures, never a network outcome injector. Selects a
// real loaded object, moves only this process's Jack, then supplies a native
// firearm event or the interaction tag normally produced by the lever action.
// Acceptance must still be observed in each world's native damage/opening state.
struct Probe {
  double due=-1,act=0; uint64_t source=0; bool done=false;
  unsigned slot=0,hits=0;
  explicit Probe(const char* name) {
    const char* value=std::getenv(name); if (!value || !*value) return;
    char* end=nullptr; const auto t=std::strtod(value,&end);
    if (end!=value && !*end && std::isfinite(t) && t>=0 && t<=3600) due=t;
  }
};

void DevProbe(PPCContext& ctx,uint8_t* b,Probe& probe,ObjectKind kind) {
  const auto t=kk::DevSecondsSinceMenu();
  if (probe.done || probe.due<0 || t<probe.due) return;
  if (t>probe.due+20) { probe.done=true; REXLOG_WARN("KK dev object probe: expired kind={} no eligible object/native player",uint8_t(kind)); return; }
  const auto hero=Load32(b,0x82CCBF7C),ai=Ptr(b,Ptr(b,hero,24,8),4,88),hm=Ptr(b,hero,12,64);
  if (!hm || !Track(b,ai,0x826718B8)) return;
  if (!probe.source) {
    Binding* best=nullptr; float distance=1000000; unsigned best_slot=0;
    for (auto& [actor,o] : state.bindings) {
      if (o.kind!=kind || !Valid(b,o) || Match(o.source,kind)!=&o) continue;
      if (kind==ObjectKind::Breakable && (!Load32(b,o.vars+556) || Load32(b,o.vars+552)>=Load32(b,o.vars+556))) continue;
      unsigned slot=0;
      if (kind==ObjectKind::Door) {
        if (Load32(b,o.vars)!=2 || LoadF(b,o.vars+1084)>=1) continue;
        for (;slot<8;++slot) { const auto ext=Lever(b,o,slot); if (ext && (Load16(b,ext+40)&1)) break; }
        if (slot==8) continue;
      }
      const auto marker=kind==ObjectKind::Door ? Ptr(b,Load32(b,o.vars+640+4*slot),12,64) : o.matrix;
      if (!marker || std::abs(LoadF(b,marker+56)-LoadF(b,hm+56))>2) continue;
      const float x=LoadF(b,marker+48),y=LoadF(b,marker+52);
      if (!std::isfinite(x) || !std::isfinite(y)) continue;
      const float d=std::hypot(x-LoadF(b,hm+48),y-LoadF(b,hm+52));
      if (d<distance) { distance=d; best=&o; best_slot=slot; }
    }
    if (!best) return;
    auto matrix=best->matrix;
    if (kind==ObjectKind::Door) matrix=Ptr(b,Load32(b,best->vars+640+4*best_slot),12,64);
    if (!matrix) return;
    // A marker high above/below the current walkable area is not a demonstrated
    // placement. Do not repeat the chapter-3 probe's off-cliff teleport.
    if (std::abs(LoadF(b,matrix+56)-LoadF(b,hm+56))>2) return;
    float fx=LoadF(b,matrix+48)-LoadF(b,hm+48),fy=LoadF(b,matrix+52)-LoadF(b,hm+52);
    const auto length=std::hypot(fx,fy); fx=length>.01f ? fx/length : 0; fy=length>.01f ? fy/length : 1;
    const float gap=kind==ObjectKind::Door ? 1.f : 3.f;
    const ShotVector position{LoadF(b,matrix+48)-gap*fx,LoadF(b,matrix+52)-gap*fy,LoadF(b,hm+56)};
    if (!kk::DevTeleportJack(ctx,b,hero,position,fx,fy)) return;
    probe.source=best->source; probe.act=t+1; probe.slot=best_slot;
    if (state.debug_sources.size()<8) state.debug_sources.insert(probe.source);
    REXLOG_INFO("KK dev object probe: placed role={} kind={} id={:016X} at=({:.3f},{:.3f},{:.3f}) native-input-in=1s collision-unverified=true",
        state.host ? "host" : "guest",uint8_t(kind),best->source,position.x,position.y,position.z);
    return;
  }
  if (t<probe.act) return;
  auto* o=Match(probe.source,kind); if (!o || !Valid(b,*o)) { probe.done=true; return; }
  bool supplied=false;
  if (kind==ObjectKind::Breakable) {
    static const auto magnitude=[] {
      const char* value=std::getenv("KK_DEV_OBJECT_HIT_MAGNITUDE");
      if (!value) return 1000u;
      char* end=nullptr; const auto n=std::strtoul(value,&end,10);
      return end!=value && !*end && n<=100000 ? uint32_t(n) : 1000u;
    }();
    supplied=NativeHit(ctx,b,*o,hero,ai,64,magnitude);
  }
  else if (const auto ext=Lever(b,*o,probe.slot)) { Store16(b,ext+40,Load16(b,ext+40)|0x8000); supplied=true; }
  if (!supplied) return;
  ++probe.hits;
  const char* repeat=kind==ObjectKind::Breakable ? std::getenv("KK_DEV_OBJECT_HIT_COUNT") : nullptr;
  const auto count=repeat ? std::clamp(std::atoi(repeat),1,12) : 1;
  probe.done=probe.hits>=unsigned(count); probe.act=t+1;
  REXLOG_INFO("KK dev object probe: native input role={} kind={} id={:016X} supplied={} native-result-awaiting=true",
      state.host ? "host" : "guest",uint8_t(kind),o->source,supplied);
}
void Probes(PPCContext& ctx,uint8_t* b) {
  static Probe hit("KK_DEV_OBJECT_HIT"),lever("KK_DEV_OBJECT_LEVER");
  DevProbe(ctx,b,hit,ObjectKind::Breakable); DevProbe(ctx,b,lever,ObjectKind::Door);
  static Probe fire("KK_DEV_OBJECT_FIRE"),off("KK_DEV_OBJECT_EXTINGUISH");
  const auto t=kk::DevSecondsSinceMenu();
  for (auto* probe : {&fire,&off}) {
    if (probe->done || probe->due<0 || t<probe->due) continue;
    for (auto& [actor,o] : state.bindings) {
      if (o.kind!=ObjectKind::Fire || !Valid(b,o) || Match(o.source,o.kind)!=&o || !FireContract(b,o.vars)) continue;
      const auto value=Sample(b,o);
      if (probe==&off) {
        if (!state.host || value.value!=1) continue;
        StoreF(b,o.vars+388,0); // Dev input: native Burn owns cleanup and removal.
      } else {
        if (value.value!=0) continue;
        if (state.host) o.fire_request=true;
        else {
          const auto m=Ptr(b,Load32(b,0x82CCBF7C),12,64);
          if (!m || std::hypot(LoadF(b,m+48)-o.x,LoadF(b,m+52)-o.y,LoadF(b,m+56)-o.z)>5) continue;
          if (!Request(o,1)) continue;
        }
      }
      probe->done=true;
      REXLOG_INFO("KK dev object fire fixture: role={} id={:016X} operation={} awaiting-native-result=true",
          state.host ? "host" : "guest",o.source,probe==&off ? "expire" : "ignite");
      break;
    }
    if (!probe->done && t>probe->due+20) { probe->done=true; REXLOG_WARN("KK dev object fire fixture: no eligible static fire"); }
  }
  static Probe raft("KK_DEV_OBJECT_RAFT");
  if (!raft.done && raft.due>=0 && t>=raft.due) {
    for (auto& [actor,o] : state.bindings) {
      if (o.kind!=ObjectKind::Raft || !Valid(b,o) || Match(o.source,o.kind)!=&o ||
          !o.raft_ready || o.raft.next==255) continue;
      const auto p=NativePoint(ctx,b,o.actor);
      const auto setting=std::getenv("KK_DEV_OBJECT_RAFT_BOARD");
      const bool board=!setting || std::strcmp(setting,"0")!=0;
      if (!p || (board && !kk::DevTeleportJack(ctx,b,Load32(b,0x82CCBF7C),{p->x,p->y,p->z+2},1,0))) continue;
      const bool endpoint=std::getenv("KK_DEV_OBJECT_RAFT_END")!=nullptr;
      if (state.host && endpoint) {
        if (o.route.nodes.size()<2) continue;
        const auto end=NativePoint(ctx,b,o.route.nodes.back()); if (!end) continue;
        auto s=Sample(b,o); s.value=0;
        s.raft.current=uint8_t(o.route.nodes.size()-2); s.raft.next=uint8_t(o.route.nodes.size()-1);
        s.raft.pose[0]=end->x; s.raft.pose[1]=end->y; s.raft.pose[2]=end->z;
        s.raft.pose[9]=s.raft.pose[10]=s.raft.pose[11]=0;
        if (!ApplyRaft(ctx,b,o,s)) continue;
      }
      if (state.host) Store32(b,o.vars+376,0); // Original Follow owns arrival and stop.
      if (state.host && !board) state.raft_fixture=o.source;
      raft.done=true;
      REXLOG_INFO("KK dev object raft fixture: role={} id={:016X} board={} endpoint={} target=({:.3f},{:.3f},{:.3f}) native-resume={}",
          state.host ? "host" : "guest",o.source,board,endpoint,p->x,p->y,p->z+2,state.host);
      break;
    }
    if (!raft.done && t>raft.due+20) { raft.done=true; REXLOG_WARN("KK dev object raft fixture: no eligible initialized route with a next node"); }
  }
  static Probe withdraw("KK_DEV_OBJECT_RAFT_WITHDRAW");
  if (state.host && state.raft_fixture && !withdraw.done && withdraw.due>=0 && t>=withdraw.due) {
    withdraw.done=true; state.raft_withdraw_until=Now()+3;
    REXLOG_INFO("KK dev object raft withdrawal fixture: id={:016X} unsupported-for-seconds=3",state.raft_fixture);
  }
}
void TickRaftFixture(PPCContext& ctx,uint8_t* b) {
  if (!state.host || !state.raft_fixture) return;
  for (auto& [actor,o] : state.bindings) {
    if (o.source!=state.raft_fixture || o.kind!=ObjectKind::Raft || !Valid(b,o) ||
        Match(o.source,o.kind)!=&o || !o.raft_ready || o.raft.next==255 || o.raft_tick==state.ai_tick) continue;
    // Explicit developer activation only, for a dormant authored boat. Tick its
    // own native wait/follow slot once; never invent its movement or rider refs.
    const auto first=Load16(b,o.ai+80),last=Load16(b,o.ai+82);
    if (first>last || last>4096 || last-first>64 || !PoseAddress(o.ai,(last+7)*4)) continue;
    for (uint32_t slot=first;slot<=last;++slot) {
      const auto block=Ptr(b,o.ai,(slot+6)*4,24); if (!block) continue;
      const auto fn=Load32(b,block+20); if (fn!=0x825E7348 && fn!=0x8260C988) continue;
      const auto sp=ctx.r1.u32;
      if ((sp&15) || sp<0x40014000 || sp>=0xA0000000) return;
      uint8_t scratch[16384]; const auto start=sp-sizeof(scratch); const PPCContext saved=ctx;
      std::memcpy(scratch,b+start,sizeof(scratch));
      constexpr uint32_t globals[]{0x82CCBF58,0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
      uint32_t values[7]; for (unsigned n=0;n<7;++n) values[n]=Load32(b,globals[n]);
      ctx.r1.u64=start+12288; std::memset(b+start+12288,0,4096); Store32(b,start+12288,sp);
      Store32(b,globals[0],slot); Store32(b,globals[3],o.ai); Store32(b,globals[4],o.ai);
      Store32(b,globals[5],o.actor); Store32(b,globals[6],o.actor);
      ctx.r3.u64=block; ctx.r4.u64=o.ai+(slot+1)*4; ctx.r5.u64=0;
      __imp__sub_8271CCB8(ctx,b);
      for (unsigned n=0;n<7;++n) Store32(b,globals[n],values[n]);
      std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved;
      return;
    }
  }
}
void ReloadProbe(uint8_t* b) {
  static Probe probe("KK_DEV_OBJECT_RELOAD");
  const auto t=kk::DevSecondsSinceMenu();
  if (probe.done || probe.due<0 || state.host || t<probe.due) return;
  if (t>=probe.act) {
    probe.act=t+2;
    REXLOG_INFO("KK dev object reload probe: generation={} observed-native-change={}",state.generation,state.observed_change);
  }
  if (!state.observed_change) return;
  if (SessionDevObjectReload(b)) probe.done=true;
}
void PickupProbe(PPCContext& ctx,uint8_t* b) {
  static Probe probe("KK_DEV_OBJECT_PICKUP");
  const auto t=kk::DevSecondsSinceMenu();
  if (probe.done || probe.due<0 || t<probe.due) return;
  if (t>probe.due+20) { probe.done=true; REXLOG_WARN("KK dev object pickup: expired, no eligible ammo/spear rack"); return; }
  const auto hero=Load32(b,0x82CCBF7C),ai=Ptr(b,Ptr(b,hero,24,8),4,88),hm=Ptr(b,hero,12,64);
  if (!hm || !Track(b,ai,0x826718B8)) return;
  State::Pickup* best=nullptr; float distance=1000000;
  for (auto& [actor,p] : state.pickups) {
    if (probe.source && p.source!=probe.source) continue;
    if (Ptr(b,Ptr(b,actor,24,8),4,88)!=p.ai || Ptr(b,p.ai,64,p.rack ? 256 : 32)!=p.vars || Ptr(b,actor,12,64)!=p.matrix ||
        !Track(b,p.ai,p.rack ? 0x8251BA60 : 0x824F3820) || !Load32(b,p.vars+(p.rack ? 196 : 4))) continue;
    unsigned matches=0; for (const auto& [other,q] : state.pickups) matches+=q.source==p.source;
    if (matches!=1) continue;
    const auto d=std::hypot(std::hypot(LoadF(b,p.matrix+48)-LoadF(b,hm+48),LoadF(b,p.matrix+52)-LoadF(b,hm+52)),LoadF(b,p.matrix+56)-LoadF(b,hm+56));
    if (d<distance) { distance=d; best=&p; }
  }
  if (!best) return;
  if (!probe.source) {
    const ShotVector position{LoadF(b,best->matrix+48),LoadF(b,best->matrix+52)-1,LoadF(b,hm+56)};
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        std::abs(LoadF(b,best->matrix+56)-position.z)>2) return;
    if (!kk::DevTeleportJack(ctx,b,hero,position,0,1)) return;
    probe.source=best->source; probe.act=t+1; state.debug_sources.insert(probe.source);
    REXLOG_INFO("KK dev object pickup: placed role={} id={:016X} pos=({:.3f},{:.3f},{:.3f}) count={} type={} family={}",
        state.host ? "host" : "guest",best->source,position.x,position.y,position.z,
        Load32(b,best->vars+(best->rack ? 196 : 4)),Load32(b,best->vars+(best->rack ? 192 : 0)),best->rack ? "rack" : "ammo");
    return;
  }
  if (t<probe.act) return;
  const auto ext=Ptr(b,best->actor,24,42); if (!ext) return;
  Store16(b,ext+40,Load16(b,ext+40)|0x8000); probe.done=true;
  REXLOG_INFO("KK dev object pickup: native interaction tag role={} id={:016X} count={} inventory-result-awaiting=true",
      state.host ? "host" : "guest",best->source,Load32(b,best->vars+(best->rack ? 196 : 4)));
}
#endif
} // namespace
void ObjectsPad(uint32_t user,const uint8_t* pad,bool connected) {
  if (user) return;
  use_until.store(connected && pad && pad[7]>30 && pad[6]<=30 ? Now()+.2 : 0,std::memory_order_relaxed);
}
void ObjectsFirearm(const PPCContext& input,const uint8_t* b) {
  if (!Trace() || !state.ready || !Shared() || input.r6.u32!=Load32(b,0x82CCBF7C)) return;
  const auto caller=uint32_t(input.lr);
  if (caller!=0x82696E74 && caller!=0x82697494 && caller!=0x82697970 && caller!=0x82698C00) return;
  const Binding* target=nullptr; bool part=false;
  for (const auto& [actor,o] : state.bindings) if (o.kind==ObjectKind::Breakable && Valid(b,o)) {
    bool hit=actor==input.r8.u32;
    for (unsigned i=0;i<10 && !hit;++i) hit=Load32(b,o.vars+20+4*i)==input.r8.u32 && input.r8.u32!=0;
    if (hit) { if (target) return; target=&o; part=actor!=input.r8.u32; }
  }
  if (!target) return;
  REXLOG_INFO("KK dev object firearm: role={} id={:016X} target={:08X} part={} weapon-caller={:08X} flags={:08X} magnitude={}",
      state.host ? "host" : "guest",target->source,input.r8.u32,part,caller,input.r5.u32,input.r10.u32);
}
ObjectEnqueue ObjectsBeginEnqueue(const PPCContext& input,const uint8_t* b) {
  if (input.r4.u32!=4 || !state.ready || input.r3.u32!=Load32(b,0x82CCBF7C) ||
      !Shared() || !PoseAddress(input.r8.u32,8)) return {};
  return {input.r4.u32,input.r3.u32,input.r6.u32,input.r8.u32,uint32_t(input.lr),float(input.f1.f64),
      uint64_t(Load32(b,input.r8.u32))<<32|Load32(b,input.r8.u32+4)};
}
void ObjectsEnqueued(const ObjectEnqueue& input,const uint8_t* b) {
  if (input.type!=4 || !state.ready || !Shared() || !PoseAddress(input.payload,108)) return;
  const auto payload=input.payload,hero=Load32(b,0x82CCBF7C);
  if (input.source!=hero) return;
  if (!objects::StrikeRecord([&](uint32_t address){ return Load32(b,address); },input.output,input.previous,
      hero,Load32(b,payload+4),Load32(b,payload+88),Load32(b,payload+92),input.seconds)) return;
  const auto ai=Ptr(b,Ptr(b,hero,24,8),4,88);
  if (!Track(b,ai,0x826718B8)) return;
  const auto it=state.bindings.find(Load32(b,payload+4));
  if (it==state.bindings.end()) return;
  const auto& o=it->second;
  if (o.kind!=ObjectKind::Breakable || !Valid(b,o) || Match(o.source,o.kind)!=&o) return;
  const auto flags=Load32(b,payload+88),magnitude=Load32(b,payload+92);
  if ((flags&~kObjectHitFlags) || magnitude>100000 || !std::isfinite(input.seconds) || input.seconds<0 || input.seconds>2) return;
  if (!state.host) Request(o,flags,float(magnitude),input.seconds);
  if (Trace()) REXLOG_INFO("KK dev object raw hit: role={} id={:016X} handle={:08X}/{} flags={:08X} magnitude={} lifetime={:.3f} caller={:08X}",
      state.host ? "host" : "guest",o.source,Load32(b,input.output),Load32(b,input.output+4),flags,magnitude,input.seconds,input.caller);
}

void ObjectsLoaded(uint8_t* b) {
  if (!REXCVAR_GET(kk_coop_enabled) || !SessionInProcess()) { state={}; return; }
  const auto v=SessionFlowSnapshot(); const auto epoch=StorySessionEpoch();
  if (!v.active || v.policy!=ChapterPolicy::Together || !epoch || !v.world.generation || !v.world.level) { state={}; return; }
  const bool same=state.epoch==epoch && state.generation==v.world.generation && state.level==v.world.level;
  // Preserve absolute host outcomes/replay cursors across a private guest load.
  // Discard every native address before rebinding to the newly loaded registry.
  if (!same) state={};
  state.epoch=epoch; state.generation=v.world.generation; state.level=v.world.level; state.host=v.host;
  state.file_keys=objects::FileKeys([&](uint32_t address){ return Load32(b,address); });
  state.bindings.clear(); state.retired.clear(); state.pending.clear(); state.outgoing.clear(); state.ready=false;
  state.identities.Clear();
  state.census_started=false;
#if defined(KK_DEV_TOOLS)
  state.pickups.clear(); state.debug_sources.clear();
  state.scan_pass=state.scan_rows=0; state.scan_next=0; state.scan_seen.clear();
#endif
  state.census_until=Now()+10; state.next_census=state.next_full=state.next_sample=0;
  Census(b);
  REXLOG_INFO("KK co-op objects: baseline role={} generation={} level={} objects={} private={}",state.host ? "host" : "guest",state.generation,state.level,state.bindings.size(),same);
}
void ObjectsBeforeAi(PPCContext& ctx,uint8_t* b,uint32_t w) {
  state.ready=false;
  if (uint32_t(ctx.lr)!=0x82881CF0) return;
#if defined(KK_DEV_TOOLS)
  ScanInventory(b,w);
#endif
  if (!Shared() || w!=Load32(b,0x828E8388)) {
    state.outgoing.clear();
    for (auto& [actor,o] : state.bindings) {
      if (o.local_levers && w==state.world && Valid(b,o)) FinishLocalLever(b,o,o.local_levers);
      o.pending_hits.clear(); o.local_levers=o.sent_holds=o.observed_hold=0;
      o.local_holds={}; o.remote_holds={};
    }
    return;
  }
  // The partner may spend longer than ten seconds behind the load barrier.
  // Cached authored identities stay fixed; admit still-initializing bindings
  // for ten seconds of actual shared AI, rather than expiring while waiting.
  if (!state.census_started) { state.census_started=true; state.census_until=Now()+10; state.next_census=0; }
  if (Now()>=state.next_census) Census(b);
  state.ready=true; ++state.ai_tick; Receive(b); Holds(b); Flush();
#if defined(KK_DEV_TOOLS)
  Probes(ctx,b);
  PickupProbe(ctx,b);
#endif
}
void ObjectsAfterAi(PPCContext& ctx,uint8_t* b,uint32_t w) {
  if (!state.ready || w!=state.world || !Shared()) return;
  ReplayBreakables(ctx,b);
  IdleDoors(ctx,b);
  ReplayFires(ctx,b);
#if defined(KK_DEV_TOOLS)
  TickRaftFixture(ctx,b);
#endif
  UpdateRafts(ctx,b);
  const auto now=Now();
#if defined(KK_DEV_TOOLS)
  if (Trace() && std::getenv("KK_DEV_OBJECT_PASSAGE")) {
    const auto matrix=Ptr(b,Load32(b,0x82CCBF7C),12,64);
    if (matrix) for (auto& [source,p] : state.planes) {
      if (!state.identities.Allows(ObjectKind::Door,source) || now<p.next) continue;
      const float x=LoadF(b,matrix+48),y=LoadF(b,matrix+52),z=LoadF(b,matrix+56);
      if (std::hypot(x-p.x,y-p.y,z-p.z)>20) continue;
      p.next=now+.1;
      const auto it=state.outcomes.find(source);
      const auto* s=state.host ? (it==state.outcomes.end() ? nullptr : &it->second) : state.replica.Find(ObjectKind::Door,source);
      REXLOG_INFO("KK dev object passage: role={} id={:016X} pos=({:.3f},{:.3f},{:.3f}) plane=({:.3f},{:.3f},{:.3f}) normal=({:.3f},{:.3f},{:.3f}) signed={:.3f} authority-fraction={} condition={}",
          state.host ? "host" : "guest",source,x,y,z,p.x,p.y,p.z,p.nx,p.ny,p.nz,
          (x-p.x)*p.nx+(y-p.y)*p.ny+(z-p.z)*p.nz,s ? s->position : -1,int(LocalPlayerCondition(b).value_or(PlayerCondition::Dead)));
    }
  }
#endif
  if (state.host && now>=state.next_sample) {
    state.next_sample=now+.1; const bool full=now>=state.next_full;
    if (full) state.next_full=now+2;
    for (auto& [actor,o] : state.bindings) {
      if (!Valid(b,o) || Match(o.source,o.kind)!=&o) continue;
      Observe(b,o);
    }
    if (full) for (auto& [source,s] : state.outcomes) {
      if (state.number==UINT32_MAX) break;
      s.number=++state.number; state.pending[source]=s;
    }
  }
  Flush();
#if defined(KK_DEV_TOOLS)
  ReloadProbe(b);
#endif
  if (now>=state.next_log) {
    state.next_log=now+5;
    unsigned breakables=0,doors=0;
    for (auto& [actor,o] : state.bindings) { breakables+=o.kind==ObjectKind::Breakable; doors+=o.kind==ObjectKind::Door; }
    REXLOG_INFO("KK co-op objects: role={} generation={} level={} breakables={} doors={} requests={} accepted={} applied={} received={} refused={} pending={}",
        state.host ? "host" : "guest",state.generation,state.level,breakables,doors,state.requested,state.accepted,state.applied,state.received,state.rejected,state.pending.size()+state.outgoing.size());
#if defined(KK_DEV_TOOLS)
    unsigned details=0;
    if (Trace()) {
      const auto hero=Load32(b,0x82CCBF7C),matrix=Ptr(b,hero,12,64);
      const auto human=Ptr(b,Ptr(b,Ptr(b,hero,24,8),4,88),64,13388);
      const auto game=Load32(b,0x82CC97C0); // actual cheats/firearm inventory cache
      if (matrix && human && PoseAddress(game,13652))
        REXLOG_INFO("KK dev object player: role={} pos=({:.3f},{:.3f},{:.3f}) mode={} paired={:08X} marker={:08X} weapon={} ammo={}/{}/{}/{} facing={:.3f}/{:.3f} use={} cutscene={} condition={}",
            state.host ? "host" : "guest",LoadF(b,matrix+48),LoadF(b,matrix+52),LoadF(b,matrix+56),Load32(b,human+2712),
            Load32(b,human+13376),Load32(b,human+13380),Load32(b,game+13128),Load32(b,game+13432),Load32(b,game+13504),Load32(b,game+13576),Load32(b,game+13648),
            -LoadF(b,matrix+16),-LoadF(b,matrix+20),use_until.load(std::memory_order_relaxed)>=now,observer::InCutscene(b),int(LocalPlayerCondition(b).value_or(PlayerCondition::Dead)));
    }
    if (Trace()) for (auto& [actor,o] : state.bindings) if (Valid(b,o) && Match(o.source,o.kind)==&o) {
      if (details++>=32 && !state.debug_sources.contains(o.source)) continue;
      const auto local=Sample(b,o); const auto* authority=state.replica.Find(o.kind,o.source);
      REXLOG_INFO("KK dev object state: role={} id={:016X} kind={} local={}/{:.3f}/{:.3f} host={}/{:.3f} matched={}",
          state.host ? "host" : "guest",o.source,uint8_t(o.kind),local.value,local.position,local.seconds,
          authority ? authority->value : local.value,authority ? authority->position : local.position,state.host || authority!=nullptr);
      if (o.kind==ObjectKind::Door) REXLOG_INFO("KK dev object door callbacks: role={} id={:016X} init={} wait={} loop={} update={} holds={} context-misses={} dt={} duration={} fraction={} endpoint={}",
          state.host ? "host" : "guest",o.source,o.init_calls,o.wait_calls,o.loop_calls,o.update_calls,
          o.hold_calls,o.context_misses,LoadF(b,0x82C636E0),LoadF(b,o.vars+1088),LoadF(b,o.vars+1084),Load32(b,o.vars+1096));
      if (o.kind==ObjectKind::Door) REXLOG_INFO("KK dev object door geometry: role={} id={:016X} translate={} rotate={} target={:08X} pos=({:.3f},{:.3f},{:.3f}) normal=({:.3f},{:.3f},{:.3f})",
          state.host ? "host" : "guest",o.source,Load32(b,o.vars+560),Load32(b,o.vars+564),Load32(b,o.vars+364),
          LoadF(b,o.matrix+48),LoadF(b,o.matrix+52),LoadF(b,o.matrix+56),
          -LoadF(b,o.matrix+16),-LoadF(b,o.matrix+20),-LoadF(b,o.matrix+24));
      if (o.kind==ObjectKind::Door) {
        if (const auto p=NativePoint(ctx,b,o.actor))
          REXLOG_INFO("KK dev object door world: role={} id={:016X} type={:08X} point=({:.3f},{:.3f},{:.3f})",
              state.host ? "host" : "guest",o.source,Load32(b,o.actor),p->x,p->y,p->z);
        for (unsigned i=0;i<4;++i) if (Lever(b,o,i)) {
          const auto actor=Load32(b,o.vars+640+4*i);
          if (const auto p=NativePoint(ctx,b,actor))
            REXLOG_INFO("KK dev object marker world: role={} id={:016X} slot={} type={:08X} point=({:.3f},{:.3f},{:.3f})",
                state.host ? "host" : "guest",o.source,i,Load32(b,actor),p->x,p->y,p->z);
        }
      }
      if (o.kind==ObjectKind::Breakable) REXLOG_INFO("KK dev object breakable consumer: role={} id={:016X} maximum={} init={} go={} consumed={}",
          state.host ? "host" : "guest",o.source,Load32(b,o.vars+556),Track(b,o.ai,0x82516128),Track(b,o.ai,0x82515CF0),o.damage_tick==state.ai_tick);
      if (o.kind==ObjectKind::Raft) {
        const auto p=NativePoint(ctx,b,o.actor);
        REXLOG_INFO("KK dev object raft state: role={} id={:016X} ready={} driven={} nodes={} route={:016X} current={}/{} native-source={:08X} native-key={:08X} native-current={:08X}/{:08X} pos=({:.3f},{:.3f},{:.3f}) host=({:.3f},{:.3f},{:.3f}) speed={} time={} stop={}",
            state.host ? "host" : "guest",o.source,o.raft_ready,o.raft_driven,o.route.nodes.size(),o.raft.route,o.raft.current,o.raft.next,
            Load32(b,o.vars+204),Load32(b,o.vars+224),Load32(b,o.vars+240),Load32(b,o.vars+244),
            p ? p->x : 0,p ? p->y : 0,p ? p->z : 0,
            authority ? authority->raft.pose[0] : o.raft.pose[0],authority ? authority->raft.pose[1] : o.raft.pose[1],authority ? authority->raft.pose[2] : o.raft.pose[2],
            LoadF(b,o.vars+420),LoadF(b,o.vars+4),Load32(b,o.vars+376));
      }
      if (o.kind==ObjectKind::Door && !o.lever_logged) {
        bool logged=false;
        for (unsigned i=0;i<8;++i) if (const auto ext=Lever(b,o,i)) {
          const auto m=Ptr(b,Load32(b,o.vars+640+4*i),12,64);
          if (!m) continue;
          REXLOG_INFO("KK dev object lever: role={} id={:016X} slot={} flags={:04X} marker=({:.3f},{:.3f},{:.3f}) installed={} one-way={} close-on-zero={} native-duration={}",
              state.host ? "host" : "guest",o.source,i,Load16(b,ext+40),LoadF(b,m+48),LoadF(b,m+52),LoadF(b,m+56),
              bool(Load16(b,ext+40)&1),Load32(b,o.vars+568),Load32(b,o.vars+572),LoadF(b,o.vars+1088));
          logged=true;
        }
        o.lever_logged=logged;
      }
    }
    details=0;
    if (Trace()) for (const auto& [actor,p] : state.pickups) {
      if (details++>=16 && !state.debug_sources.contains(p.source)) continue;
      if (Ptr(b,Ptr(b,actor,24,8),4,88)!=p.ai || Ptr(b,p.ai,64,p.rack ? 256 : 32)!=p.vars) continue;
      REXLOG_INFO("KK dev object pickup state: role={} id={:016X} count={} pending={} type={} family={}",
          state.host ? "host" : "guest",p.source,Load32(b,p.vars+(p.rack ? 196 : 4)),
          Load32(b,p.vars+(p.rack ? 228 : 24)),Load32(b,p.vars+(p.rack ? 192 : 0)),p.rack ? "rack" : "ammo");
    }
#endif
  }
}
} // namespace kk::coop
REX_HOOK_RAW(sub_824ADD48) { kk::coop::Damage(ctx,base); }
REX_HOOK_RAW(sub_8256C3F8) { kk::coop::Door(ctx,base); }
REX_HOOK_RAW(sub_82552200) { kk::coop::DoorUpdate(ctx,base); }
REX_HOOK_RAW(sub_82515CF0) { kk::coop::Breakable(ctx,base); }
REX_HOOK_RAW(sub_82516128) { kk::coop::BreakableInit(ctx,base); }
REX_HOOK_RAW(sub_82543478) { kk::coop::DoorWait(ctx,base); }
REX_HOOK_RAW(sub_82574D80) { kk::coop::DoorInit(ctx,base); }
REX_HOOK_RAW(sub_826F9DC0) { kk::coop::DoorStop(ctx,base); }
REX_HOOK_RAW(sub_825FAF58) { kk::coop::FireInit(ctx,base); }
REX_HOOK_RAW(sub_8260F858) { kk::coop::FireBurn(ctx,base); }
REX_HOOK_RAW(sub_8260C988) { kk::coop::RaftFollow(ctx,base); }
#if defined(KK_DEV_TOOLS)
REX_HOOK_RAW(sub_825E7348) { kk::coop::RaftWait(ctx,base); }
#endif
