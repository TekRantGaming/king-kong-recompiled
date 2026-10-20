// C71: semantic native breakable/door state only. Pickup families deliberately
// remain independent. All memory and native calls stay on the AI thread.
#include "coop_objects_game.h"
#include "coop_objects.h"
#include "coop_runtime.h"
#include "coop_pose.h"
#include "hfr.h"
#include "settings.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>

REX_EXTERN(__imp__sub_824ADD48);
REX_EXTERN(__imp__sub_8256C3F8);
REX_EXTERN(__imp__sub_82552200);
REX_EXTERN(__imp__sub_82515CF0);
REX_EXTERN(__imp__sub_82543478);
REX_EXTERN(__imp__sub_826F9DC0);
#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_82634428);
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
  float x=0,y=0,z=0;
  uint32_t pending_damage=0,pending_levers=0;
  uint32_t local_levers=0;
  double pending_until=0;
  double local_lever_until=0;
  double local_hit_until=0;
  float driven_position=-1;
  ObjectState last;
  bool sent=false;
};
struct State {
  uint64_t epoch=0;
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
  struct Pickup { uint32_t actor=0,ai=0,vars=0,matrix=0; uint64_t source=0; bool rack=false; };
  std::map<uint32_t,Pickup> pickups;
  std::set<uint64_t> debug_sources;
  unsigned scan_pass=0,scan_rows=0;
  double scan_next=0;
  std::set<uint32_t> scan_seen;
#endif
} state;
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
  return ObjectKind(0);
}
bool DoorScheduled(const uint8_t* b,uint32_t ai) {
  return Track(b,ai,0x8256C3F8) || Track(b,ai,0x82543478) || Track(b,ai,0x82574D80);
}
uint32_t Vars(const uint8_t* b,uint32_t ai,ObjectKind kind) {
  const uint32_t needed=kind==ObjectKind::Breakable ? 580 : 1340;
  const auto definition=Ptr(b,Ptr(b,ai,0,4),0,8);
  if (!definition || Load32(b,definition+4)<needed || Load32(b,definition+4)>1024*1024) return 0;
  return Ptr(b,ai,64,needed);
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
      if (!Valid(b,it->second)) { state.retired.insert(actor); state.bindings.erase(it); }
      continue; // Component reuse never acquires an old authored identity.
    }
    if (state.retired.contains(actor) || Now()>state.census_until || state.bindings.size()>=objects::kCensus) continue;
    const auto ai=Ptr(b,Ptr(b,actor,24,8),4,88),matrix=Ptr(b,actor,12,64);
    const auto kind=Family(b,ai);
    if (!matrix || uint8_t(kind)==0) continue;
    const auto vars=Vars(b,ai,kind); if (!vars) continue;
    Binding o; o.actor=actor; o.ai=ai; o.vars=vars; o.matrix=matrix; o.kind=kind;
    o.x=LoadF(b,matrix+48); o.y=LoadF(b,matrix+52); o.z=LoadF(b,matrix+56);
    const auto asset=state.file_keys.find(actor);
    o.source=asset==state.file_keys.end() ? objects::Source(kind,o.x,o.y,o.z,uint8_t(kind)) : objects::AssetSource(kind,asset->second);
    if (o.source) {
      state.identities.Record(o);
      state.bindings.emplace(actor,o);
      if (Trace()) REXLOG_INFO("KK dev object: role={} kind={} id={:016X} actor={:08X} vars={:08X} pos=({:.3f},{:.3f},{:.3f}) damage={} maximum={} levers={} type={} asset={:08X}",
          state.host ? "host" : "guest",uint8_t(kind),o.source,actor,vars,o.x,o.y,o.z,
          kind==ObjectKind::Breakable ? Load32(b,vars+552) : 0,kind==ObjectKind::Breakable ? Load32(b,vars+556) : 0,
          kind==ObjectKind::Door ? Load32(b,vars+612) : 0,Load32(b,vars),asset==state.file_keys.end() ? 0 : asset->second);
    }
  }
  // Keep terminal breakable bindings as tombstones while still registered.
  // Their final cumulative state is resent, including to a privately reloaded guest.
  std::erase_if(state.bindings,[&](const auto& entry){
    if (members.contains(entry.first)) return false;
    state.retired.insert(entry.first); return true;
  });
