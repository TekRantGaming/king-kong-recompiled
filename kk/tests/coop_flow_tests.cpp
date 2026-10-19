#include "coop_flow.h"
#include "coop_progress.h"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace kk::coop;
namespace {
void Check(bool ok,const char* name) { if (!ok) { std::cerr<<"FAIL: "<<name<<'\n'; std::exit(1); } }
SessionStart Start(int chapter=1) {
  SessionStart s; s.chapter=uint16_t(chapter); s.level=kChapterRules[size_t(chapter)].level;
  s.start=chapter==0 || chapter==41 ? 0 : chapter==40 ? 49 : 1;
  s.save_hash=SessionSaveHash({}); return s;
}
Message Event(MessageType type,FlowEvent world) { Message m; m.type=type; m.flow=world; return m; }
void Transfer(SessionFlow& from,SessionFlow& to,double now=1) {
  while (!from.Outgoing().empty()) {
    auto m=from.Outgoing().front(); from.Sent();
    std::vector<uint8_t> bytes; Message decoded;
    Check(Encode(m,bytes) && Decode(bytes,decoded),"controller output has valid codec"); to.Remote(decoded,now);
  }
}
struct Flows {
  SessionFlow host{true},guest{false};
  explicit Flows(int chapter=1) {
    Check(host.Begin(Start(chapter),{},0),"host begins"); Transfer(host,guest);
    Check(guest.View().start_ready,"empty-save session ready");
  }
  void Loaded(double now=2) {
    host.Local(Event(MessageType::Loaded,host.View().world),now); Transfer(host,guest,now);
    Check(host.View().phase==FlowPhase::Loading && guest.View().phase==FlowPhase::Loading,"one loaded is insufficient");
    guest.Local(Event(MessageType::Loaded,guest.View().world),now+.1); Transfer(guest,host,now+.1); Transfer(host,guest,now+.1);
    Check(host.View().phase==FlowPhase::Playing && guest.View().phase==FlowPhase::Playing,"host releases both loaded");
  }
};
void Codecs() {
  for (auto type : {MessageType::SessionStart,MessageType::SaveChunk,MessageType::LevelChange,
      MessageType::Loaded,MessageType::LoadRelease,MessageType::Checkpoint,MessageType::ChapterFinished,
      MessageType::ReloadRequest,MessageType::FlowAbort}) {
    Message m; m.type=type; m.start=Start(); m.start.cheats=0x1ff; m.start.difficulty=1;
    m.save_chunk={0,std::vector<uint8_t>(kSaveChunkBytes,0xa5)}; m.flow={9,201,5,0x1234};
    std::vector<uint8_t> wire; Message decoded;
    Check(Encode(m,wire) && Decode(wire,decoded),"all flow messages round trip");
    std::vector<uint8_t> again; Check(Encode(decoded,again) && wire==again,"canonical fields and byte order");
    Check(wire.size()<=kMaxPacketBytes,"bounded control packet");
    for (size_t i=0;i<wire.size();++i) {
      Message sentinel; sentinel.sequence=0x12345678;
      Check(!Decode(std::span(wire).first(i),sentinel) && sentinel.sequence==0x12345678,"all truncations reject without modifying output");
    }
    wire.push_back(0); Check(!Decode(wire,decoded),"trailing data rejects");
  }
  Message bad; bad.type=MessageType::SessionStart; bad.start=Start(); std::vector<uint8_t> out;
  bad.start.chapter=42; Check(!Encode(bad,out),"bad chapter"); bad.start=Start();
  bad.start.cheats=0x200; Check(!Encode(bad,out),"bad cheat bits"); bad.start=Start();
  bad.start.difficulty=2; Check(!Encode(bad,out),"bad difficulty"); bad.start=Start();
  bad.start.save_bytes=kSessionSaveBytes+1; Check(!Encode(bad,out),"save too large");
  bad=Event(MessageType::LevelChange,{0,201,1,0}); Check(!Encode(bad,out),"zero load generation");
  bad.flow={1,201,10001,0}; Check(!Encode(bad,out),"invalid checkpoint");
  bad.flow={1,201,1,uint64_t(1)<<42}; Check(!Encode(bad,out),"completion bit overflow");
  bad.type=MessageType::SaveChunk; bad.save_chunk={kSessionSaveBytes-1,{1,2}};
  Check(!Encode(bad,out),"chunk end overflow"); bad.save_chunk={0,{}}; Check(!Encode(bad,out),"empty chunk");
  bad.type=MessageType::SessionStart; bad.start=Start(); Check(Encode(bad,out),"valid metadata encode");
  out[kHeaderBytes+10]=2; Check(!Decode(out,bad),"noncanonical bool rejects");
}
void SaveTransfer() {
  SessionFlow host(true),guest(false); auto s=Start(2);
  std::vector<uint8_t> save(kSessionSaveBytes); for (size_t i=0;i<save.size();++i) save[i]=uint8_t(i*17);
  s.save_bytes=uint32_t(save.size()); s.save_hash=SessionSaveHash(save);
  Check(host.Begin(s,save,0),"full bounded host snapshot");
  auto metadata=host.Outgoing().front(); host.Sent(); guest.Remote(metadata,1);
  Check(!guest.View().start_ready && guest.View().phase==FlowPhase::Preparing,"no launch before complete save");
  auto chunk=host.Outgoing().front(); chunk.save_chunk.offset=4; guest.Remote(chunk,1);
  Check(guest.View().save.empty(),"out of order chunk ignored");
  Transfer(host,guest); Check(guest.View().save==save && guest.View().start_ready,"exact full snapshot reconstructed");
  guest.Remote(metadata,2); Check(guest.View().save==save,"duplicate start cannot reset active world");
  SessionFlow corrupt(false); corrupt.Remote(metadata,0);
  for (uint32_t i=0;i<save.size();i+=kSaveChunkBytes) {
    Message c; c.type=MessageType::SaveChunk; c.save_chunk.offset=i;
    c.save_chunk.bytes.assign(save.begin()+i,save.begin()+std::min(save.size(),size_t(i+kSaveChunkBytes)));
    if (!i) c.save_chunk.bytes[0]^=1; corrupt.Remote(c,1);
  }
  Check(corrupt.View().phase==FlowPhase::Failed && !corrupt.View().start_ready,"corrupt snapshot cannot launch");
}
void BarriersAndFollow() {
  Flows f; auto world=f.host.View().world;
  f.guest.Remote(Event(MessageType::LoadRelease,world),1);
  Check(f.guest.View().phase==FlowPhase::Loading,"release before local load cannot run");
  f.host.Remote(Event(MessageType::Loaded,{2,101,1,0}),1);
  f.Loaded();
  f.host.Remote(Event(MessageType::LevelChange,{2,201,1,0}),3);
  Check(f.host.View().world.generation==1,"guest cannot order host level");
  f.guest.Remote(Event(MessageType::Checkpoint,{1,101,4,0}),3);
  Check(f.guest.View().checkpoint==4,"host checkpoint recorded");
  f.host.Remote(Event(MessageType::Checkpoint,{1,101,8,0}),3);
  Check(f.host.View().checkpoint==1,"guest cannot author checkpoint");
  f.host.Local(Event(MessageType::LevelChange,{1,201,1,0}),4); Transfer(f.host,f.guest,4);
  Check(f.host.View().world.generation==2 && f.guest.View().world.level==201,"guest follows authoritative next world");
  Check((f.guest.View().completed & 2)!=0,"completed chapter recorded separately");
  f.guest.Remote(Event(MessageType::LevelChange,{2,203,1,2}),4);
  Check(f.guest.View().world.level==201,"duplicate generation rejected");
  f.guest.Local(Event(MessageType::Loaded,world),5); Transfer(f.guest,f.host,5);
  Check(f.host.View().phase==FlowPhase::Loading,"stale loaded cannot release later world");
  f.Loaded(5);
  auto checkpoint=f.host.View().world; checkpoint.start=7;
  f.host.Local(Event(MessageType::Checkpoint,checkpoint),6); Transfer(f.host,f.guest,6);
  f.guest.Local(Event(MessageType::ReloadRequest,f.guest.View().world),7); Transfer(f.guest,f.host,7); Transfer(f.host,f.guest,7);
  Check(f.guest.View().world.generation==3 && f.guest.View().world.start==7,"recovery loads host checkpoint on both peers");
  f.host.Tick(126.9); Check(f.host.View().phase==FlowPhase::Loading,"before load deadline");
  f.host.Tick(127); Check(f.host.View().phase==FlowPhase::Failed,"load barrier timeout"); Transfer(f.host,f.guest,127);
  Check(f.guest.View().phase==FlowPhase::Failed,"timeout propagated to peer");
}
void SoloAndObserver() {
  for (bool guest_first : {false,true}) {
    Flows f(10); f.Loaded(); auto next=f.host.View().world; next.level=401;
    if (guest_first) { f.guest.Local(Event(MessageType::ChapterFinished,f.guest.View().world),3); Transfer(f.guest,f.host,3); }
    f.host.Local(Event(MessageType::LevelChange,next),4); Transfer(f.host,f.guest,4);
    if (!guest_first) {
      Check(f.host.View().phase==FlowPhase::Waiting && f.host.View().world.level==304,"first finisher waits at solo boundary");
      f.host.Tick(1000); Check(f.host.View().phase==FlowPhase::Waiting,"solo completion wait has no load deadline");
      f.guest.Local(Event(MessageType::ChapterFinished,f.guest.View().world),1001); Transfer(f.guest,f.host,1001); Transfer(f.host,f.guest,1001);
    }
    Check(f.host.View().world.level==401 && f.guest.View().world.level==401,"both solo completion orders regroup");
    Check((f.guest.View().completed & (uint64_t(1)<<10))!=0,"solo completion credited only after both finish");
  }
  Flows observer(40); observer.Loaded();
  Check(observer.guest.View().policy==ChapterPolicy::Observer,"ending guest uses observer status");
  observer.host.Local(Event(MessageType::LevelChange,{1,7,0,0}),3); Transfer(observer.host,observer.guest,3);
  Check(observer.guest.View().world.level==7,"observer follows without guest chapter finish");
  Flows same_level(10); same_level.Loaded(); auto restart=same_level.host.View().world; restart.start=5;
  same_level.host.Local(Event(MessageType::LevelChange,restart),3); Transfer(same_level.host,same_level.guest,3);
  Check(same_level.guest.View().world.generation==2 && same_level.guest.View().completed==0,
      "same-level solo transition is a load, not a chapter finish wait");
}
void PoliciesAndBehind() {
  Check(kChapterRules.size()==42,"42 policy rows");
  for (size_t i=0;i<kChapterRules.size();++i) {
    Check(FlowChapter(kChapterRules[i].level)==int(i),"unique policy level and entry mapping");
    Check(kChapterRules[i].uncertain,"no unverified chapter claimed certified");
  }
  Check(PolicyFor(1402)==ChapterPolicy::Together && PolicyFor(1703)==ChapterPolicy::SoloRegroup &&
      PolicyFor(2004)==ChapterPolicy::Observer && PolicyFor(99999)==ChapterPolicy::Observer,"provisional and unknown fallback policies");
  BehindRule behind; PlayerPose local,host; local.level=host.level=101; host.x=101;
  local.time=0; Check(!behind.Tick(0,local,host),"behind interval starts");
  local.time=7.9; Check(!behind.Tick(7.9,local,host),"behind interval below eight seconds");
  local.time=8; Check(behind.Tick(8,local,host),"continuous distance causes catchup");
  Check(!behind.Tick(8.1,local,host),"catchup cooldown restarts evidence");
  local.time=9; host.x=0; Check(!behind.Tick(9,local,host),"near partner clears evidence");
  host.level=201; local.time=10; Check(!behind.Tick(10,local,host),"different-level evidence begins");
  Check(!behind.Tick(18,std::nullopt,host),"missing local evidence resets timer");
  local.time=19; Check(!behind.Tick(19,local,host),"reset requires fresh interval");
  local.time=27; Check(behind.Tick(27,local,host),"different level catches up");
  Check(!behind.Tick(std::numeric_limits<double>::infinity(),local,host),"invalid clock ignored");
}
void PersonalProgress() {
  std::vector<uint8_t> save(kSessionSaveBytes,0xa5);
  save[0x28]=save[0x29]=0; save[0x2a]=1; save[0x2b]=1;
  save[0x34]=save[0x35]=save[0x36]=save[0x37]=0;
  const auto before=save;
  Check(GrantTogetherProgress(save,uint64_t(1)<<10),"finished chapter unlocks following entry");
  Check(save[0x37]==11,"unlock word stores next reached entry");
  for (size_t i=0;i<save.size();++i) if (i<0x34 || i>=0x38) Check(save[i]==before[i],"all personal world and option bytes preserved");
  auto unlocked=save; Check(GrantTogetherProgress(save,1) && save==unlocked,"no progress regression");
  Check(!GrantTogetherProgress(save,uint64_t(1)<<42) && save==unlocked,"invalid completion leaves personal data unchanged");
  save.resize(16); const auto short_save=save;
  Check(!GrantTogetherProgress(save,1) && save==short_save,"short personal save never changed");
}
struct Link : ICoopTransport {
  Link* peer=nullptr; std::deque<std::vector<uint8_t>> incoming;
  bool Send(std::span<const uint8_t> b,bool reliable) override {
    Message m; Check(Decode(b,m),"valid transport packet");
    if (IsFlowMessage(m.type)) Check(reliable,"every flow packet is reliable");
    peer->incoming.emplace_back(b.begin(),b.end()); return true;
  }
  bool Receive(std::vector<uint8_t>& out) override { if (incoming.empty()) return false; out=std::move(incoming.front());incoming.pop_front();return true; }
  void Poll(double) override {} TransportState State() const override {return TransportState::Ready;}
  TransportStats Stats() const override {return {};} void Close() override {}
};
void WorkerHandoff() {
  Link a,b; a.peer=&b; b.peer=&a;
  Compatibility compatibility{kProtocolVersion,"test","test","test"};
  CoopSession host({true,Role::Host,55,compatibility},a),guest({true,Role::Guest,0,compatibility},b);
  FlowMailbox h,g;
  Check(h.Begin(Start(),{}),"start queued before handshake");
  for (int i=0;i<100;++i) { const double t=i*.01; host.Tick(t);guest.Tick(t);h.Exchange(host,t);g.Exchange(guest,t); }
  Check(h.Snapshot().host && !g.Snapshot().host && g.Snapshot().start_ready,"worker role and launch handoff");
  Check(h.Submit(Event(MessageType::Loaded,h.Snapshot().world)) && g.Submit(Event(MessageType::Loaded,g.Snapshot().world)),"game-thread loaded handoff");
  for (int i=100;i<150;++i) { const double t=i*.01; host.Tick(t);guest.Tick(t);h.Exchange(host,t);g.Exchange(guest,t); }
  Check(h.Snapshot().phase==FlowPhase::Playing && g.Snapshot().phase==FlowPhase::Playing,"barrier across real CoopSession queues");
  host.End(2); h.Exchange(host,2);
  Check(h.Snapshot().phase==FlowPhase::Failed,"disconnect cannot leave game waiting forever");
}
}
int main() {
  Codecs(); SaveTransfer(); BarriersAndFollow(); SoloAndObserver(); PoliciesAndBehind(); PersonalProgress(); WorkerHandoff();
  std::cout<<"coop session flow tests passed\n";
}
