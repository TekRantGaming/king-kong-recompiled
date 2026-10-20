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
REX_EXTERN(__imp__sub_82501A30);
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
  bool checkpoint_omitted=false;
  std::optional<StoryEvent> checkpoint_finished,checkpoint_covered;
  std::deque<StoryEvent> restore;
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
story::Snapshot snapshot;
std::deque<StoryEvent> snapshot_out;
std::vector<StoryEvent> snapshot_unresolved;
uint64_t snapshot_token=0;
double snapshot_deadline=0,last_snapshot_request=-10,snapshot_recovery_time=0;
bool snapshot_waiting=false;
std::atomic<bool> behind{false};
std::atomic<bool> needs_move{false};
uint32_t waiting_number=0;
double waiting_since=0;
double checkpoint_loaded_at=0;
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
void TraceLayout(const uint8_t* b,const Script& s) {
#if defined(KK_DEV_TOOLS)
  static const uint64_t selected=[] {
    const char* value=std::getenv("KK_DEV_STORY_LAYOUT");
    return value ? std::strtoull(value,nullptr,16) : 0ull;
  }();
  if (!selected || s.binding.source!=selected) return;
  const auto vars=s.binding.vars;
  REXLOG_INFO("KK dev story layout: source={:016X} script={:016X} actor={:08X} vars={:08X} detect={:08X}",
      s.binding.source,s.binding.script,s.binding.actor,vars,Load32(b,vars+20));
  for (uint32_t i=0;i<=kStoryRecords;++i) {
    const auto fn=Load32(b,vars+924+180*i),p=vars+992+180*i;
    REXLOG_INFO("KK dev story record: source={:016X} index={} fn={:08X} args={:08X},{:08X},{:08X},{:08X} flags={:08X} local={}",
        selected,i,fn,Load32(b,p+4),Load32(b,p+8),Load32(b,p+12),Load32(b,p+16),Load32(b,p+84),Load32(b,p+100));
    if (!fn) break;
  }
#endif
}
void TraceLag(const uint8_t* b,const Script& s,const StoryEvent& e) {
  if (!Trace()) return;
  const auto vars=s.binding.vars,index=Load32(b,vars+19812);
  const auto p=vars+992+180*std::min(index,kStoryRecords);
  REXLOG_INFO("KK co-op story lag detail: source={:016X} script={:016X} host={}/{} local={}/{} detect={:08X} callback={:08X} args={:08X},{:08X},{:08X},{:08X} flags={:08X} local-word={} loop={} exact={}",
      e.source,e.script,e.run,e.record,s.run,index,Load32(b,vars+20),Load32(b,p-68),
      Load32(b,p+4),Load32(b,p+8),Load32(b,p+12),Load32(b,p+16),Load32(b,p+84),Load32(b,p+100),s.saw_loop,s.exact);
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
std::optional<bool> Scheduled(const uint8_t* b,uint32_t actor) {
  const auto world=Load32(b,0x828E8388);
  if (!PoseAddress(world,308)) return {};
  const auto begin=Load32(b,world+304),end=Load32(b,world+292);
  if (!PoseAddress(begin) || !PoseAddress(end) || end<=begin || (end-begin)%8 || end-begin>8*20000) return {};
  bool hero=false,found=false;
  for (auto p=begin;p<end;p+=8) {
    const auto entry=Load32(b,p);
    hero|=entry==Load32(b,0x82CCBF7C); found|=entry==actor;
  }
  // An empty/partially populated loader table is not evidence of omission.
  return hero ? std::optional<bool>{found} : std::nullopt;
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
      TraceLayout(b,it->second);
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
    snapshot={}; snapshot_out.clear(); snapshot_unresolved.clear(); snapshot_token=0;
    snapshot_waiting=false; snapshot_deadline=snapshot_recovery_time=0; last_snapshot_request=-10; checkpoint_loaded_at=0;
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
  if (SessionPrivateReload() && host && !failed) Refuse("host private reload has no authority snapshot");
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
  if (!e.number) s.restore.clear(); // A failed bounded restore waits for a newer private checkpoint.
  Note("native fallback",e,reason);
}
void Converge(Script& s) {
  s.hints.Converge(s,[](const StoryEvent& e){ Note("native hint converged",e); });
}
void RequestSnapshot() {
  if (host || request_number==UINT32_MAX || snapshot_token==UINT64_MAX || outgoing.size()>=kStoryQueue) {
    Refuse("private snapshot request unavailable"); return;
  }
  ++snapshot_token; snapshot.Request(generation,level,snapshot_token);
  snapshot_unresolved.clear();
  snapshot_waiting=true; snapshot_deadline=Now()+30;
  failed=false; behind=needs_move=false; ordered.Reset(generation,level);
  StoryEvent e{generation,level,++request_number,snapshot_token,1,1,0,StoryPhase::SnapshotRequest};
  Message m; m.type=MessageType::TriggerRequest; m.epoch=epoch; m.story=e;
  outgoing.push_back(m); Flush();
  REXLOG_INFO("KK co-op story: private snapshot requested generation {} level {} token {}",generation,level,snapshot_token);
}
void CaptureSnapshot(const uint8_t* b,const StoryEvent& request) {
  unsigned safe=0;
  for (const auto& [actor,s] : scripts) {
    const auto* bind=story::Match(s.binding.source,s.binding.script,bindings);
    if (!bind || bind->actor!=actor || Vars(b,bind->ai,19824)!=bind->vars || Shape(b,bind->vars)!=bind->script) continue;
    const auto index=Load32(b,bind->vars+19812);
    if (index>kStoryRecords || !s.run) continue;
    const auto phase=Tracks(b,bind->ai,kLoop) ? StoryPhase::SnapshotLoop :
        s.finished>=s.run ? StoryPhase::SnapshotFinished : StoryPhase::SnapshotIdle;
    snapshot_out.push_back(Event(s,phase,index));
    if (s.exact && SafeRequest(b,s)) ++safe;
  }
  StoryEvent marker{generation,level,0,request.source,snapshot_out.size()+1,1,0,StoryPhase::SnapshotBegin};
  snapshot_out.push_front(marker); marker.phase=StoryPhase::SnapshotEnd; snapshot_out.push_back(marker);
  REXLOG_INFO("KK co-op story: snapshot captured scripts={} replay-safe={} token={} generation={} level={}",marker.script-1,safe,request.source,generation,level);
}
void PumpSnapshot() {
  for (unsigned budget=0;host && !snapshot_out.empty() && budget<16 && outgoing.size()<kStoryQueue/2;++budget) {
    if (!Emit(snapshot_out.front())) break;
    snapshot_out.pop_front();
  }
}
bool CheckpointCovers(const uint8_t* b,const Script& s,const StoryEvent& e) {
  return s.checkpoint_covered && s.checkpoint_omitted && !s.saw_detect && !s.saw_loop &&
      Load32(b,s.binding.vars+19812)==0 &&
      (Load32(b,s.binding.actor+4)&0x400000) && Scheduled(b,s.binding.actor)==false && !Tracks(b,s.binding.ai,kLoop) &&
      story::CheckpointPrefixCovers(*s.checkpoint_covered,e);
}
void CoverCheckpointPrefix(const uint8_t* b,Script& s) {
  if (!s.checkpoint_omitted || !s.checkpoint_finished || s.checkpoint_covered) return;
  const auto& e=*s.checkpoint_finished;
  const auto* bind=story::Match(e.source,e.script,bindings);
  if (!bind || bind->actor!=s.binding.actor || Vars(b,bind->ai,19824)!=bind->vars || Shape(b,bind->vars)!=bind->script) return;
  if (!story::CanCoverCheckpointPrefix(s.checkpoint_omitted,
      (Load32(b,s.binding.actor+4)&0x400000) && Scheduled(b,s.binding.actor)==false,
      s.saw_detect || s.saw_loop,Tracks(b,s.binding.ai,kLoop),Load32(b,s.binding.vars+19812),s.run,
      e.record<=kStoryRecords && !Load32(b,s.binding.vars+924+180*e.record),e)) return;
  // The loader, not a received callback, omitted this pre-checkpoint program.
  // Align only our activation metadata for a possible future native activation.
  // Keep the raw native record and all callback/actor state untouched.
  s.run=e.run; runs[e.source]=e.run; s.lag={}; s.checkpoint_covered=e;
  REXLOG_INFO("KK co-op story: checkpoint prefix covered source={:016X} host={}/{} native={}/{} flags={:08X}; omitted from populated native AI table at newer checkpoint load and still unscheduled, no callbacks replayed",
      e.source,e.run,e.record,s.run,Load32(b,s.binding.vars+19812),Load32(b,s.binding.actor+4));
}
bool SnapshotReached(const uint8_t* b,const Script& s,const StoryEvent& e) {
  if (CheckpointCovers(b,s,e)) return true;
  if (s.run>e.run) return true;
  if (s.run!=e.run || Load32(b,s.binding.vars+19812)<e.record) return false;
  if (e.phase==StoryPhase::SnapshotFinished) return s.finished>=e.run;
  return s.started>=e.run || s.finished>=e.run;
}
void RestoreSnapshot(uint8_t* b) {
  snapshot_waiting=false; failed=false; behind=needs_move=false;
  snapshot_recovery_time=Now()+20;
  unsigned restored=0,matched=0;
  for (const auto& [identity,e] : snapshot.Entries()) {
    const auto* bind=story::Match(e.source,e.script,bindings);
    if (!bind || Vars(b,bind->ai,19824)!=bind->vars || Shape(b,bind->vars)!=e.script) {
      snapshot_unresolved.push_back(e); continue;
    }
    ++matched;
    auto& s=scripts.at(bind->actor); s.restore.clear(); s.lag={};
    const auto index=Load32(b,bind->vars+19812);
    if (index>kStoryRecords) { snapshot_unresolved.push_back(e); continue; }
    const bool looping=Tracks(b,bind->ai,kLoop);
    if (e.phase==StoryPhase::SnapshotFinished) { s.checkpoint_finished=e; CoverCheckpointPrefix(b,s); }
    if (e.phase==StoryPhase::SnapshotIdle) {
      if (!looping) { s.run=e.run; runs[e.source]=e.run; s.record=index; }
      continue;
    }
    // Metadata activation counters restart on a private native load. Align
    // the currently running local program, never rewind its native record.
    if (looping || (e.phase==StoryPhase::SnapshotFinished && index==e.record && !Load32(b,bind->vars+924+180*index))) {
      s.run=e.run; s.started=e.run; s.record=index; runs[e.source]=s.run;
      if (!looping) s.finished=e.run;
    }
    if (SnapshotReached(b,s,e)) continue;
    REXLOG_INFO("KK co-op story: private snapshot gap source={:016X} host={}/{} phase={} local={}/{} actor-kind={:08X} actor-flags={:08X} schedule-byte={:02X} detect-track={} loop-track={} omitted-at-load={} scheduled-now={} vars0={:08X} vars4={:08X} vars8={:08X} vars12={:08X} vars16={:08X}",
        e.source,e.run,e.record,unsigned(e.phase),s.run,index,Load32(b,bind->actor),Load32(b,bind->actor+4),b[bind->actor+31],Tracks(b,bind->ai,kDetect),looping,s.checkpoint_omitted,Scheduled(b,bind->actor).value_or(true),
        Load32(b,bind->vars),Load32(b,bind->vars+4),Load32(b,bind->vars+8),Load32(b,bind->vars+12),Load32(b,bind->vars+16));
    snapshot_unresolved.push_back(e);
    s.hints.Remember(e);
    // Only the existing all-safe Jack/door/activation chain can be replayed.
    // Mixed actions, NPC messages, inventory, combat and cinematics stay native.
    if (!s.exact || !SafeRequest(b,s) || !story::CanReplaySnapshotPrefix(looping,index,e)) continue;
    const auto add=[&](StoryPhase phase,uint32_t record) {
      auto replay=e; replay.number=0; replay.phase=phase; replay.record=record; s.restore.push_back(replay);
    };
    uint32_t first=index;
    if (!looping) {
      s.run=e.run-1; s.record=0; runs[e.source]=s.run; first=0;
      add(StoryPhase::Armed,kDetectRecord); add(StoryPhase::Started,kDetectRecord);
    }
    for (auto i=first;i<e.record;++i) add(StoryPhase::Advance,i);
    if (e.phase==StoryPhase::SnapshotFinished) add(StoryPhase::Finished,e.record);
    if (!s.restore.empty()) ++restored;
  }
  REXLOG_INFO("KK co-op story: private snapshot complete scripts={} matched={} safe-replays={} unresolved={} token={}; native gaps await a newer host checkpoint",
      snapshot.Entries().size(),matched,restored,snapshot_unresolved.size(),snapshot_token);
  // Give ordinary native execution/replay the same twenty-second grace as
  // checksum convergence before considering another private catch-up.
}
void SnapshotProgress(uint8_t* b) {
  for (auto& [actor,s] : scripts) CoverCheckpointPrefix(b,s);
  const auto before=snapshot_unresolved.size();
  std::erase_if(snapshot_unresolved,[&](const auto& e) {
    const auto* bind=story::Match(e.source,e.script,bindings);
    return bind && Vars(b,bind->ai,19824)==bind->vars && Shape(b,bind->vars)==bind->script && SnapshotReached(b,scripts.at(bind->actor),e);
  });
  if (before && snapshot_unresolved.empty()) REXLOG_INFO("KK co-op story: private snapshot fully converged token={} generation={} level={}",snapshot_token,generation,level);
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
  if (!e.number) {
    if (const auto* bind=story::Match(e.source,e.script,bindings)) {
      auto& replay=scripts.at(bind->actor).restore;
      if (!replay.empty() && replay.front()==e) replay.pop_front();
    }
    return;
  }
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
      if (StorySnapshotEntry(e.phase)) {
        if (!snapshot_waiting || !snapshot.Add(e)) { Refuse("invalid private snapshot entry",e); break; }
        ordered.Applied(now); continue;
      }
      if (e.phase==StoryPhase::SnapshotEnd) {
        if (!snapshot_waiting || !snapshot.End(e)) { Refuse("incomplete private snapshot",e); break; }
        ordered.Applied(now); RestoreSnapshot(b); continue;
      }
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
        if (e.phase==StoryPhase::Finished) { s.checkpoint_finished=e; CoverCheckpointPrefix(b,s); }
        if (s.exact && !snapshot_waiting) {
          if (waiting_number!=e.number) { waiting_number=e.number; waiting_since=now; }
          if (now-waiting_since<=5) break;
          Native(s,e,"exact hook did not progress; allow native scheduling before comparing checksums");
        }
        Note("native hint recorded",e);
        s.hints.Remember(e); Converge(s);
        ordered.Applied(now); continue;
      }
      if (snapshot_waiting) { ordered.Applied(now); continue; }
      auto reply=e; reply.run=s.run; reply.record=Load32(b,bind->vars+19812);
      if (!ValidStoryEvent(reply) || outgoing.size()>=kStoryQueue) break;
      Message m; m.type=MessageType::TriggerRequest; m.epoch=epoch; m.story=reply;
      outgoing.push_back(m); ordered.Applied(now);
      const auto before=s.lag.State(now);
      const bool checkpoint_covered=CheckpointCovers(b,s,e);
      if (checkpoint_covered) s.lag={};
      const auto stage=checkpoint_covered ? story::Lag::Stage::Clear :
          s.lag.Sample({e.run,e.record},{reply.run,reply.record},now);
      if (stage!=before) {
        const char* what=checkpoint_covered ? "checksum history covered by checkpoint" :
            stage==story::Lag::Stage::Clear ? "native checksum converged" :
            stage==story::Lag::Stage::Move ? "persistent lag: move offered" :
            stage==story::Lag::Stage::Reload ? "lag after soft fix: private catch-up ready" : "checksum lag grace started";
        Note(what,e,"guest must remain behind without advancing for more than 20 seconds");
        if (stage!=story::Lag::Stage::Clear) TraceLag(b,s,e);
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
bool StoryAnyBehind() {
  const double now=Now();
  if (host || snapshot_waiting) return false;
  for (const auto& [actor,s] : scripts) if (s.lag.State(now)!=story::Lag::Stage::Clear) return true;
  return !snapshot_unresolved.empty();
}
bool StoryCatchupReady() {
  const double now=Now();
  if (host || snapshot_waiting) return false;
  for (const auto& [actor,s] : scripts) if (s.lag.State(now)==story::Lag::Stage::Reload) return true;
  return false;
}
void StoryHostCheckpoint() {
  // Supply a current comparison during the checkpoint offer window, even if
  // the previous sweep happened just before the checkpoint fired.
  next_checksum=0;
}
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
  checkpoint_loaded_at=SessionCheckpointCatchup() ? Now() : 0;
  for (auto& [actor,s] : scripts) FinishRequest(s,"request refused","level load invalidated request");
  if (last_loaded==v.world.generation) {
    scripts.clear(); bindings.clear(); next_census=0;
    if (!host) {
      runs.clear(); Census(b);
      RequestSnapshot();
    }
    else recovery_pending=true;
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
  // uni_go returns before the first native AI initialization pass. Capture
  // authored checkpoint omissions only during its short settling window, and
  // only from a populated table plus the dispatcher's explicit disabled bit
  // (G.32:8271D610). No detector/loop may have scheduled since this load.
  if (checkpoint_loaded_at>0 && now>=checkpoint_loaded_at && now-checkpoint_loaded_at<=2) {
    for (auto& [actor,s] : scripts) if (!s.checkpoint_omitted && !s.saw_detect && !s.saw_loop &&
        (Load32(b,actor+4)&0x400000) && Load32(b,s.binding.vars+19812)==0 && !Tracks(b,s.binding.ai,kLoop) &&
        Scheduled(b,actor)==false && Vars(b,s.binding.ai,19824)==s.binding.vars && Shape(b,s.binding.vars)==s.binding.script) {
      s.checkpoint_omitted=true;
      REXLOG_INFO("KK co-op story: native checkpoint baseline source={:016X} flags={:08X} record=0 omitted from populated AI table after initialization; no detect/loop scheduled",
          s.binding.source,Load32(b,actor+4));
    }
  }
  if (snapshot_waiting && now>snapshot_deadline) {
    snapshot_waiting=false; Refuse("private snapshot timed out; awaiting private catch-up");
  }
  if (recovery_pending) {
    recovery_pending=false;
    Refuse("private load or reconnect requires checkpoint catch-up");
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
    if (!host) {
      if (e.phase==StoryPhase::SnapshotBegin) {
        if (snapshot_waiting && snapshot.Begin(e) && ordered.ResumeAfter(e)) Note("private snapshot begin",e);
        else Note("private snapshot ignored",e,"unsolicited, duplicate or stale token");
        continue;
      }
      if (snapshot_waiting && !snapshot.Receiving()) continue;
      if (ordered.Push(e,now)) Note("received",e);
      continue;
    }
    if (e.phase==StoryPhase::SnapshotRequest) {
      if (e.number<=last_request || now-last_snapshot_request<3 || !snapshot_out.empty()) {
        Note("request refused",e,"duplicate, rate-limited or snapshot already pending"); continue;
      }
      last_request=e.number; last_snapshot_request=now; CaptureSnapshot(b,e); continue;
    }
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
  PumpSnapshot(); ChecksumFrame(b,now);
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
    SnapshotProgress(b);
    needs_move=move; behind=failed || reload || (!snapshot_unresolved.empty() && now>snapshot_recovery_time);
  }
  Flush();
}

// Return true when co-op handled this call, including a suppressed local advance.
bool StoryDetect(PPCContext& ctx,uint8_t* b) {
  if (!Ensure(b)) return false;
  if (!host && snapshot_waiting) return false;
  auto* s=ObservedScript(b);
  if (!s) return false; // Unknown scripts retain native behavior.
  s->checkpoint_omitted=false; s->checkpoint_covered.reset(); s->checkpoint_finished.reset();
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
  const auto* front=s->restore.empty() ? ordered.Front() : &s->restore.front();
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
  if (!host && snapshot_waiting) return false;
  auto* s=ObservedScript(b);
  if (!s) return false;
  s->checkpoint_omitted=false; s->checkpoint_covered.reset(); s->checkpoint_finished.reset();
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
    const auto* front=s->restore.empty() ? ordered.Front() : &s->restore.front();
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
bool ConvergeFlagWait(uint8_t* b,uint32_t payload) {
  if (!Ensure(b) || host || failed) return false;
  auto* s=Current(b);
  if (!s || s->exact || !s->saw_loop) return false;
  const auto index=Load32(b,s->binding.vars+19812);
  if (index>=kStoryRecords || payload!=s->binding.vars+992+180*index ||
      Load32(b,payload-68)!=0x82501A30 || Load32(b,payload+84)!=0x100) return false;
  // This native variant only reads one non-player object's flag. With +8..+20
  // null, success has no completion flags to consume (.65:18510-18799).
  // Never extend to Jack's volumes, NPC movement waits or action callbacks.
  const auto target=Load32(b,payload+4);
  if (!PoseAddress(target,32) || target==Load32(b,0x82CCBF7C) || !Ptr(b,target,24,42)) return false;
  for (uint32_t off=8;off<=20;off+=4) if (Load32(b,payload+off)) return false;
  const auto stage=s->lag.State(Now());
  if ((stage!=story::Lag::Stage::Move && stage!=story::Lag::Stage::AfterMove && stage!=story::Lag::Stage::Reload) ||
      !s->hints.Passed(s->run,index)) return false;
  Note("native flag wait converged",Event(*s,StoryPhase::Advance,index),
      "host passed same activation; single-target read-only 0x100 wait stalled >20 seconds");
  return true;
}
} // namespace kk::coop

REX_HOOK_RAW(sub_82476848) {
  if (!kk::coop::StoryDetect(ctx,base)) __imp__sub_82476848(ctx,base);
}
REX_HOOK_RAW(sub_82501A30) {
  const auto payload=ctx.r3.u32;
  __imp__sub_82501A30(ctx,base);
  if (!ctx.r3.u32 && kk::coop::ConvergeFlagWait(base,payload)) ctx.r3.u32=1;
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