#if defined(KK_DEV_TOOLS)
  std::erase_if(state.pickups,[&](const auto& entry){ return !members.contains(entry.first); });
#endif
  state.world=w; state.next_census=Now()+1;
}
uint32_t Lever(const uint8_t* b,const Binding& o,unsigned slot) {
  const auto count=Load32(b,o.vars+612);
  if (count>8 || slot>=count) return 0;
  return Ptr(b,Load32(b,o.vars+640+slot*4),24,42);
}
ObjectState Sample(const uint8_t* b,const Binding& o) {
  ObjectState s{state.generation,state.level,1,o.source,o.kind};
  if (o.kind==ObjectKind::Breakable) s.value=Load32(b,o.vars+552);
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
#if defined(KK_DEV_TOOLS)
  if (state.debug_sources.size()<8 && (s.kind==ObjectKind::Breakable ? s.value>0 : s.position>0 || (s.value&255)))
    state.debug_sources.insert(s.source);
#endif
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
bool Request(const Binding& o,uint32_t value) {
  if (state.request==UINT32_MAX || state.outgoing.size()>=objects::kQueue) { ++state.rejected; return false; }
  Message m; m.type=MessageType::ObjectRequest; m.epoch=state.epoch;
  m.object={state.generation,state.level,++state.request,o.source,o.kind,value};
  state.outgoing.push_back(m); ++state.requested;
  return true;
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
    if (r.kind==ObjectKind::Breakable) o->pending_damage=std::min(o->pending_damage+r.value,100000u);
    else o->pending_levers|=1u<<r.value;
    o->pending_until=Now()+.5;
#if defined(KK_DEV_TOOLS)
    if (state.debug_sources.size()<8) state.debug_sources.insert(r.source);
#endif
    ++state.accepted;
  }
}
void Damage(PPCContext& ctx,uint8_t* b) {
  const auto output=ctx.r4.u32;
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Breakable) : nullptr;
  __imp__sub_824ADD48(ctx,b);
  if (!o || !PoseAddress(output,4)) return;
  const auto local=Load32(b,output);
  if (state.host) {
    if (o->pending_damage && Now()<=o->pending_until) {
      Store32(b,output,std::min(local+o->pending_damage,100000u)); o->pending_damage=0; ++state.applied;
    }
    else o->pending_damage=0;
    return;
  }
  // Environmental/AI damage is simulated only by the host. Forward only a
  // native hit whose source was this guest's validated Jack, once consumed.
  if (local && Now()<=o->local_hit_until) {
    Request(*o,local>=100 ? 100 : 1); o->local_hit_until=0;
  }
  const auto* authority=state.replica.Find(o->kind,o->source);
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
void RefreshDoor(PPCContext& ctx,uint8_t* b,Binding& o,const ObjectState& s) {
  if (Load32(b,0x82CCBF64)!=o.ai || Load32(b,0x82CCBF6C)!=o.actor) return;
  const bool ended=!(s.value&kDoorScheduled) && DoorScheduled(b,o.ai);
  if (!ended && LoadF(b,o.vars+1084)==s.position && o.driven_position==s.position) return;
  // This is called only in the door's own native loop/dispatcher context.
  // Retain native geometry/grid/capability work and protect the hook caller's
  // volatile registers, stack scratch and dispatcher globals from the extra call.
  const auto sp=ctx.r1.u32;
  if ((sp&15) || sp<0x40014000 || sp>=0xA0000000) return;
  uint8_t scratch[16384]; const auto start=sp-sizeof(scratch); const PPCContext saved=ctx;
  std::memcpy(scratch,b+start,sizeof(scratch));
  constexpr uint32_t globals[]{0x82CCBF5C,0x82CCBF60,0x82CCBF64,0x82CCBF68,0x82CCBF6C,0x82CCBF70};
  uint32_t values[6]; for (unsigned i=0;i<6;++i) values[i]=Load32(b,globals[i]);
  ctx.r1.u64=start+12288; std::memset(b+start+12288,0,4096); Store32(b,start+12288,sp);
  ApplyDoor(b,o,s);
  {
    DoorStopGuard guard(o,s); __imp__sub_82552200(ctx,b);
    if (ended && DoorScheduled(b,o.ai)) __imp__sub_826F9DC0(ctx,b);
  }
  for (unsigned i=0;i<6;++i) Store32(b,globals[i],values[i]);
  std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved; o.driven_position=s.position;
}
void Door(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Door) : nullptr;
  if (!o) { __imp__sub_8256C3F8(ctx,b); return; }
  const auto game=Load32(b,kDoorGamePtr);
  const auto* s=state.host ? nullptr : state.replica.Find(o->kind,o->source);
  if (!state.host && o->local_levers) {
    const auto done=s ? (s->position==1 ? o->local_levers : o->local_levers&s->value) : 0;
    if (done) FinishLocalLever(b,*o,done);
    if (Now()>o->local_lever_until) FinishLocalLever(b,*o,o->local_levers);
  }
  if (Now()>o->pending_until) o->pending_levers=0;
  bool natural=false;
  for (unsigned i=0;i<8;++i) if (const auto ext=Lever(b,*o,i)) natural|=(Load16(b,ext+40)&0x8001)==0x8000;
  // Do not restore inventory over a real host interaction occurring in this
  // same native call. Let that input finish, then admit guest-only work on the
  // next scheduled door pass (still subject to the half-second deadline).
  const bool forced=state.host && o->pending_levers && !natural && PoseAddress(game,17804);
  uint32_t saved[4]{};
  constexpr uint32_t offsets[]{13128,13204,13280,17800};
  if (forced) for (unsigned i=0;i<4;++i) saved[i]=Load32(b,game+offsets[i]);
  for (unsigned i=0;i<8;++i) if (const auto ext=Lever(b,*o,i)) {
    const auto flags=Load16(b,ext+40);
    if (forced && (o->pending_levers&(1u<<i)) && !(flags&1)) Store16(b,ext+40,flags|0x8000);
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
  if (forced) {
    for (unsigned i=0;i<4;++i) Store32(b,game+offsets[i],saved[i]);
    if ((Sample(b,*o).value&o->pending_levers)==o->pending_levers) ++state.applied;
    else ++state.rejected;
    o->pending_levers=0;
  }
  if (s && Valid(b,*o)) RefreshDoor(ctx,b,*o,*s);
  if (state.host && Components(b,*o) && !DoorScheduled(b,o->ai)) Observe(b,*o);
}
void Breakable(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Breakable) : nullptr;
  __imp__sub_82515CF0(ctx,b);
  // Capture the final native damage in the same call, before a later frame
  // retires an exploded obstacle from the full registry.
  if (o && state.host && Components(b,*o)) Observe(b,*o);
}
void DoorUpdate(PPCContext& ctx,uint8_t* b) {
  auto* o=state.ready && Shared() ? Current(b,ObjectKind::Door) : nullptr;
  const auto* s=o && !state.host ? state.replica.Find(o->kind,o->source) : nullptr;
  if (s) ApplyDoor(b,*o,*s);
  // Geometry, nav-grid, capability bits and completed-lever cleanup remain
  // native. Never copy transform or collision-component memory from the peer.
  if (s) { DoorStopGuard guard(*o,*s); __imp__sub_82552200(ctx,b); }
  else __imp__sub_82552200(ctx,b);
  if (s) o->driven_position=s->position;
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
  auto* o=state.ready && !state.host && Shared() ? Current(b,ObjectKind::Door) : nullptr;
  __imp__sub_82543478(ctx,b);
  if (o && Valid(b,*o)) if (const auto* s=state.replica.Find(o->kind,o->source)) RefreshDoor(ctx,b,*o,*s);
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
  unsigned counts[12]{};
  constexpr const char* names[]{"unknown","iob","door","ode","tree","bamboo","rack","fire","raft","ammo","javelin","checkpoint"};
  constexpr uint32_t offsets[12][3]{{},{552,556,560},{0,612,1096},{0,8,2444},{24,20,84},
      {196,24,32},{192,196,228},{440,444,408},{204,224,644},{0,4,24},{0,232,536},{4,9540,9156}};
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
        case 0x826F88F0: case 0x826E58B8: case 0x826EAE60: case 0x826E4330: case 0x826DF6D8: next=4; break;
        case 0x8264EC58: case 0x8262A7A0: case 0x82644A60: next=5; break;
        case 0x8251C540: case 0x8251BA60: case 0x8251B800: next=6; break;
        case 0x825FAF58: case 0x8260F858: case 0x825F8CE8: next=7; break;
        case 0x825FD388: case 0x825E7348: case 0x8260C988: next=8; break;
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
  }
  REXLOG_INFO("KK dev object inventory totals: role={} level={} pass={} iob={} door={} ode={} tree={} bamboo={} rack={} fire={} raft={} ammo={} javelin={} checkpoint={} unknown={} bounded-rows={}",
      v.host ? "host" : "guest",v.world.level,state.scan_pass,counts[1],counts[2],counts[3],counts[4],counts[5],counts[6],counts[7],counts[8],counts[9],counts[10],counts[11],counts[0],state.scan_rows);
}
// Explicit native-input fixtures, never a network outcome injector. Selects a
// real loaded object, moves only this process's Jack, then supplies a native
// firearm event or the interaction tag normally produced by the lever action.
// Acceptance must still be observed in each world's native damage/opening state.
struct Probe {
  double due=-1,act=0; uint64_t source=0; bool done=false;
  unsigned slot=0;
  explicit Probe(const char* name) {
    const char* value=std::getenv(name); if (!value || !*value) return;
    char* end=nullptr; const auto t=std::strtod(value,&end);
    if (end!=value && !*end && std::isfinite(t) && t>=0 && t<=3600) due=t;
  }
};
bool ProbeHit(PPCContext& ctx,uint8_t* b,const Binding& o,uint32_t hero,uint32_t ai) {
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
  ctx.r3.u64=result; ctx.r4.s64=-1; ctx.r5.u64=64; ctx.r6.u64=hero; ctx.r7.u64=0;
  ctx.r8.u64=o.actor; ctx.r9.s64=-1; ctx.r10.u64=1000; ctx.f1.f64=.5;
  ObjectsObserveHit(ctx,b); __imp__sub_82634428(ctx,b);
  const bool queued=Load32(b,result)!=UINT32_MAX && Load32(b,result+4)!=0;
  for (unsigned i=0;i<4;++i) Store32(b,globals[i],values[i]);
  std::memcpy(b+start,scratch,sizeof(scratch)); ctx=saved; return queued;
}
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
        for (;slot<8;++slot) { const auto ext=Lever(b,o,slot); if (ext && !(Load16(b,ext+40)&1)) break; }
        if (slot==8) continue;
      }
      const auto marker=kind==ObjectKind::Door ? Ptr(b,Load32(b,o.vars+640+4*slot),12,64) : o.matrix;
      if (!marker || std::abs(LoadF(b,o.matrix+56)-LoadF(b,hm+56))>2) continue;
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
    if (std::abs(LoadF(b,best->matrix+56)-LoadF(b,hm+56))>2) return;
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
  if (kind==ObjectKind::Breakable) supplied=ProbeHit(ctx,b,*o,hero,ai);
  else if (const auto ext=Lever(b,*o,probe.slot)) { Store16(b,ext+40,Load16(b,ext+40)|0x8000); supplied=true; }
  if (!supplied) return;
  probe.done=true;
  REXLOG_INFO("KK dev object probe: native input role={} kind={} id={:016X} supplied={} native-result-awaiting=true",
      state.host ? "host" : "guest",uint8_t(kind),o->source,supplied);
}
void Probes(PPCContext& ctx,uint8_t* b) {
  static Probe hit("KK_DEV_OBJECT_HIT"),lever("KK_DEV_OBJECT_LEVER");
  DevProbe(ctx,b,hit,ObjectKind::Breakable); DevProbe(ctx,b,lever,ObjectKind::Door);
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
void ObjectsObserveHit(const PPCContext& ctx,const uint8_t* b) {
  if (!state.ready || state.host || !Shared()) return;
  const auto hero=Load32(b,0x82CCBF7C);
  if (ctx.r6.u32!=hero) return;
  const auto ai=Ptr(b,Ptr(b,hero,24,8),4,88);
  if (!Track(b,ai,0x826718B8)) return;
  const auto it=state.bindings.find(ctx.r8.u32);
  if (it!=state.bindings.end() && it->second.kind==ObjectKind::Breakable && Valid(b,it->second) &&
      Match(it->second.source,it->second.kind)==&it->second) it->second.local_hit_until=Now()+.5;
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
      o.pending_damage=o.pending_levers=o.local_levers=0; o.local_hit_until=0;
    }
    return;
  }
  // The partner may spend longer than ten seconds behind the load barrier.
  // Cached authored identities stay fixed; admit still-initializing bindings
  // for ten seconds of actual shared AI, rather than expiring while waiting.
  if (!state.census_started) { state.census_started=true; state.census_until=Now()+10; state.next_census=0; }
  if (Now()>=state.next_census) Census(b);
  state.ready=true; Receive(b); Flush();
