// Safe door/activation scripts replay exactly. Other scripts run natively on
// both peers; host events are convergence hints, never arbitrary native calls.
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
#include <optional>
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
struct Script : story::Progress {
  Binding binding;
  story::RequestLatch requested;
  std::optional<StoryEvent> request;
  double request_until=0;
  story::Hints hints;
  story::Lag lag;
  bool exact=false;
  bool saw_detect=false, saw_loop=false;
};
std::map<uint32_t,Script> scripts;
std::map<uint64_t,uint32_t> runs;
std::vector<Binding> bindings;
std::deque<Message> outgoing;
story::Ordered ordered;
story::Checksums checksums;
uint64_t epoch=0;
uint32_t generation=0, level=0, number=0, request_number=0, last_request=0, last_loaded=0;
double next_census=0, request_window=0;
double next_checksum=0;
uint32_t checksum_actor=0;
bool checksum_sweep=false;
unsigned request_count=0;
bool host=false, failed=false, disconnected=false, recovery_pending=false;
std::atomic<bool> behind{false};
std::atomic<bool> needs_move{false};
uint32_t waiting_number=0;
double waiting_since=0;
bool SafeRequest(const uint8_t* b,const Script& s);
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
  if (Trace()) REXLOG_INFO("KK co-op {}: {} role={} gen={} level={} number={} source={:016X} script={:016X} run={} record={} phase={} {}",
      e.phase==StoryPhase::Checksum ? "story checksum" : "story",what,host ? "host" : "guest",e.generation ? e.generation : generation,e.level ? e.level : level,
      e.number,e.source,e.script,e.run,e.record,unsigned(e.phase),reason);
}
void FinishRequest(Script& s,const char* outcome,const char* reason="") {
  if (!s.request) return;
  Note(outcome,*s.request,reason); s.request.reset();
}
// Copy before clearing Ordered: the caller may be passing its current Front().
void Refuse(const char* why,StoryEvent e={}) {
  ordered.Refuse(host);
  if (host) {
    Note("authority observation deferred",e,why);
    return;
  }
  if (!failed) {
    Note("refused",e,why);
    REXLOG_WARN("KK co-op story: guest world behind: {} expected={} received={} source={:016X} script={:016X} run={} record={} phase={}",
        why,ordered.Next(),e.number,e.source,e.script,e.run,e.record,unsigned(e.phase));
  }
  failed=true;
  behind=true;
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
bool LoopProgram(const uint8_t* b) {
  const auto block=Load32(b,0x82CCBF5C);
  return PoseAddress(block,24) && Load32(b,block+20)==kLoop;
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
      FinishRequest(it->second,"request refused","script allocation reused");
      Note("local identity retired",{},"script allocation reused; native continues");
      scripts.erase(it); it=scripts.end();
    }
    if (it==scripts.end()) {
      const auto matrix=Ptr(b,actor,12,64);
      const auto shape=Shape(b,vars);
      if (!matrix || !shape || scripts.size()>=4096) continue;
      const auto source=story::Source(shape,LoadF(b,matrix+48),LoadF(b,matrix+52),LoadF(b,matrix+56));
      if (!source) continue;
      Script s; s.binding={source,shape,actor,ai,vars};
      s.Baseline(Load32(b,vars+19812));
      // Cache the policy before native callbacks mutate their payloads. Mixed
      // chains stay native too: gating their detector would gate unsafe records.
      s.exact=SafeRequest(b,s);
      if (const auto run=runs.find(source); run!=runs.end()) s.run=run->second;
      it=scripts.emplace(actor,s).first;
    }
    // Include dormant and ambiguous objects. Never pick the first same-position script.
    bindings.push_back(it->second.binding);
  }
  next_census=Now()+1;
}
void World(const FlowView& v,uint64_t session_epoch) {
  if (epoch!=session_epoch || generation!=v.world.generation || level!=v.world.level) {
    for (auto& [actor,s] : scripts) FinishRequest(s,"request refused","world retired");
    epoch=session_epoch; generation=v.world.generation; level=v.world.level; host=v.host;
    number=request_number=last_request=last_loaded=0; scripts.clear(); bindings.clear(); runs.clear(); outgoing.clear();
    ordered.Reset(generation,level); behind=needs_move=false; failed=disconnected=recovery_pending=false;
    waiting_number=0; waiting_since=0;
    request_count=0; request_window=next_census=0;
    checksums={}; next_checksum=Now()+3; checksum_actor=0; checksum_sweep=false;
  }
}
bool Ensure(uint8_t* b) {
  if (!REXCVAR_GET(kk_coop_enabled) || !SessionInProcess()) { behind=needs_move=false; return false; }
  const auto v=SessionFlowSnapshot();
  if (!v.active || v.policy!=ChapterPolicy::Together ||
      !story::Observing(v.world.generation,v.world.level,v.phase==FlowPhase::Playing)) {
    behind=needs_move=false; return false;
  }
  const auto link=EnemyLinkSnapshot();
  if (!link.epoch) return false;
  World(v,link.epoch);
  if (SessionPrivateReload() && !failed) Refuse("private reload has no story snapshot");
  if (!link.connected && link.SessionConnected()) {
    static unsigned logged=0;
    if (logged++<4) REXLOG_INFO("KK co-op story: stale enemy snapshot, session connected; retaining continuity generation {} level {}",
        generation,level);
  }
  if (!link.SessionConnected()) { disconnected=true; if (!host) Refuse("connection interrupted; no script snapshot"); }
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
Script* ObservedScript(const uint8_t* b) {
  if (auto* s=Current(b)) return s;
  // A new/different local allocation or callback layout is local evidence,
  // not proof of divergence. Rebind only our own metadata.
  const auto actor=Load32(b,0x82CCBF6C);
  if (const auto it=scripts.find(actor); it!=scripts.end())
    FinishRequest(it->second,"request refused","script identity rebound");
  scripts.erase(actor); Census(b);
  auto* s=Current(b);
  Note("local script re-baselined",{},s ? "local identity rebound" : "identity unavailable; native continues");
  return s;
}
StoryEvent Event(const Script& s,StoryPhase phase,uint32_t record) {
  return {generation,level,0,s.binding.source,s.binding.script,s.run,record,phase};
}
void Flush() {
  while (!outgoing.empty() && SubmitStory(outgoing.front())) {
    const auto& m=outgoing.front();
    Note(m.story.phase==StoryPhase::Checksum ?
        (m.type==MessageType::StoryEvent ? "sample queued" : "reply queued") :
        (m.type==MessageType::StoryEvent ? "sent (reliable queued)" : "request sent"),m.story);
    outgoing.pop_front();
  }
}
bool Emit(StoryEvent e) {
  if (number==UINT32_MAX || outgoing.size()>=kStoryQueue) {
    Refuse("story outgoing overflow",e);
    Message abort; abort.type=MessageType::FlowAbort; abort.flow=SessionFlowSnapshot().world; SubmitFlowMessage(abort); return false;
  }
  e.number=number+1;
  if (!ValidStoryEvent(e)) { Refuse("invalid observed story event",e); return false; }
  ++number;
  Message m; m.type=MessageType::StoryEvent; m.epoch=epoch; m.story=e; outgoing.push_back(m); Flush();
  return true;
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
bool ReplaySafe(const uint8_t* b,const Script& s,uint32_t index) {
  if (index>=kStoryRecords) return false;
  const auto payload=s.binding.vars+992+180*index;
  const auto fn=Load32(b,s.binding.vars+924+180*index);
  return fn && (SafeAction(b,fn,payload) || (fn==kPivot && HeroPivot(b,payload)));
}
void Native(Script& s,const StoryEvent& e,const char* reason) {
  s.exact=false;
  Note("native fallback",e,reason);
}
void Converge(Script& s) {
  s.hints.Converge(s,[](const StoryEvent& e){ Note("native hint converged",e); });
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
  if (failed || s.run==UINT32_MAX || s.requested.run>=s.run+1 ||
      Load32(b,s.binding.vars+20)!=kPivot || !HeroPivot(b,payload)) return;
  const auto saved=ctx;
  uint8_t old[108]; std::memcpy(old,b+payload,sizeof old);
  ctx.r3.u32=payload; __imp__sub_8244F0E8(ctx,b);
  const bool entered=ctx.r3.u32!=0;
  std::memcpy(b+payload,old,sizeof old); ctx=saved;
  if (!entered) return;
  if (!s.requested.Begin(s.run+1)) return;
  if (request_number==UINT32_MAX || outgoing.size()>=kStoryQueue) { Refuse("request overflow"); return; }
  auto e=Event(s,StoryPhase::Armed,kDetectRecord); e.run=s.run+1; e.number=++request_number;
  Message m; m.type=MessageType::TriggerRequest; m.epoch=epoch; m.story=e; outgoing.push_back(m); Flush();
}
bool For(const StoryEvent* e,const Script& s,StoryPhase phase) {
  return e && e->source==s.binding.source && e->script==s.binding.script && e->phase==phase;
}
void Applied(const StoryEvent& e) {
  if (e.phase!=StoryPhase::Checksum) Note("applied",e);
  ordered.Applied(Now());
}
void OrderedFailure(uint8_t* b) {
  if (failed || !ordered.Behind()) return;
  const auto& error=ordered.Error();
  const auto& e=error.received;
  if (Trace()) {
    REXLOG_INFO("KK co-op story: ordered failure expected={} received={} reason={} source={:016X} script={:016X} run={} record={} phase={}",
        error.expected,e.number,story::Ordered::Reason(error.reason),e.source,e.script,e.run,e.record,unsigned(e.phase));
    if (const auto* bind=story::Match(e.source,e.script,bindings)) {
      const auto& s=scripts.at(bind->actor);
      const auto index=Load32(b,bind->vars+19812);
      REXLOG_INFO("KK co-op story: blocked local actor={:08X} ai={:08X} vars={:08X} run={} record={} detect-seen={} loop-seen={} detect-track={} loop-track={} detect-callback={:08X} record-callback={:08X}",
          bind->actor,bind->ai,bind->vars,s.run,index,s.saw_detect,s.saw_loop,Tracks(b,bind->ai,kDetect),Tracks(b,bind->ai,kLoop),
          Load32(b,bind->vars+20),index<=kStoryRecords ? Load32(b,bind->vars+924+180*index) : 0);
    } else Note("blocked identity unavailable",e,"missing/ambiguous script");
  }
  Refuse(story::Ordered::Reason(error.reason),e);
}
void ChecksumFrame(uint8_t* b,double now) {
  if (!host) {
    // Drain native hints immediately, even if that script never schedules.
    // Exact scripts alone may wait for their local detector/loop hook.
    for (unsigned budget=0;budget<kStoryQueue && !failed;++budget) {
      const auto* front=ordered.Front();
      if (!front) break;
      const auto e=*front;
      const auto* bind=story::Match(e.source,e.script,bindings);
      if (!bind || Vars(b,bind->ai,19824)!=bind->vars || Shape(b,bind->vars)!=bind->script) {
        Note("hint identity unavailable",e,"native continues; no comparable checksum");
        ordered.Applied(now); continue;
      }
      auto& s=scripts.at(bind->actor);
      if (e.phase==StoryPhase::Recovery) {
        Note("legacy recovery hint ignored",e,"only persistent checksum lag requests recovery");
        ordered.Applied(now); continue;
      }
      if (e.phase!=StoryPhase::Checksum) {
        if (s.exact) {
          if (waiting_number!=e.number) { waiting_number=e.number; waiting_since=now; }
          if (now-waiting_since<=5) break;
          Native(s,e,"exact hook did not progress; allow native scheduling before comparing checksums");
        }
        Note("native hint recorded",e);
        s.hints.Remember(e); Converge(s);
        ordered.Applied(now); continue;
      }
      auto reply=e; reply.run=s.run; reply.record=Load32(b,bind->vars+19812);
      if (!ValidStoryEvent(reply) || outgoing.size()>=kStoryQueue) break;
      Message m; m.type=MessageType::TriggerRequest; m.epoch=epoch; m.story=reply;
      outgoing.push_back(m); ordered.Applied(now);
      const auto before=s.lag.State(now);
      const auto stage=s.lag.Sample({e.run,e.record},{reply.run,reply.record},now);
      if (stage!=before) {
        const char* what=stage==story::Lag::Stage::Clear ? "native checksum converged" :
            stage==story::Lag::Stage::Move ? "persistent lag: move offered" :
            stage==story::Lag::Stage::Reload ? "lag after soft fix: checkpoint recovery offered" : "checksum lag grace started";
        Note(what,e,"guest must remain behind without advancing for more than 20 seconds");
      }
      if (reply.run!=e.run || reply.record!=e.record) Note("checksum differs (hint)",reply);
      else Note("checksum matched",reply);
    }
    return;
  }
  checksums.Expire(now);
  if (now<next_checksum && !checksum_sweep) return;
  checksum_sweep=true;
  // Bound each frame's work and share story backpressure. Checksums are cheap
  // samples, not another full object census and not recovery on lost observations.
  auto it=scripts.upper_bound(checksum_actor);
  for (unsigned budget=0;it!=scripts.end() && budget<16;++it,++budget) {
    if (outgoing.size()>=kStoryQueue/2 || number==UINT32_MAX) break;
    const auto& s=it->second;
    const auto* bind=story::Match(s.binding.source,s.binding.script,bindings);
    if (!bind || bind->actor!=it->first || Vars(b,bind->ai,19824)!=bind->vars || Shape(b,bind->vars)!=bind->script) {
      checksum_actor=it->first; continue;
    }
    auto e=Event(s,StoryPhase::Checksum,Load32(b,bind->vars+19812)); e.number=number+1;
    if (!ValidStoryEvent(e)) { checksum_actor=it->first; continue; }
    if (!checksums.Remember(e,now)) break;
    if (!Emit(e)) break;
    checksum_actor=it->first;
  }
  if (it==scripts.end()) { checksum_sweep=false; checksum_actor=0; next_checksum=now+3; }
}
} // namespace

bool StoryBehind() { return behind.load(); }
bool StoryNeedsMove() { return needs_move.load(); }
void StoryPartnerMoved() {
  const double now=Now();
  for (auto& [actor,s] : scripts) if (s.lag.State(now)==story::Lag::Stage::Move) {
    s.lag.Moved(now);
    Note("soft fix complete",Event(s,StoryPhase::Checksum,s.record),"native teleport; allow another 20 seconds of stationary checksum lag");
  }
  needs_move=false;
}
void StoryLoaded(uint8_t* b) {
  const auto v=SessionFlowSnapshot();
  if (!v.active || v.policy!=ChapterPolicy::Together || !v.world.generation || !v.world.level) return;
  const auto session_epoch=StorySessionEpoch();
  if (!session_epoch) return;
  World(v,session_epoch);
  for (auto& [actor,s] : scripts) FinishRequest(s,"request refused","level load invalidated request");
  if (last_loaded==v.world.generation) {
    recovery_pending=true;
    scripts.clear(); bindings.clear(); next_census=0;
    return;
  }
  last_loaded=v.world.generation;
  // Both natives ran freely during loading. Capture all current records before
  // Loaded/LoadRelease can release either world's first gameplay AI pass.
  scripts.clear(); bindings.clear(); runs.clear(); Census(b);
  REXLOG_INFO("KK co-op story: baseline role={} gen={} level={} scripts={} before load release",
      host ? "host" : "guest",generation,level,scripts.size());
}
void StoryGameFrame(uint8_t* b) {
  if (!Ensure(b)) {
    for (auto& [actor,s] : scripts) FinishRequest(s,"request refused","story world inactive");
    for (const auto& m : TakeStory()) if (m.type==MessageType::TriggerRequest && m.story.phase==StoryPhase::Armed) {
      Note("request received",m.story);
      Note("request refused",m.story,"story world inactive or loading");
    }
    return;
  }
  const double now=Now();
  if (recovery_pending) {
    recovery_pending=false;
    Refuse("private load or reconnect requires shared checkpoint recovery");
  }
  for (const auto& m : TakeStory()) {
    const auto& e=m.story;
    if (host && e.phase!=StoryPhase::Checksum) Note("request received",e);
    if (m.epoch!=epoch || e.generation!=generation || e.level!=level) {
      Note(host && e.phase!=StoryPhase::Checksum ? "request refused" : "refused",e,"stale world"); continue;
    }
    if ((host && m.type!=MessageType::TriggerRequest) || (!host && m.type!=MessageType::StoryEvent)) {
      Note(host ? "request refused" : "refused",e,"wrong direction"); continue;
    }
    if (!host) { if (ordered.Push(e,now)) Note("received",e); continue; }
    if (e.phase==StoryPhase::Checksum) {
      const auto result=checksums.Compare(e);
      if (result==story::Checksums::Result::Mismatch) {
        Note("checksum differs (hint)",e,"guest monitors persistent lag; no immediate recovery");
      } else if (result==story::Checksums::Result::Match) Note("checksum matched",e);
      continue;
    }
    if (e.number<=last_request) { Note("request refused",e,"duplicate or old request number"); continue; }
    last_request=e.number;
    if (now-request_window>=1) { request_window=now; request_count=0; }
    if (++request_count>4) { Note("request refused",e,"rate limit"); continue; }
    const auto* bind=story::Match(e.source,e.script,bindings);
    if (!bind) { Note("request refused",e,"missing/ambiguous identity"); continue; }
    auto& s=scripts.at(bind->actor);
    if (s.request) { Note("request refused",e,"activation already pending"); continue; }
    if (failed || s.run==UINT32_MAX || e.run!=s.run+1) { Note("request refused",e,"stale activation run"); continue; }
    if (!SafeRequest(b,s)) { Note("request refused",e,"unsafe predicate or action chain"); continue; }
    // Recheck a fresh partner pose and native volume when this script is scheduled.
    s.request=e; s.request_until=now+.5; Note("request pending",e);
  }
  if (host) for (auto& [actor,s] : scripts) if (s.request && now>s.request_until)
    FinishRequest(s,"request refused","script not scheduled before request deadline");
  ChecksumFrame(b,now);
  if (!host && SessionFlowSnapshot().phase==FlowPhase::Playing) {
    // Present heads are consumed as hints or given a native fallback above.
    // Only a genuine reliable-delivery gap uses Ordered's short deadline.
    if (!ordered.Front()) ordered.Tick(now);
    OrderedFailure(b);
    bool move=false, reload=false;
    for (auto& [actor,s] : scripts) {
      Converge(s);
      const auto stage=s.lag.State(now);
      move|=stage==story::Lag::Stage::Move;
      reload|=stage==story::Lag::Stage::Reload;
    }
    needs_move=move; behind=failed || reload;
  }
  Flush();
}

// Return true when co-op handled this call, including a suppressed local advance.
bool StoryDetect(PPCContext& ctx,uint8_t* b) {
  if (!Ensure(b)) return false;
  auto* s=ObservedScript(b);
  if (!s) return false; // Unknown scripts retain native behavior.
  if (!s->saw_detect) { s->saw_detect=true; Note("native detect scheduled",Event(*s,StoryPhase::Armed,kDetectRecord)); }
  const auto flag=Load32(b,0x82CCBF60),vars=s->binding.vars;
  if (!PoseAddress(flag)) return false;
  const auto before=Load32(b,flag);
  if (host || !s->exact) {
    if (host && s->request) {
      if (Now()>s->request_until) FinishRequest(*s,"request refused","request deadline elapsed");
      else if (failed || !story::DetectIdle(before) || s->run==UINT32_MAX || s->request->run!=s->run+1)
        FinishRequest(*s,"request refused","script already armed or activation changed");
      else if (!SafeRequest(b,*s)) FinishRequest(*s,"request refused","action chain changed or became unsafe");
      else {
        const auto link=EnemyLinkSnapshot();
        if (!link.connected || !link.partner || link.partner->level!=level)
          FinishRequest(*s,"request refused","fresh guest position unavailable");
        else if (!NativeInside(ctx,b,Load32(b,vars+92),*link.partner))
          FinishRequest(*s,"request refused","guest outside native trigger volume");
        else forced_pivot=vars+88;
      }
    }
    __imp__sub_82476848(ctx,b); forced_pivot=0;
    if (failed) return true;
    const auto after=Load32(b,flag);
    if (story::DetectArmed(before,after)) {
      if (s->run==UINT32_MAX) Refuse("script run overflow");
      else {
        ++s->run; runs[s->binding.source]=s->run;
        if (host) {
          FinishRequest(*s,"request applied","native detection armed");
          Emit(Event(*s,StoryPhase::Armed,kDetectRecord));
        }
      }
    } else if (story::DetectStarted(before,ctx.r3.u32,LoopProgram(b))) {
      s->record=Load32(b,vars+19812); s->started=s->run;
      if (host) Emit(Event(*s,StoryPhase::Started,kDetectRecord));
    }
    if (!host) Converge(*s);
    return true;
  }
  ctx.r3.u32=0;
  if (failed || ordered.Behind()) return true;
  const auto* front=ordered.Front();
  if (For(front,*s,StoryPhase::Armed)) {
    const auto e=*front;
    if (!story::DetectIdle(before) || e.run!=s->run+1 || Load32(b,vars+20)!=kPivot || !HeroPivot(b,vars+88) || Load32(b,vars+8)) {
      Native(*s,e,"detection callback or state is not replay-safe");
      // Re-enter once under the native policy to observe any immediate Armed
      // transition too. Returning to the raw hook would lose that activation.
      return StoryDetect(ctx,b);
    }
    forced_pivot=vars+88; __imp__sub_82476848(ctx,b); forced_pivot=0;
    if (Load32(b,flag)==1) { s->run=e.run; runs[s->binding.source]=s->run; Applied(e); }
    else Native(*s,e,"native detection did not arm");
  } else if (For(front,*s,StoryPhase::Started)) {
    const auto e=*front;
    if (before!=1 || e.run!=s->run) { Native(*s,e,"detection start state mismatch"); return StoryDetect(ctx,b); }
    __imp__sub_82476848(ctx,b);
    if (story::DetectStarted(before,ctx.r3.u32,LoopProgram(b))) { s->record=Load32(b,vars+19812); s->started=s->run; Applied(e); }
    else Native(*s,e,"native script start deferred");
  } else if (story::DetectIdle(before)) Request(ctx,b,*s);
  return true;
}

bool StoryLoop(PPCContext& ctx,uint8_t* b) {
  if (!Ensure(b)) return false;
  auto* s=ObservedScript(b);
  if (!s) return false;
  if (!s->saw_loop) { s->saw_loop=true; Note("native loop scheduled",Event(*s,StoryPhase::Advance,Load32(b,s->binding.vars+19812))); }
  const auto vars=s->binding.vars,before=Load32(b,vars+19812);
  if (host || !s->exact) {
    __imp__sub_82454690(ctx,b);
    if (failed) return true;
    const auto after=Load32(b,vars+19812);
    s->started=s->run;
    if (ctx.r3.u32==5) s->finished=s->run;
    if (!s->ObserveLoop(before,after)) {
      Note("local script re-baselined",Event(*s,StoryPhase::Checksum,after),"loop jump or unobserved activation");
      return true; // No synthetic callbacks. The next checksum compares actual guest state.
    }
    if (!host) { Converge(*s); return true; }
    for (uint32_t i=before;i<after;++i) {
      Emit(Event(*s,StoryPhase::Advance,i));
    }
    if (ctx.r3.u32==5) Emit(Event(*s,StoryPhase::Finished,after));
    return true;
  }
  ctx.r3.u32=0;
  if (failed || ordered.Behind()) return true;
  // Reproduce only the documented SSLoop record bookkeeping. Never invoke an
  // address from the packet; all calls below are fixed, locally validated natives.
  for (unsigned budget=0;budget<kStoryRecords;++budget) {
    const auto* front=ordered.Front();
    if (!front || front->source!=s->binding.source || front->script!=s->binding.script || !story::LoopEvent(*front)) break;
    const auto e=*front;
    const auto index=Load32(b,vars+19812);
    if (e.run!=s->run || e.record!=index || index>kStoryRecords) { Native(*s,e,"record/run mismatch"); return StoryLoop(ctx,b); }
    const auto record=vars+920+180*index,payload=record+72,fn=Load32(b,record+4);
    if (e.phase==StoryPhase::Finished) {
      if (fn) { Native(*s,e,"terminator mismatch"); return StoryLoop(ctx,b); }
      ctx.r3.u32=Load32(b,0x82CC9B0C); __imp__sub_826F9E70(ctx,b); ctx.r3.u32=5;
      s->finished=s->run; Applied(e); return true;
    }
    if (e.phase!=StoryPhase::Advance || !ReplaySafe(b,*s,index)) {
      Native(*s,e,"unsupported callback runs natively"); return StoryLoop(ctx,b);
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
    if (!ok || Load32(b,vars+19812)!=index) {
      Native(*s,e,"native callback deferred or changed index"); return true;
    }
    Store32(b,vars+19812,index+1); Store32(b,vars+19816,0);
    s->record=index+1;
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
