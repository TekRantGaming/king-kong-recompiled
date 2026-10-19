// C41 source handoff. Claude builds/runs this SDK/game-free executable.
#include "coop_story.h"
#include "coop_session.h"
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
  static_assert(kProtocolVersion==11);
  static_assert(uint16_t(MessageType::StoryEvent)==29 && uint16_t(MessageType::TriggerRequest)==30);
  for (auto phase : {StoryPhase::Armed,StoryPhase::Started,StoryPhase::Advance,StoryPhase::Finished,StoryPhase::Recovery,StoryPhase::Checksum}) {
    for (auto type : {MessageType::StoryEvent,MessageType::TriggerRequest}) {
      if (type==MessageType::TriggerRequest && !ValidStoryRequest(phase)) continue;
      auto m=Packet(type); m.story=Event(0x12345678,phase);
      auto bytes=Wire(m); Message out;
      Check(bytes.size()==kHeaderBytes+37 && bytes[5]==11 && bytes[7]==uint8_t(type),"bounded v11 story wire");
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
void Ordering() {
  story::Ordered queue; queue.Reset(2,203);
  Check(queue.Push(Event(2,StoryPhase::Started),1) && !queue.Front(),"native schedule cannot bypass earlier script event");
  Check(queue.Push(Event(1),1.1) && queue.Front()->number==1,"out-of-order delivery reassembles");
  Check(!queue.Push(Event(1),2) && !queue.Behind(),"identical duplicate is idempotent");
  queue.Applied(); Check(queue.Front() && queue.Front()->number==2,"strict order across script phases");
  queue.Applied(); Check(!queue.Front() && queue.Next()==3,"consumed exactly once");
  Check(!queue.Push(Event(1),2.1) && !queue.Behind(),"reliable retransmission cannot replay consumed event");
  auto e=Event(3); e.generation=1; Check(!queue.Push(e,3),"old checkpoint visit rejected");
  e.generation=3; Check(!queue.Push(e,3),"future visit does not reset active world");
  e.generation=2; e.level=201; Check(!queue.Push(e,3),"another level rejected");
  Check(queue.Push(Event(4),3),"gap held"); queue.Tick(8.01);
  Check(queue.Behind() && !queue.Front(),"missing event offers recovery after five seconds");
  queue.Reset(2,203); Check(queue.Push(Event(),1),"reset ready");
  e=Event(); ++e.script; Check(!queue.Push(e,1.1) && queue.Behind(),"conflicting duplicate refuses rather than selecting one");
  queue.Reset(2,203); Check(!queue.Push(Event(1+kStoryQueue),1) && queue.Behind(),"bounded reorder window");
  queue.Reset(2,203);
  for (uint32_t i=1;i<=kStoryQueue;++i) Check(queue.Push(Event(i),1),"bounded ordering capacity");
  Check(!queue.Push(Event(1),2) && !queue.Behind(),"duplicate at full capacity remains harmless");
  queue.Reset(2,203); Check(queue.Push(Event(),1),"waiting callback"); queue.Tick(6.01);
  Check(queue.Behind(),"unmatched or unscheduled native script times out too");
  queue.Reset(3,203); e=Event(); e.generation=3;
  Check(queue.Push(e,10) && !queue.Behind() && queue.Front(),"same-level shared reload clears old visit and starts from one");
  queue.Refuse(); Check(!queue.Front() && !queue.Push(e,11),"refusal holds all subsequent local story");
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
  events.Applied(); guest.record++;
  Check(events.Front() && story::LoopEvent(*events.Front()),"SSLoop may resume after checksum is consumed");
  Check(checksums.Compare(reply)==story::Checksums::Result::Match,
      "reply compares captured state even after either native world advances");
  Check(checksums.Compare(reply)==story::Checksums::Result::Ignored,"duplicate checksum reply is harmless");
  events.Applied();
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
  if (checksums.Compare(reply)==story::Checksums::Result::Mismatch) events.Refuse();
  Check(events.Behind(),"compared guest record mismatch requires recovery");
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
  Check(host.State()==SessionState::Connected && guest.State()==SessionState::Connected,"real v11 session handshake");
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
  Check(CheckCompatibility(hc.compatibility,old)==RejectReason::Protocol,"v10 peers rejected at handshake");
}
}
int main() { Codec(); Ordering(); Identity(); LoadBaselineAndChecksums(); TransportAndMailbox(); std::puts("co-op story tests passed"); }