#if defined(KK_DEV_TOOLS)
  Probes(ctx,b);
  PickupProbe(ctx,b);
#endif
}
void ObjectsAfterAi(PPCContext&,uint8_t* b,uint32_t w) {
  if (!state.ready || w!=state.world || !Shared()) return;
  const auto now=Now();
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
  if (now>=state.next_log) {
    state.next_log=now+5;
    unsigned breakables=0,doors=0;
    for (auto& [actor,o] : state.bindings) { breakables+=o.kind==ObjectKind::Breakable; doors+=o.kind==ObjectKind::Door; }
    REXLOG_INFO("KK co-op objects: role={} generation={} level={} breakables={} doors={} requests={} accepted={} applied={} received={} refused={} pending={}",
        state.host ? "host" : "guest",state.generation,state.level,breakables,doors,state.requested,state.accepted,state.applied,state.received,state.rejected,state.pending.size()+state.outgoing.size());
#if defined(KK_DEV_TOOLS)
    unsigned details=0;
    if (Trace()) for (const auto& [actor,o] : state.bindings) if (Valid(b,o) && Match(o.source,o.kind)==&o) {
      if (details++>=32 && !state.debug_sources.contains(o.source)) continue;
      const auto local=Sample(b,o); const auto* authority=state.replica.Find(o.kind,o.source);
      REXLOG_INFO("KK dev object state: role={} id={:016X} kind={} local={}/{:.3f}/{:.3f} host={}/{:.3f} matched={}",
          state.host ? "host" : "guest",o.source,uint8_t(o.kind),local.value,local.position,local.seconds,
          authority ? authority->value : local.value,authority ? authority->position : local.position,state.host || authority!=nullptr);
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
REX_HOOK_RAW(sub_82543478) { kk::coop::DoorWait(ctx,base); }
REX_HOOK_RAW(sub_826F9DC0) { kk::coop::DoorStop(ctx,base); }
