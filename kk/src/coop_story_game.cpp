// C41: host owns SSDetect/SSLoop. Native memory and callbacks stay on the game
// thread. Unsupported records stop guest story and offer C24 checkpoint recovery.
#include "coop_story_game.h"
#include "coop_story.h"
#include "coop_runtime.h"
#include "coop_pose.h"
#include "coop_partner_body.h"
#include "hfr.h"
#include "settings.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

REX_EXTERN(__imp__sub_82454690);
REX_EXTERN(__imp__sub_82476848);
REX_EXTERN(__imp__sub_8244F0E8);
REX_EXTERN(__imp__sub_824FC2D0);
REX_EXTERN(__imp__sub_8250EE50);
REX_EXTERN(__imp__sub_8250EDE8);
REX_EXTERN(__imp__sub_82543CA0);
REX_EXTERN(__imp__sub_826F9E70);
REX_EXTERN(__imp__sub_82847478);
#if defined(KK_DEV_TOOLS)
namespace kk { void DevTrigPivotResult(uint8_t*,uint32_t,uint32_t,uint32_t); }
#endif

namespace kk::coop {
namespace {
using namespace hfr;
using story::Binding;
constexpr uint32_t kDetect=0x82476848, kLoop=0x82454690, kPivot=0x8244F0E8;
constexpr uint32_t kNothing=0x824FC2D0, kActivity=0x8250EE50, kVisible=0x8250EDE8, kDoor=0x82543CA0;
constexpr uint32_t kDetectRecord=kStoryRecords;
struct Script {
  Binding binding;
  uint32_t run=0, request_run=0;
  double request_until=0, next_request=0;
};
std::map<uint32_t,Script> scripts;
std::map<uint64_t,uint32_t> runs;
std::vector<Binding> bindings;
std::deque<Message> outgoing;
story::Ordered ordered;
uint64_t epoch=0;
uint32_t generation=0, level=0, number=0, request_number=0, last_request=0, last_loaded=0;
double next_census=0, request_window=0;
unsigned request_count=0;
bool host=false, failed=false, disconnected=false, recovery_pending=false;
std::atomic<bool> behind{false};
// Only wraps the exact safe native predicate call. No hero/global pointer swap.
thread_local uint32_t forced_pivot=0;
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool Trace() {
#if defined(KK_DEV_TOOLS)
  static const bool on=std::getenv("KK_DEV_TRIG_TRACE")!=nullptr;
  return on;
#else
  return false;
#endif
}
void Note(const char* what,const StoryEvent& e,const char* reason="") {
  if (Trace()) REXLOG_INFO("KK co-op story: {} role={} gen={} level={} number={} source={:016X} script={:016X} run={} record={} phase={} {}",
      what,host ? "host" : "guest",e.generation,e.level,e.number,e.source,e.script,e.run,e.record,unsigned(e.phase),reason);
}
void Refuse(const char* why,const StoryEvent& e={}) {
  if (!failed) {
    Note("refused",e,why); REXLOG_WARN("KK co-op story: {} world behind: {}",host ? "host" : "guest",why);
    if (host) recovery_pending=true;
  }
  failed=true; ordered.Refuse();
  if (!host) behind=true;
}
uint32_t Ptr(const uint8_t* b,uint32_t p,uint32_t offset,uint32_t bytes) {
  if (!PoseAddress(p,offset+4)) return 0;
  const auto q=Load32(b,p+offset);
  return PoseAddress(q,bytes) ? q : 0;
}
uint32_t Vars(const uint8_t* b,uint32_t ai,uint32_t needed) {
  // Allocation contract from 8271DC78, also used by C25's body reference audit.
  const auto definition=Ptr(b,Ptr(b,ai,0,4),0,8);
  if (!definition) return 0;
  const auto bytes=Load32(b,definition+4);
  return bytes>=needed && bytes<=1024*1024 ? Ptr(b,ai,64,needed) : 0;
}
bool Tracks(const uint8_t* b,uint32_t ai,uint32_t tag) {
  if (!PoseAddress(ai,88)) return false;
  const auto first=Load16(b,ai+80),last=Load16(b,ai+82);
  if (first>last || last>4096 || last-first>64 || !PoseAddress(ai,(last+7)*4)) return false;
  for (uint32_t t=first;t<=last;++t) {
    const auto block=Ptr(b,ai,(t+6)*4,24);
    if (block && Load32(b,block+20)==tag) return true;
  }
  return false;
}
uint64_t Shape(const uint8_t* b,uint32_t vars) {
  if (!PoseAddress(vars,19824)) return 0;
  uint64_t hash=enemies::Mix(14695981039346656037ull,Load32(b,vars+20));
  for (uint32_t i=0;i<=kStoryRecords;++i) {
    const auto fn=Load32(b,vars+924+i*180);
    // Executable addresses are hashed locally, never sent or invoked from wire.
    if (fn && (fn<0x82000000 || fn>=0x82900000 || (fn&3))) return 0;
    hash=enemies::Mix(hash,fn);
    if (!fn) return hash ? hash : 1;
  }
  return 0; // No bounded terminator. Do not guess how many records exist.
}
void Census(const uint8_t* b) {
  bindings.clear();
  const auto world=Load32(b,0x828E8388);
  if (!PoseAddress(world,44)) return;
  const auto begin=Load32(b,world+40),end=Load32(b,world+28);
  if (!PoseAddress(begin) || !PoseAddress(end) || end<begin || (end-begin)%8 || end-begin>8*20000) return;
  std::set<uint32_t> seen;
  for (uint32_t p=begin;p<end;p+=8) {
    const auto actor=Load32(b,p);
    if (!PoseAddress(actor,32) || !seen.insert(actor).second) continue;
    const auto ai=Ptr(b,Ptr(b,actor,24,8),4,88),vars=Vars(b,ai,19824);
    auto it=scripts.find(actor);
    if (!vars || (it==scripts.end() && !Tracks(b,ai,kDetect) && !Tracks(b,ai,kLoop))) continue;
    if (it!=scripts.end() && (it->second.binding.ai!=ai || it->second.binding.vars!=vars)) {
      Refuse("script allocation reused"); continue;
    }
    if (it==scripts.end()) {
      const auto matrix=Ptr(b,actor,12,64);
      const auto shape=Shape(b,vars);
      if (!matrix || !shape || scripts.size()>=4096) continue;
      const auto source=story::Source(shape,LoadF(b,matrix+48),LoadF(b,matrix+52),LoadF(b,matrix+56));
      if (!source) continue;
      Script s; s.binding={source,shape,actor,ai,vars};
      if (const auto run=runs.find(source); run!=runs.end()) s.run=run->second;
      it=scripts.emplace(actor,s).first;
    }
    // Include dormant and ambiguous objects. Never pick the first same-position script.
    bindings.push_back(it->second.binding);
  }
  next_census=Now()+1;
}
bool Ensure(uint8_t* b) {
  if (!REXCVAR_GET(kk_coop_enabled) || !SessionInProcess()) { behind=false; return false; }
  const auto v=SessionFlowSnapshot();
  if (!v.active || v.policy!=ChapterPolicy::Together ||
      (v.phase!=FlowPhase::Playing && v.phase!=FlowPhase::Loading)) { behind=false; return false; }
  const auto link=EnemyLinkSnapshot();
  if (!link.epoch) return false;
  if (epoch!=link.epoch || generation!=v.world.generation || level!=v.world.level) {
    epoch=link.epoch; generation=v.world.generation; level=v.world.level; host=v.host;
    number=request_number=last_request=0; scripts.clear(); bindings.clear(); runs.clear(); outgoing.clear();
    ordered.Reset(generation,level); behind=false; failed=disconnected=recovery_pending=false;
    request_count=0; request_window=next_census=0;
  }
  if (SessionPrivateReload() && !failed) Refuse("private reload has no story snapshot");
  if (!link.connected) { disconnected=true; if (!host) Refuse("connection interrupted; no script snapshot"); }
  else if (disconnected) { recovery_pending=true; disconnected=false; }
  if (Now()>=next_census) Census(b);
  return true;
}
Script* Current(const uint8_t* b) {
  const auto actor=Load32(b,0x82CCBF6C),ai=Load32(b,0x82CCBF64);
  const auto it=scripts.find(actor);
  if (it==scripts.end()) return nullptr;
  const auto& bind=it->second.binding;
  const auto* match=story::Match(bind.source,bind.script,bindings);
  if (!match || match->actor!=actor || match->ai!=ai || match->vars!=bind.vars ||
      bind.ai!=ai || Vars(b,ai,19824)!=bind.vars || Shape(b,bind.vars)!=bind.script) return nullptr;
  return &it->second;
}
StoryEvent Event(const Script& s,StoryPhase phase,uint32_t record) {
  return {generation,level,0,s.binding.source,s.binding.script,s.run,record,phase};
}
void Flush() {
  while (!outgoing.empty() && SubmitStory(outgoing.front())) {
    Note(outgoing.front().type==MessageType::StoryEvent ? "sent (reliable queued)" : "request sent",outgoing.front().story);
    outgoing.pop_front();
  }
}
void Emit(StoryEvent e) {
  if (number==UINT32_MAX || outgoing.size()>=kStoryQueue) {
    Refuse("story outgoing overflow",e);
    Message abort; abort.type=MessageType::FlowAbort; abort.flow=SessionFlowSnapshot().world; SubmitFlowMessage(abort); return;
  }
  e.number=number+1;
  if (!ValidStoryEvent(e)) { Refuse("invalid observed story event",e); return; }
  ++number;
  Message m; m.type=MessageType::StoryEvent; m.epoch=epoch; m.story=e; outgoing.push_back(m); Flush();
}
bool HeroPivot(const uint8_t* b,uint32_t payload) {
  const auto hero=Load32(b,0x82CCBF7C);
  const auto ai=Ptr(b,Ptr(b,hero,24,8),4,88);
  return PoseAddress(payload,108) && PoseAddress(Load32(b,payload+4),32) &&
      Tracks(b,ai,0x826718B8) && Ptr(b,hero,12,64) && Load32(b,payload+8)==hero &&
      !Load32(b,payload+96) && !Load32(b,payload+100);
}
bool DoorTarget(const uint8_t* b,uint32_t target) {
  const auto ai=Ptr(b,Ptr(b,target,24,44),4,88);
  return ai && Tracks(b,ai,0x82551ED8) && Load32(b,ai)==Load32(b,0x82CC90B4) && Vars(b,ai,556);
}
bool EnemyTarget(const uint8_t* b,uint32_t target) {
  if (target==Load32(b,0x82CCBF7C)) return false;
  const auto ai=Ptr(b,Ptr(b,target,24,8),4,88);
  for (auto fn : {0x8254BCB0u,0x825B9020u,0x82572E40u,0x825088A0u,0x82516820u,0x82516508u,
                  0x825F2380u,0x826055F0u,0x825FE790u}) if (Tracks(b,ai,fn)) return true;
  return false;
}
bool SafeAction(const uint8_t* b,uint32_t fn,uint32_t payload) {
  if (fn==kNothing) return true;
  const auto target=Load32(b,payload+4);
  if (!PoseAddress(target,32) || target==Load32(b,0x82CCBF7C)) return false;
  if (fn==kDoor) {
    const auto time=LoadF(b,payload+24);
    return DoorTarget(b,target) && std::isfinite(time) && time>=0 && time<=3600;
  }
  // Native wrappers pass (1 - payload+84). Zero to 82713A58 clears
  // the disabled-AI bit 0x400000, which 8271D498 tests before dispatch.
  return (fn==kActivity || fn==kVisible) && Load32(b,payload+84)==1 &&
      (DoorTarget(b,target) || EnemyTarget(b,target));
}
bool SafeRequest(const uint8_t* b,const Script& s) {
  const auto vars=s.binding.vars;
  // Capability bookkeeping and inverse/camera/edge tests are not request-safe.
  if (Load32(b,vars+8) || Load32(b,vars+20)!=kPivot || !HeroPivot(b,vars+88)) return false;
  bool action=false;
  for (uint32_t i=0;i<=kStoryRecords;++i) {
    const auto fn=Load32(b,vars+924+180*i);
    if (!fn) return action;
    if (i==kStoryRecords) return false;
    if (!SafeAction(b,fn,vars+992+180*i)) return false;
    action|=fn!=kNothing;
  }
  return false;
}
bool NativeInside(PPCContext& ctx,uint8_t* b,uint32_t volume,const PlayerPose& pose) {
  // 82847478 only reads volume flags, bound data, and the second object's XYZ.
  // Its only writes are its caller's stack scratch (.34:32210-32440).
  if (!Ptr(b,volume,16,24) || !Ptr(b,volume,12,64)) return false;
  const auto saved=ctx;
  if (ctx.r1.u32<512 || !PoseAddress(ctx.r1.u32-512,512)) return false;
  const auto scratch=ctx.r1.u32-384,actor=scratch+80,matrix=scratch+128;
  const auto save_begin=ctx.r1.u32-512;
  uint8_t old[512]; std::memcpy(old,b+save_begin,sizeof old);
  std::memset(b+actor,0,32); std::memset(b+matrix,0,64);
  Store32(b,actor+4,1); Store32(b,actor+12,matrix);
  StoreF(b,matrix+48,pose.x); StoreF(b,matrix+52,pose.y); StoreF(b,matrix+56,pose.z);
  ctx.r1.u32=scratch; ctx.r3.u32=volume; ctx.r4.u32=actor; ctx.r5.u32=0;
  __imp__sub_82847478(ctx,b);
  const bool inside=ctx.r3.u32!=0;
  std::memcpy(b+save_begin,old,sizeof old); ctx=saved; return inside;
}
void Request(PPCContext& ctx,uint8_t* b,Script& s) {
  const auto payload=s.binding.vars+88;
  if (Now()<s.next_request || Load32(b,s.binding.vars+20)!=kPivot || !HeroPivot(b,payload)) return;
  const auto saved=ctx;
  uint8_t old[108]; std::memcpy(old,b+payload,sizeof old);
  ctx.r3.u32=payload; __imp__sub_8244F0E8(ctx,b);
  const bool entered=ctx.r3.u32!=0;
  std::memcpy(b+payload,old,sizeof old); ctx=saved;
  if (!entered) return;
  s.next_request=Now()+1;
  if (request_number==UINT32_MAX || s.run==UINT32_MAX || outgoing.size()>=kStoryQueue) { Refuse("request overflow"); return; }
  auto e=Event(s,StoryPhase::Armed,kDetectRecord); e.run=s.run+1; e.number=++request_number;
  Message m; m.type=MessageType::TriggerRequest; m.epoch=epoch; m.story=e; outgoing.push_back(m); Flush();
}
bool For(const StoryEvent* e,const Script& s,StoryPhase phase) {
  return e && e->source==s.binding.source && e->script==s.binding.script && e->phase==phase;
}
void Applied(const StoryEvent& e) { Note("applied",e); ordered.Applied(); }
} // namespace

bool StoryBehind() { return behind.load(); }
void StoryLoaded() {
  const auto v=SessionFlowSnapshot();
  if (v.policy==ChapterPolicy::Together && last_loaded==v.world.generation) {
    recovery_pending=true;
  }
  last_loaded=v.world.generation;
  // Never carry heap bindings across uni_go. Preserve only value IDs/run counts
  // observed during this generation's initialization, before its Loaded barrier.
  scripts.clear(); bindings.clear(); next_census=0;
}
void StoryGameFrame(uint8_t* b) {
  if (!Ensure(b)) { TakeStory(); return; }
  const double now=Now();
  if (recovery_pending) {
    recovery_pending=false;
    if (host) { StoryEvent e{generation,level,0,1,1,1,0,StoryPhase::Recovery}; Emit(e); }
    Refuse("private load or reconnect requires shared checkpoint recovery");
  }
  for (const auto& m : TakeStory()) {
    const auto& e=m.story;
    if (m.epoch!=epoch || e.generation!=generation || e.level!=level) { Note("refused",e,"stale world"); continue; }
    if (!host) { ordered.Push(e,now); continue; }
    if (e.number<=last_request) continue;
    last_request=e.number;
    if (now-request_window>=1) { request_window=now; request_count=0; }
    if (++request_count>4) { Note("request refused",e,"rate limit"); continue; }
    const auto* bind=story::Match(e.source,e.script,bindings);
    if (!bind) { Note("request refused",e,"missing/ambiguous identity"); continue; }
    auto& s=scripts.at(bind->actor);
    if (failed || e.run!=s.run+1 || !SafeRequest(b,s)) { Note("request refused",e,"unsafe kind or stale run"); continue; }
    // Recheck a fresh partner pose and native volume when this script is scheduled.
    s.request_run=e.run; s.request_until=now+.5; Note("request pending",e);
  }
  if (!host && SessionFlowSnapshot().phase==FlowPhase::Playing) {
    ordered.Tick(now);
    if (ordered.Behind()) Refuse("event gap, conflict or apply timeout");
    if (const auto* e=ordered.Front(); e && e->phase==StoryPhase::Recovery) Refuse("host requested checkpoint catch-up",*e);
  }
  Flush();
}

// Return true when co-op handled this call, including a suppressed local advance.
bool StoryDetect(PPCContext& ctx,uint8_t* b) {
  if (!Ensure(b)) return false;
  auto* s=Current(b);
  if (!s) { Refuse("unmatched detection script"); if (host) return false; ctx.r3.u32=0; return true; }
  const auto flag=Load32(b,0x82CCBF60),vars=s->binding.vars;
  if (!PoseAddress(flag)) { Refuse("detection state missing"); if (host) return false; ctx.r3.u32=0; return true; }
  const auto before=Load32(b,flag);
  if (host) {
    if (!failed && !before && s->request_run==s->run+1 && Now()<=s->request_until && SafeRequest(b,*s)) {
      const auto link=EnemyLinkSnapshot();
      if (link.connected && link.partner && link.partner->level==level &&
          NativeInside(ctx,b,Load32(b,vars+92),*link.partner)) {
        forced_pivot=vars+88; Note("request accepted",Event(*s,StoryPhase::Armed,kDetectRecord));
      }
    }
    __imp__sub_82476848(ctx,b); forced_pivot=0;
    if (failed) return true;
    const auto after=Load32(b,flag);
    if (!before && after==1) {
      if (s->run==UINT32_MAX) Refuse("script run overflow");
      else { ++s->run; runs[s->binding.source]=s->run; s->request_run=0; Emit(Event(*s,StoryPhase::Armed,kDetectRecord)); }
    } else if (before==1 && !after && ctx.r3.u32==5) Emit(Event(*s,StoryPhase::Started,kDetectRecord));
    return true;
  }
  ctx.r3.u32=0;
  if (failed || ordered.Behind()) return true;
  const auto* front=ordered.Front();
  if (For(front,*s,StoryPhase::Armed)) {
    const auto e=*front;
    if (before || e.run!=s->run+1 || Load32(b,vars+20)!=kPivot || !HeroPivot(b,vars+88) || Load32(b,vars+8)) {
      Refuse("detection callback or state is not replay-safe",e); return true;
    }
    forced_pivot=vars+88; __imp__sub_82476848(ctx,b); forced_pivot=0;
    if (Load32(b,flag)==1) { s->run=e.run; runs[s->binding.source]=s->run; Applied(e); }
  } else if (For(front,*s,StoryPhase::Started)) {
    const auto e=*front;
    if (before!=1 || e.run!=s->run) { Refuse("detection start state mismatch",e); return true; }
    __imp__sub_82476848(ctx,b);
    if (ctx.r3.u32==5 && !Load32(b,flag)) Applied(e);
    else Refuse("native script start refused",e);
  } else if (!before) Request(ctx,b,*s);
  return true;
}

bool StoryLoop(PPCContext& ctx,uint8_t* b) {
  if (!Ensure(b)) return false;
  auto* s=Current(b);
  if (!s) { Refuse("unmatched loop script"); if (host) return false; ctx.r3.u32=0; return true; }
  const auto vars=s->binding.vars,before=Load32(b,vars+19812);
  if (host) {
    __imp__sub_82454690(ctx,b);
    if (failed) return true;
    const auto after=Load32(b,vars+19812);
    if (!s->run || before>after || after>kStoryRecords) { Refuse("loop jump or unobserved activation"); return true; }
    for (uint32_t i=before;i<after;++i) Emit(Event(*s,StoryPhase::Advance,i));
    if (ctx.r3.u32==5) Emit(Event(*s,StoryPhase::Finished,after));
    return true;
  }
  ctx.r3.u32=0;
  if (failed || ordered.Behind()) return true;
  // Reproduce only the documented SSLoop record bookkeeping. Never invoke an
  // address from the packet; all calls below are fixed, locally validated natives.
  for (unsigned budget=0;budget<kStoryRecords;++budget) {
    const auto* front=ordered.Front();
    if (!front || front->source!=s->binding.source || front->script!=s->binding.script) break;
    const auto e=*front;
    const auto index=Load32(b,vars+19812);
    if (e.run!=s->run || e.record!=index || index>kStoryRecords) { Refuse("record/run mismatch",e); break; }
    const auto record=vars+920+180*index,payload=record+72,fn=Load32(b,record+4);
    if (e.phase==StoryPhase::Finished) {
      if (fn) { Refuse("terminator mismatch",e); break; }
      ctx.r3.u32=Load32(b,0x82CC9B0C); __imp__sub_826F9E70(ctx,b); ctx.r3.u32=5; Applied(e); return true;
    }
    if (index==kStoryRecords || e.phase!=StoryPhase::Advance || !fn || !(SafeAction(b,fn,payload) || (fn==kPivot && HeroPivot(b,payload)))) {
      Refuse("unsupported story callback; use checkpoint recovery",e); break;
    }
    Store32(b,payload,Load32(b,0x82CCBF6C));
    kk::PartnerBodyTriggerReference(b,payload);
    const auto saved=ctx; ctx.r3.u32=payload;
    switch (fn) {
      case kNothing: __imp__sub_824FC2D0(ctx,b); break;
      case kActivity: __imp__sub_8250EE50(ctx,b); break;
      case kVisible: __imp__sub_8250EDE8(ctx,b); break;
      case kDoor: __imp__sub_82543CA0(ctx,b); break;
      case kPivot: __imp__sub_8244F0E8(ctx,b);
        Store32(b,payload+20,Load32(b,0x82CCBF7C)); ctx.r3.u32=1; break;
    }
    const bool ok=ctx.r3.u32!=0; ctx=saved;
    if (!ok || Load32(b,vars+19812)!=index) { Refuse("native callback refused or changed index",e); break; }
    Store32(b,vars+19812,index+1); Store32(b,vars+19816,0);
    // Native initialization of the next record's payload word +100.
    if (index+1<kStoryRecords && Load32(b,record+184)) Store32(b,record+180+72+100,0);
    Applied(e);
  }
  Store32(b,vars+19816,1); // native waiting result; no guest-local callback is run
  return true;
}
} // namespace kk::coop

REX_HOOK_RAW(sub_82476848) {
  if (!kk::coop::StoryDetect(ctx,base)) __imp__sub_82476848(ctx,base);
}
REX_HOOK_RAW(sub_8244F0E8) {
  const auto rec=ctx.r3.u32,lr=uint32_t(ctx.lr);
  __imp__sub_8244F0E8(ctx,base);
  if (kk::coop::forced_pivot==rec) {
    kk::hfr::Store32(base,rec+20,kk::hfr::Load32(base,0x82CCBF7C)); ctx.r3.u32=1;
  }
#if defined(KK_DEV_TOOLS)
  kk::DevTrigPivotResult(base,rec,lr,ctx.r3.u32);
#endif
}
