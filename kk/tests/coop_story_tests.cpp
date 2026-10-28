// C41 source handoff. Claude builds/runs this SDK/game-free executable.
#include "coop_story.h"
#include "coop_session.h"
#include "coop_runtime.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace kk::coop;
namespace {
void Check(bool ok,const char* why) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
StoryEvent Event(uint32_t number=1,StoryPhase phase=StoryPhase::Armed) {
  return {2,203,number,0x0123456789abcdefull,0xfedcba9876543210ull,3,
      phase==StoryPhase::Armed || phase==StoryPhase::Started ? kStoryRecords : 7,phase};
}
Message Packet(MessageType type=MessageType::StoryEvent,uint32_t number=1) {
  Message m; m.type=type; m.epoch=17; m.story=Event(number); return m;
}
std::vector<uint8_t> Wire(const Message& m) { std::vector<uint8_t> bytes; Check(Encode(m,bytes),"valid fixture encodes"); return bytes; }
void Codec() {
  static_assert(kProtocolVersion==31);
  static_assert(uint16_t(MessageType::StoryEvent)==29 && uint16_t(MessageType::TriggerRequest)==30);
  for (auto phase : {StoryPhase::Armed,StoryPhase::Started,StoryPhase::Advance,StoryPhase::Finished,StoryPhase::Recovery,StoryPhase::Checksum}) {
    for (auto type : {MessageType::StoryEvent,MessageType::TriggerRequest}) {
      if (type==MessageType::TriggerRequest && !ValidStoryRequest(phase)) continue;
      auto m=Packet(type); m.story=Event(0x12345678,phase);
      auto bytes=Wire(m); Message out;
      Check(bytes.size()==kHeaderBytes+37 && bytes[5]==kProtocolVersion && bytes[7]==uint8_t(type),"bounded v19 story wire");
      Check(bytes[kHeaderBytes+8]==0x12 && bytes[kHeaderBytes+11]==0x78 &&
          bytes[kHeaderBytes+12]==1 && bytes[kHeaderBytes+19]==0xef,"network byte order, including 64-bit identity");
      Check(Decode(bytes,out) && out.story==m.story && out.epoch==17 && Wire(out)==bytes,"exact canonical story round trip");
      for (size_t size=0;size<bytes.size();++size) {
        Message sentinel; sentinel.sequence=0x76543210;
        Check(!Decode(std::span(bytes).first(size),sentinel) && sentinel.sequence==0x76543210,"every truncation preserves output");
      }
      auto bad=bytes; bad.push_back(0); ++bad[23]; Check(!Decode(bad,out),"trailing payload rejected");
      bad=bytes; bad.back()=255; Check(!Decode(bad,out),"unknown phase rejected");
    }
  }
  for (unsigned field=0;field<8;++field) {
    auto m=Packet();
    switch (field) {
      case 0: m.story.generation=0; break;
      case 1: m.story.level=100001; break;
      case 2: m.story.number=0; break;
      case 3: m.story.source=0; break;
      case 4: m.story.script=0; break;
      case 5: m.story.run=0; break;
      case 6: m.story.record=kStoryRecords+1; break;
      case 7: m.story.phase=StoryPhase(0); break;
    }
    std::vector<uint8_t> out; Check(!Encode(m,out),"invalid field rejected by encoder");
  }
  auto m=Packet(MessageType::TriggerRequest); m.story=Event(1,StoryPhase::Advance);
  std::vector<uint8_t> out; Check(!Encode(m,out),"request cannot ask for a record callback or arbitrary phase");
  m=Packet(); m.story.record=0; Check(!Encode(m,out),"detection uses reserved record identity");
  m.story=Event(1,StoryPhase::Advance); m.story.record=kStoryRecords;
  Check(!Encode(m,out),"terminator cannot be an advance");
  m.story.phase=StoryPhase::Finished; Check(Encode(m,out),"last bounded terminator is legal");
  for (size_t offset : {size_t(0),size_t(4),size_t(8),size_t(12),size_t(20),size_t(28),size_t(32)}) {
    auto bytes=Wire(Packet());
    const size_t count=offset==12 || offset==20 ? 8 : 4;
    for (size_t i=0;i<count;++i) bytes[kHeaderBytes+offset+i]=0;
    Message sentinel; sentinel.sequence=19;
    Check(!Decode(bytes,sentinel) && sentinel.sequence==19,"decoder also rejects zero scalar/identity fields atomically");
  }
}
void HostActivation() {
  Check(story::FileSource(123,0x12345678) && story::FileSource(123,0x12345678)!=story::FileSource(123,0x12345679),"script BIG keys distinguish colocated native controllers");
  Check(!story::FileSource(123,0) && !story::FileSource(0,0x12345678),"unproven script resources cannot acquire authored identity");
  story::ActivationGate gate;
  Check(!gate.CanArm(1),"guest volume alone cannot arm a shared detector");
  auto e=Event(); e.run=2;
  gate.Observe(e);
  Check(gate.CanArm(1) && !gate.CanArm(2),"host activation permits exactly one native activation");
  e.phase=StoryPhase::Checksum; e.run=8; gate.Observe(e);
  Check(gate.CanArm(1),"idle checksum cannot invent an activation");
  e.phase=StoryPhase::Started; e.run=3; gate.Observe(e);
  Check(gate.CanArm(2) && !gate.CanArm(1),"missing intermediate activation is never skipped");
  e.phase=StoryPhase::Armed; e.run=2; gate.Observe(e);
  Check(gate.CanArm(2),"stale activation cannot rewind the host boundary");
  Check(!gate.CanArm(UINT32_MAX),"activation run cannot wrap");
  gate={}; Check(!gate.CanArm(2),"native rebind discards previous activation authority");
  story::Hints hints;
  e=Event(); e.run=3; e.record=8; e.phase=StoryPhase::SnapshotIdle; hints.Remember(e);
  Check(!hints.Passed(3,7),"idle snapshot cannot authorize a cinematic completion");
  e.phase=StoryPhase::Checksum; hints.Remember(e);
  Check(!hints.Passed(3,7),"checksum alone cannot authorize a native completion");
  e.phase=StoryPhase::Advance; e.record=7; hints.Remember(e);
  Check(hints.Passed(3,7) && !hints.Passed(2,7) && !hints.Passed(3,8),"native completion needs the exact host activation and boundary");
}
void Ordering() {
  story::Ordered queue; queue.Reset(2,203);
  Check(queue.Push(Event(2,StoryPhase::Started),1) && !queue.Front(),"native schedule cannot bypass earlier script event");
  Check(queue.Push(Event(1),1.1) && queue.Front()->number==1,"out-of-order delivery reassembles");
  Check(!queue.Push(Event(1),2) && !queue.Behind(),"identical duplicate is idempotent");
  queue.Applied(2); Check(queue.Front() && queue.Front()->number==2,"strict order across script phases");
  queue.Applied(2); Check(!queue.Front() && queue.Next()==3,"consumed exactly once");
  Check(!queue.Push(Event(1),2.1) && !queue.Behind(),"reliable retransmission cannot replay consumed event");
  auto e=Event(3); e.generation=1; Check(!queue.Push(e,3),"old checkpoint visit rejected");
  e.generation=3; Check(!queue.Push(e,3),"future visit does not reset active world");
  e.generation=2; e.level=201; Check(!queue.Push(e,3),"another level rejected");
  Check(queue.Push(Event(4),3),"gap held"); queue.Tick(8.01);
  Check(queue.Behind() && !queue.Front(),"missing event offers recovery after five seconds");
  Check(queue.Error().reason==story::Ordered::Failure::Gap && queue.Error().expected==3 &&
      queue.Error().received==Event(4),"gap keeps expected number and real received script after clearing queue");
  queue.Reset(2,203); Check(queue.Push(Event(),1),"reset ready");
  e=Event(); ++e.script; Check(!queue.Push(e,1.1) && queue.Behind(),"conflicting duplicate refuses rather than selecting one");
  Check(queue.Error().reason==story::Ordered::Failure::Conflict && queue.Error().expected==1 &&
      queue.Error().received==e,"conflict preserves actual conflicting identity");
  queue.Reset(2,203); Check(!queue.Push(Event(1+kStoryQueue),1) && queue.Behind(),"bounded reorder window");
  Check(queue.Error().reason==story::Ordered::Failure::Window && queue.Error().received.number==1+kStoryQueue,
      "window overflow is distinct from missing event timeout");
  queue.Reset(2,203);
  for (uint32_t i=1;i<=kStoryQueue;++i) Check(queue.Push(Event(i),1),"bounded ordering capacity");
  Check(!queue.Push(Event(1),2) && !queue.Behind(),"duplicate at full capacity remains harmless");
  queue.Reset(2,203); Check(queue.Push(Event(),1),"waiting callback"); queue.Tick(6.01);
  Check(queue.Behind(),"unmatched or unscheduled native script times out too");
  Check(queue.Error().reason==story::Ordered::Failure::ApplyTimeout && queue.Error().expected==1 &&
      queue.Error().received==Event(),"present head timeout never reports a missing packet or default event");
  queue.Reset(2,203);
  auto other=Event(2,StoryPhase::Advance); ++other.source; ++other.script;
  Check(queue.Push(Event(),1) && queue.Push(other,1),"two scripts arrive in one ordered batch");
  queue.Applied(5.9); queue.Tick(6.1);
  Check(!queue.Behind() && queue.Front() && *queue.Front()==other,
      "time waiting behind an earlier native script does not consume the next script's apply deadline");
  queue.Tick(10.91);
  Check(queue.Behind() && queue.Error().received==other,"new head still times out after five seconds without progress");
  queue.Reset(2,203); Check(queue.Push(Event(2),1),"gap with no progress");
  Check(queue.Push(Event(3),5.9),"later traffic arrives during gap"); queue.Tick(6.01);
  Check(queue.Behind() && queue.Error().reason==story::Ordered::Failure::Gap && queue.Error().expected==1,
      "checksum or later-script traffic cannot keep a real gap alive indefinitely");
  queue.Reset(2,203); Check(queue.Push(Event(2),1) && queue.Push(Event(),5.9),"missing head arrives just before gap deadline");
  queue.Tick(6.1);
  Check(!queue.Behind() && queue.Front()->number==1,"a repaired delivery gap gets its own native apply deadline");
  queue.Reset(3,203); e=Event(); e.generation=3;
  Check(queue.Error().reason==story::Ordered::Failure::None,"new world clears old failure evidence");
  Check(queue.Push(e,10) && !queue.Behind() && queue.Front(),"same-level shared reload clears old visit and starts from one");
  queue.Refuse(); Check(!queue.Front() && !queue.Push(e,11),"refusal holds all subsequent local story");
}
void Requests() {
  // 8271D1D8/826F9E70 store -1 for a newly selected native program. The
  // initial Armed and Started events must survive that scheduler convention.
  Check(story::DetectIdle(UINT32_MAX) && story::DetectIdle(0) && !story::DetectIdle(1),
      "native idle sentinel permits guest requests and replay without admitting an armed detector");
  Check(story::DetectArmed(UINT32_MAX,1) && story::DetectArmed(0,1) && !story::DetectArmed(1,1),
      "host emits one Armed transition when native detection starts from minus one");
  Check(story::DetectStarted(1,5,true) && !story::DetectStarted(1,0,true) && !story::DetectStarted(1,5,false),
      "Started follows the actual native loop-program switch rather than its overwritten state word");
  story::Cursor authority,replica; authority.Baseline(0); replica.Baseline(0);
  story::Ordered stream; stream.Reset(2,203);
  auto armed=Event(1); armed.run=authority.run+1;
  if (story::DetectArmed(UINT32_MAX,1)) { ++authority.run; Check(stream.Push(armed,1),"authority publishes native-sentinel Armed"); }
  if (story::DetectIdle(UINT32_MAX) && stream.Front()) { replica.run=stream.Front()->run; stream.Applied(1.1); }
  auto started=armed; started.number=2; started.phase=StoryPhase::Started;
  if (story::DetectStarted(1,5,true)) Check(stream.Push(started,1.2),"authority publishes Started after native state becomes minus one again");
  Check(stream.Front() && stream.Front()->phase==StoryPhase::Started && replica.run==authority.run,
      "guest receives the activation before any cross-script loop advances");
  stream.Applied(1.3);
  auto advance=Event(3,StoryPhase::Advance); advance.run=authority.run; advance.record=0;
  Check(stream.Push(advance,1.4) && stream.Front()->run==replica.run,
      "first loop event has the shared activation run instead of waiting on a gated detector");
  story::RequestLatch pending;
  Check(pending.Begin(2),"first local entry submits next activation");
  for (unsigned frame=0;frame<3600;++frame)
    Check(!pending.Begin(2),"reliable pending or refused activation is not resubmitted each frame/second");
  Check(pending.Begin(3) && !pending.Begin(2),"only a newer native activation permits a new request");
  pending={}; Check(pending.Begin(2),"shared world reset permits the activation in the new generation");
  Check(!pending.Begin(0),"zero run is never a valid request");
}
void Identity() {
  const auto source=story::Source(55,-48,146,12);
  Check(source && source==story::Source(55,-48,146,12),"same authored script/position matches across heaps");
  Check(source!=story::Source(56,-48,146,12) && source!=story::Source(55,-47,146,12),"different callback layout or position differs");
  Check(!story::Source(0,0,0,0) && !story::Source(55,std::numeric_limits<float>::quiet_NaN(),0,0) &&
      !story::Source(55,1000000,0,0),"invalid identity evidence refused");
  std::array<story::Binding,1> a{{{source,55,0x40001000,0x40002000,0x40003000}}};
  std::array<story::Binding,1> b{{{source,55,0x50001000,0x50002000,0x50003000}}};
  Check(story::Match(source,55,a) && story::Match(source,55,b),"binding depends on stable evidence, not peer addresses");
  std::array<story::Binding,2> duplicate{{a[0],b[0]}};
  Check(!story::Match(source,55,duplicate),"same-position identical scripts are ambiguous");
  duplicate[1].source++; duplicate[1].vars=duplicate[0].vars;
  Check(!story::Match(source,55,duplicate),"aliased mutable script vars refused");
  Check(!story::Match(source,56,a),"schema mismatch cannot invoke a different local script");
}
void LoadBaselineAndChecksums() {
  story::Cursor host,guest;
  story::Ordered events; events.Reset(2,203);
  // SSLoop can be entered during uni_go before generation is published. No
  // observation, request or refusal is legal at any of these boundaries.
  constexpr std::array<std::array<uint32_t,3>,4> deferred{{{0,0,1},{0,203,1},{2,0,1},{2,203,0}}};
  for (const auto& world : deferred) {
    if (story::Observing(world[0],world[1],world[2]!=0)) host.ObserveLoop(7,0);
    Check(!host.run && events.Next()==1 && !events.Behind(),"zero generation/level and loading defer all observations");
  }
  Check(story::Observing(2,203,true),"only valid Playing world observes");
  for (uint32_t loaded : {0u,7u,kStoryRecords}) {
    host.Baseline(loaded); guest.Baseline(loaded);
    Check(host.run==guest.run && host.record==guest.record && events.Next()==1 && !events.Front(),
        "load baseline accepts inactive, already looping and completed records without events");
    Check(host.ObserveLoop(loaded,loaded),"first unchanged SSLoop after barrier is observed, not an unobserved activation");
  }
  host.Baseline(7); guest.Baseline(7);
  for (unsigned i=0;i<3;++i) {
    Check(!host.ObserveLoop(7,0),"host record reversal asks for a local re-baseline");
    events.Refuse(true);
    Check(!events.Behind() && events.Next()==1,"host is never behind and emits no recovery for missing observations");
  }
  Check(host.ObserveLoop(0,1),"authority continues observing after re-baseline");
  Check(!host.ObserveLoop(3,4) && host.record==4,"unobserved native index change is locally re-baselined");
  events.Refuse(true);
  Check(!events.Behind() && host.ObserveLoop(4,5),"host continues after missing observations without recovery");

  story::Checksums checksums;
  auto sample=Event(1,StoryPhase::Checksum); sample.run=guest.run; sample.record=guest.record;
  Check(checksums.Remember(sample,3),"host captures periodic per-script record sample");
  Check(events.Push(sample,3.1),"checksum shares strict native event order");
  auto advance=Event(2,StoryPhase::Advance); advance.run=guest.run; advance.record=guest.record;
  Check(events.Push(advance,3.1) && events.Front()->phase==StoryPhase::Checksum,"later advance cannot overtake checksum boundary");
  Check(!story::LoopEvent(*events.Front()),"SSLoop defers checksum to frame handler instead of refusing it as a native callback");
  auto reply=*events.Front(); reply.run=guest.run; reply.record=guest.record;
  events.Applied(3.2); guest.record++;
  Check(events.Front() && story::LoopEvent(*events.Front()),"SSLoop may resume after checksum is consumed");
  Check(checksums.Compare(reply)==story::Checksums::Result::Match,
      "reply compares captured state even after either native world advances");
  Check(checksums.Compare(reply)==story::Checksums::Result::Ignored,"duplicate checksum reply is harmless");
  events.Applied(3.3);
  auto run_sample=sample; run_sample.number=2;
  Check(checksums.Remember(run_sample,4),"activation run is part of checksum evidence");
  reply=run_sample; reply.run++;
  Check(checksums.Compare(reply)==story::Checksums::Result::Mismatch,"same index in a different run is actual divergence");
  events.Refuse(true);
  Check(!events.Behind(),"even a confirmed guest mismatch never marks host behind");
  sample.number=3; sample.record=guest.record;
  Check(checksums.Remember(sample,6),"next periodic checksum captures new state");
  reply=sample; reply.generation++;
  Check(checksums.Compare(reply)==story::Checksums::Result::Ignored,"wrong generation cannot claim a mismatch");
  reply=sample; reply.script++;
  Check(checksums.Compare(reply)==story::Checksums::Result::Ignored,"wrong identity cannot claim a mismatch");
  reply=sample; reply.record++;
  Check(events.Push(sample,6.1),"guest receives comparison for current boundary");
  Check(checksums.Compare(reply)==story::Checksums::Result::Mismatch && !events.Behind(),
      "one differing checksum is diagnostic evidence, not immediate recovery");
  sample.number=4; Check(checksums.Remember(sample,9),"outstanding sample tracked");
  checksums.Expire(19.01);
  Check(checksums.Compare(sample)==story::Checksums::Result::Ignored,"missing observations/replies expire without declaring divergence");
  checksums={};
  for (uint32_t n=1;n<=kStoryQueue/2;++n) { sample.number=n; Check(checksums.Remember(sample,20),"checksum memory bounded"); }
  sample.number=kStoryQueue/2+1;
  Check(!checksums.Remember(sample,20),"checksum pressure defers samples rather than evicting native events");
  checksums={}; sample.number=1; sample.generation++;
  Check(checksums.Compare(sample)==story::Checksums::Result::Ignored,"load reset discards old checksum samples");
}
void LenientConvergence() {
  story::Progress local; local.Baseline(0);
  story::Hints hints;
  story::Ordered events; events.Reset(2,203);
  auto armed=Event(1); armed.run=2;
  auto started=armed; started.number=2; started.phase=StoryPhase::Started;
  auto advance=armed; advance.number=3; advance.phase=StoryPhase::Advance; advance.record=0;
  auto finished=advance; finished.number=4; finished.phase=StoryPhase::Finished; finished.record=1;
  for (const auto& e : {armed,started,advance,finished}) Check(events.Push(e,1),"native script hints arrive in transport order");
  while (const auto* e=events.Front()) { hints.Remember(*e); events.Applied(1.1); }
  events.Tick(40);
  Check(!events.Behind() && !events.Front() && hints.Pending()==4,
      "unsupported native script consumes hints without holding global replay or a five-second apply deadline");
  Check(hints.Passed(2,0) && !hints.Passed(1,0) && !hints.Passed(3,0) && !hints.Passed(2,1),
      "host completion permits only an actually passed boundary in the same activation");
  unsigned converged=0;
  auto collect=[&](const StoryEvent&){ ++converged; };
  hints.Converge(local,collect); Check(!converged,"host hints never claim the guest's callback ran");
  ++local.run; hints.Converge(local,collect);
  Check(converged==1 && hints.Pending()==3,"native Armed converges independently of a native start");
  local.started=local.run; hints.Converge(local,collect);
  Check(converged==2,"native start converges without invoking any received callback");
  local.ObserveLoop(0,1); hints.Converge(local,collect);
  Check(converged==3 && hints.Pending()==1,"native record advance converges but does not invent completion");
  local.finished=local.run; hints.Converge(local,collect);
  Check(converged==4 && !hints.Pending(),"native completion converges");
  hints.Remember(advance); hints.Converge(local,collect);
  Check(converged==5 && !hints.Pending(),"guest that reaches the record before the packet converges immediately");
  ++local.run; local.record=0; hints.Remember(finished); hints.Converge(local,collect);
  Check(converged==6,"later natural activation also proves the old record passed");
  local={};
  for (uint32_t n=0;n<kStoryQueue+40;++n) { advance.number=n+1; hints.Remember(advance); }
  Check(hints.Pending()==kStoryQueue,"unconverged hints are bounded without blocking native execution");
  hints={}; Check(!hints.Pending(),"new world discards old hint evidence");
}
void PersistentLagAndSoftFix() {
  EnemyLink stale;
  stale.connected=false; stale.epoch=17; stale.session_state=SessionState::Connected;
  Check(stale.SessionConnected(),"stale enemy snapshot during native stall is not a story disconnect");
  stale.session_state=SessionState::Lost;
  Check(!stale.SessionConnected(),"actual lost transport still invalidates story continuity");
  stale.session_state=SessionState::Ended;
  Check(!stale.SessionConnected(),"ended session still requires story retirement");
  using Stage=story::Lag::Stage;
  story::Cursor authority{2,9},guest{2,1};
  story::Lag lag;
  Check(lag.Sample(authority,guest,0)==Stage::Waiting,"first behind checksum starts a grace period");
  for (double t : {3.,6.,9.,12.,15.,18.,20.})
    Check(lag.Sample(authority,guest,t)==Stage::Waiting,"stationary lag must persist more than twenty seconds");
  Check(lag.Sample(authority,guest,21)==Stage::Move,"persistent lag offers soft catch-up first");
  for (double t=24;t<120;t+=3)
    Check(lag.Sample(authority,guest,t)==Stage::Move,"waiting for player to accept a move never escalates to reload");
  lag.Moved(120);
  for (double t : {120.,123.,126.,129.,132.,135.,138.,140.})
    Check(lag.Sample(authority,guest,t)==Stage::AfterMove,"native teleport grants a second full convergence interval");
  Check(lag.Sample(authority,guest,141)==Stage::Reload,"only stationary lag after completed soft fix offers checkpoint recovery");
  guest.record=2;
  Check(lag.Sample(authority,guest,144)==Stage::AfterMove,"partial progress renews grace but retains the completed soft fix");
  for (double t : {147.,150.,153.,156.,159.,162.,164.})
    Check(lag.Sample(authority,guest,t)==Stage::AfterMove,"another NPC wait after partial progress gets a full twenty-second grace");
  Check(lag.Sample(authority,guest,165)==Stage::Reload,"partial soft-fix progress cannot strand the guest in another manual move cycle");
  guest=authority;
  Check(lag.Sample(authority,guest,166)==Stage::Clear,"natural convergence clears all recovery state");
  ++guest.record;
  Check(lag.Sample(authority,guest,150)==Stage::Clear,"guest ahead of captured host boundary never counts as divergence");
  guest={1,100}; authority={2,0};
  Check(lag.Sample(authority,guest,153)==Stage::Waiting,"activation run orders a reset record index");
  guest={2,0};
  Check(lag.Sample(authority,guest,156)==Stage::Clear,"native activation catches up after index reset");
  guest={2,0}; authority={2,9};
  Check(lag.Sample(authority,guest,160)==Stage::Waiting,"new mismatch starts independently");
  Check(lag.Sample(authority,guest,185)==Stage::Waiting,"a missing checksum interval cannot establish persistent lag");
  for (double t : {188.,191.,194.,197.,200.}) lag.Sample(authority,guest,t);
  Check(lag.Sample(authority,guest,206)==Stage::Move,"continuous comparison resumes after a gap");
  Check(lag.State(217)==Stage::Clear,"stale comparison cannot offer recovery during camera/menu/network stalls");
  lag={}; lag.Moved(0);
  Check(lag.Sample(authority,guest,0)==Stage::Waiting,"unsolicited move cannot bypass the first grace period");
  for (double t : {3.,6.,9.,12.,15.,18.,21.}) lag.Sample(authority,guest,t);
  guest.record++;
  Check(lag.Sample(authority,guest,24)==Stage::Waiting,"guest making slow progress gets a fresh twenty-second window");
  Check(lag.Sample(authority,guest,std::numeric_limits<double>::quiet_NaN())==Stage::Waiting,
      "invalid timing evidence never escalates recovery");
}
StoryEvent Marker(StoryPhase phase,uint32_t number=100,uint64_t token=99,uint32_t count=2) {
  return {2,203,number,token,phase==StoryPhase::SnapshotRequest ? 1ull : uint64_t(count)+1,1,0,phase};
}
void PrivateSnapshots() {
  const auto begin=Marker(StoryPhase::SnapshotBegin);
  auto a=Event(101,StoryPhase::SnapshotLoop); a.record=4;
  auto b=a; b.number=102; ++b.source; b.phase=StoryPhase::SnapshotFinished; b.record=9;
  auto idle=a; idle.phase=StoryPhase::SnapshotIdle;
  const auto end=Marker(StoryPhase::SnapshotEnd,103);
  for (const auto& e : {Marker(StoryPhase::SnapshotRequest),begin,idle,a,b,end}) {
    auto m=Packet(e.phase==StoryPhase::SnapshotRequest ? MessageType::TriggerRequest : MessageType::StoryEvent);
    m.story=e; const auto wire=Wire(m); Message decoded;
    Check(wire.size()==kHeaderBytes+37 && Decode(wire,decoded) && decoded.story==e,"bounded snapshot phases preserve exact cursors and token");
    for (size_t n=0;n<wire.size();++n) Check(!Decode(std::span(wire).first(n),decoded),"snapshot truncation refused");
    m.type=m.type==MessageType::TriggerRequest ? MessageType::StoryEvent : MessageType::TriggerRequest;
    std::vector<uint8_t> invalid; Check(!Encode(m,invalid),"snapshot requests and responses cannot reverse direction");
  }
  story::Snapshot snapshot; snapshot.Request(2,203,99);
  auto wrong=begin; wrong.source=98;
  Check(!snapshot.Begin(wrong) && !snapshot.Complete(),"stale private-load token cannot begin a snapshot");
  Check(snapshot.Begin(begin) && !snapshot.Begin(begin),"one token accepts exactly one census begin");
  Check(snapshot.Add(a) && !snapshot.Add(a) && !snapshot.End(end) && !snapshot.Complete(),"duplicate or partial census never commits");
  wrong=b; wrong.level=204;
  Check(!snapshot.Add(wrong) && snapshot.Add(b),"snapshot entries cannot cross levels");
  wrong=end; wrong.script=2;
  Check(!snapshot.End(wrong) && snapshot.End(end) && snapshot.Complete() && !snapshot.Add(b),"only exact complete count commits once");
  snapshot.Request(2,203,100);
  Check(!snapshot.Complete() && !snapshot.Begin(begin),"another private reload drops previous partial/native cursor evidence");
  auto empty=Marker(StoryPhase::SnapshotBegin,200,100,0);
  Check(snapshot.Begin(empty),"bounded empty census has explicit framing");
  empty.phase=StoryPhase::SnapshotEnd; ++empty.number;
  Check(snapshot.End(empty),"empty census still requires End");
  wrong=begin; wrong.script=kStorySnapshotScripts+2;
  Check(!ValidStoryEvent(wrong),"oversized script census refused by codec");
  snapshot.Request(2,203,101);
  const auto maximum=Marker(StoryPhase::SnapshotBegin,300,101,kStorySnapshotScripts);
  Check(snapshot.Begin(maximum),"maximum census count is bounded explicitly");
  for (uint32_t i=0;i<kStorySnapshotScripts;++i) {
    auto entry=a; entry.source=i+1;
    Check(snapshot.Add(entry),"maximum census stores each unique identity");
  }
  auto extra=a; extra.source=kStorySnapshotScripts+1;
  Check(!snapshot.Add(extra),"census never exceeds declared storage bound");
  auto maximum_end=maximum; maximum_end.phase=StoryPhase::SnapshotEnd;
  Check(snapshot.End(maximum_end),"maximum census commits completely");
  story::Ordered ordered; ordered.Reset(2,203);
  Check(ordered.ResumeAfter(begin) && ordered.Next()==101 && !ordered.Push(Event(99),1),"snapshot boundary skips only pre-reload ordered history");
  Check(ordered.Push(a,1) && ordered.Push(b,1) && ordered.Push(end,1),"new snapshot stream resumes contiguously");
  for (unsigned n=0;n<3;++n) ordered.Applied(1.1);
  Check(ordered.Next()==104 && !ordered.Behind(),"live hints continue after snapshot end without sequence gaps");
  auto overflow=begin; overflow.number=UINT32_MAX;
  Check(!ordered.ResumeAfter(overflow),"snapshot cannot wrap ordered event numbers");
  story::Hints hints; hints.Remember(a);
  Check(hints.Passed(a.run,3) && !hints.Passed(a.run,4) && !hints.Passed(a.run+1,3),"snapshot authorizes only passed records in the same activation");
  story::Progress progress; progress.run=a.run; progress.started=a.run; progress.record=3;
  Check(!progress.Reached(a),"cursor snapshot does not claim local callbacks already ran");
  progress.record=4; Check(progress.Reached(a),"native or safe replay progress reaches the captured cursor");
  Check(story::CanReplaySnapshotPrefix(true,2,a) && story::CanReplaySnapshotPrefix(false,0,a),
      "safe restore can advance an active prefix or start a fresh local program");
  Check(!story::CanReplaySnapshotPrefix(true,5,a) && !story::CanReplaySnapshotPrefix(false,4,a) &&
      !story::CanReplaySnapshotPrefix(false,9,a),"restore cannot rewind a native cursor or restart an already advanced program");
  auto finished=a; finished.phase=StoryPhase::SnapshotFinished;
  const auto can_cover=[&](bool loaded,bool omitted,bool scheduled,bool looping,uint32_t index,uint32_t run,bool terminator,const StoryEvent& end) {
    return story::CanCoverCheckpointPrefix(loaded,omitted,scheduled,looping,index,run,terminator,end);
  };
  Check(can_cover(true,true,false,false,0,1,true,finished),"new checkpoint may cover a loader-omitted completed prefix without replay");
  Check(!can_cover(false,true,false,false,0,1,true,finished) &&
      !can_cover(true,false,false,false,0,1,true,finished),"ordinary loads and programs present in scheduled AI cannot cover missing native work");
  Check(!can_cover(true,true,true,false,0,1,true,finished) &&
      !can_cover(true,true,false,true,0,1,true,finished),"scheduled detectors and active loops retain native convergence checks");
  Check(!can_cover(true,true,false,false,1,1,true,finished) &&
      !can_cover(true,true,false,false,0,finished.run+1,true,finished),"checkpoint coverage cannot swallow partially executed work or rewind a newer activation");
  Check(!can_cover(true,true,false,false,0,1,false,finished) &&
      !can_cover(true,true,false,false,0,1,true,a),"local terminator and explicit host completion are both mandatory");
  Check(story::CheckpointPrefixCovers(finished,a),"completed checkpoint history covers the captured prefix of the same activation");
  auto later=a; ++later.record;
  Check(!story::CheckpointPrefixCovers(finished,later),"coverage cannot extend beyond proved host completion");
  later=a; ++later.run; Check(!story::CheckpointPrefixCovers(finished,later),"new native activation retires checkpoint coverage");
  later=a; ++later.generation; Check(!story::CheckpointPrefixCovers(finished,later),"checkpoint history cannot cross shared generations");
  later=a; ++later.source; Check(!story::CheckpointPrefixCovers(finished,later),"checkpoint coverage never substitutes another actor identity");
}
struct Transport final : ICoopTransport {
  Transport* peer=nullptr;
  std::deque<std::vector<uint8_t>> incoming;
  bool blocked=false;
  unsigned story_sent=0;
  bool Send(std::span<const uint8_t> bytes,bool reliable) override {
    Message m; Check(Decode(bytes,m),"session sends valid wire");
    if (IsStoryMessage(m.type)) {
      Check(reliable,"story and requests always reliable");
      if (blocked) return false;
      ++story_sent;
    }
    if (peer) peer->incoming.emplace_back(bytes.begin(),bytes.end()); return true;
  }
  bool Receive(std::vector<uint8_t>& out) override { if (incoming.empty()) return false; out=std::move(incoming.front()); incoming.pop_front(); return true; }
  void Poll(double) override {}
  TransportState State() const override { return TransportState::Ready; }
  TransportStats Stats() const override { return {}; }
  void Close() override {}
};
void TransportAndMailbox() {
  Transport ht,gt; ht.peer=&gt; gt.peer=&ht;
  SessionConfig hc; hc.enabled=true; hc.role=Role::Host; hc.epoch=17;
  hc.compatibility={kProtocolVersion,"test","same","same"};
  auto gc=hc; gc.role=Role::Guest; gc.epoch=0;
  CoopSession host(hc,ht),guest(gc,gt);
  for (int i=0;i<20;++i) { host.Tick(i*.01); guest.Tick(i*.01); }
  Check(host.State()==SessionState::Connected && guest.State()==SessionState::Connected,"real v19 session handshake");
  StoryMailbox hm,gm;
  Check(hm.Submit(Packet()),"host game-thread submits copied event");
  hm.Exchange(host); ht.blocked=true; host.Tick(.21); guest.Tick(.21); gm.Exchange(guest);
  Check(gm.Take().empty(),"transport backpressure holds reliable event");
  ht.blocked=false; host.Tick(.22); guest.Tick(.22); gm.Exchange(guest);
  auto received=gm.Take(); Check(received.size()==1 && received[0].story==Event(),"worker hands same event to guest game thread");
  auto request=Packet(MessageType::TriggerRequest);
  Check(gm.Submit(request),"guest submits request"); gm.Exchange(guest); guest.Tick(.23); host.Tick(.23); hm.Exchange(host);
  received=hm.Take(); Check(received.size()==1 && received[0].type==MessageType::TriggerRequest,"host receives request reliably");
  auto checksum=Packet(); checksum.story.phase=StoryPhase::Checksum; checksum.story.record=7;
  Check(hm.Submit(checksum),"host checksum uses existing reliable mailbox");
  hm.Exchange(host); host.Tick(.231); guest.Tick(.231); gm.Exchange(guest);
  received=gm.Take(); Check(received.size()==1 && received[0].story==checksum.story,"ordered checksum reaches guest on real session");
  checksum.type=MessageType::TriggerRequest; checksum.story.record=8;
  Check(gm.Submit(checksum),"guest may reply with its actual differing index");
  gm.Exchange(guest); guest.Tick(.232); host.Tick(.232); hm.Exchange(host);
  received=hm.Take(); Check(received.size()==1 && received[0].story.record==8 && received[0].story.phase==StoryPhase::Checksum,
      "differing checksum returns to authority without becoming a trigger request");
  request.story=Event(2);
  Check(gm.Submit(request),"real activation request follows checksum reply on same worker");
  gm.Exchange(guest); guest.Tick(.233); host.Tick(.233); hm.Exchange(host);
  received=hm.Take(); Check(received.size()==1 && received[0].story.phase==StoryPhase::Armed && received[0].story.number==2,
      "host receives actual Armed request separately from phase-six diagnostic replies");
  auto snapshot_request=Packet(MessageType::TriggerRequest); snapshot_request.story=Marker(StoryPhase::SnapshotRequest);
  Check(gm.Submit(snapshot_request),"private snapshot request enters existing game-thread mailbox");
  gm.Exchange(guest); guest.Tick(.234); host.Tick(.234); hm.Exchange(host);
  received=hm.Take(); Check(received.size()==1 && received[0].story==snapshot_request.story,"host receives the private-load token reliably");
  auto snapshot_begin=Packet(); snapshot_begin.story=Marker(StoryPhase::SnapshotBegin);
  Check(hm.Submit(snapshot_begin),"host snapshot response uses normal authoritative stream");
  hm.Exchange(host); host.Tick(.235); guest.Tick(.235); gm.Exchange(guest);
  received=gm.Take(); Check(received.size()==1 && received[0].story==snapshot_begin.story,"snapshot boundary returns through real session and mailbox");
  Check(!host.QueueStory(request) && !guest.QueueStory(Packet()),"outgoing authority inversion rejected");
  auto wrong=Packet(); wrong.epoch=18; Check(!host.QueueStory(wrong),"outgoing stale epoch rejected");
  // Inject packets directly to ensure receive checks are independent of sender checks.
  ht.incoming.push_back(Wire(Packet())); host.Tick(.24); Check(host.TakeStory().empty(),"host ignores guest-forged story authority");
  gt.incoming.push_back(Wire(request)); guest.Tick(.24); Check(guest.TakeStory().empty(),"guest ignores forged host requests");
  wrong=Packet(); wrong.epoch=18; gt.incoming.push_back(Wire(wrong)); guest.Tick(.25);
  Check(guest.TakeStory().empty(),"incoming old session epoch rejected");
  for (uint32_t i=0;i<kStoryQueue;++i) Check(hm.Submit(Packet(MessageType::StoryEvent,i+2)),"bounded mailbox accepts capacity");
  Check(!hm.Submit(Packet()),"mailbox never silently grows beyond bound");
  hm.Clear(); Check(hm.Submit(Packet()),"clear removes stale pending events");
  guest.End(.3); gm.Exchange(guest); Check(gm.Take().empty(),"disconnect clears mailbox");
  auto old=hc.compatibility; --old.protocol;
  Check(CheckCompatibility(hc.compatibility,old)==RejectReason::Protocol,"v12 peers rejected at handshake");
}
}
int main() { HostActivation(); Codec(); Ordering(); Requests(); Identity(); LoadBaselineAndChecksums(); LenientConvergence(); PersistentLagAndSoftFix(); PrivateSnapshots(); TransportAndMailbox(); std::puts("co-op story tests passed"); }
