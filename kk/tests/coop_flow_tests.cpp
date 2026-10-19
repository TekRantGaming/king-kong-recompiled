#include "coop_flow.h"
#include "coop_progress.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

using namespace kk::coop;
namespace {
void Check(bool ok,const char* name) { if (!ok) { std::cerr<<"FAIL: "<<name<<'\n'; std::exit(1); } }
SessionStart Start(int chapter=1) {
  SessionStart s; s.chapter=uint16_t(chapter); s.level=kChapterRules[size_t(chapter)].level;
  s.start=chapter==0 || chapter==41 ? 0 : chapter==40 ? 49 : 1;
  s.save_hash=SessionSaveHash({}); return s;
}
Message Event(MessageType type,FlowEvent world) { Message m; m.type=type; m.flow=world; return m; }
Message Life(MessageType type,FlowEvent world,uint64_t incarnation=1,uint32_t number=1) {
  Message m; m.type=type; m.life={world,incarnation,number,world.start}; return m;
}
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
  for (auto type : {MessageType::PlayerDowned,MessageType::PlayerRevived}) {
    auto life=Life(type,{1,201,1,0}); Message decoded; std::vector<uint8_t> wire;
    Check(Encode(life,wire) && wire.size()==60 && Decode(wire,decoded),"life events have fixed bounded roundtrip");
    Check(decoded.type==type && decoded.life.incarnation==1 && decoded.life.checkpoint==1,"life fields preserved");
    for (size_t n=0;n<wire.size();++n) {
      decoded.type=MessageType::Bye;
      Check(!Decode(std::span(wire).first(n),decoded) && decoded.type==MessageType::Bye,"life truncations preserve output");
    }
    auto extra=wire; extra.push_back(0); ++extra[23]; Check(!Decode(extra,decoded),"life trailing data rejects");
    life.life.incarnation=0; Check(!Encode(life,out),"life zero incarnation rejects");
    life=Life(type,{0,201,1,0}); Check(!Encode(life,out),"life missing generation rejects");
    life=Life(type,{1,201,1,0},1,0); Check(!Encode(life,out),"life zero sequence rejects");
    life=Life(type,{1,201,1,0}); life.life.checkpoint=10001; Check(!Encode(life,out),"life checkpoint bounded");
    wire[59]=0xff; wire[58]=0xff; Check(!Decode(wire,decoded),"wire checkpoint bounds enforced");
  }
}
void LifeObservations() {
  Flows f(2); f.Loaded(); const auto world=f.host.View().world;
  auto alive=Life(MessageType::PlayerRevived,world);
  f.host.Remote(alive,3);
  Check(f.host.View().partner_life.Fresh(4.5) && !f.host.View().partner_life.Fresh(4.501),"life heartbeat age is bounded");
  Check(f.host.View().partner_life.notice==0,"initial alive heartbeat is silent");
  f.host.Remote(alive,4); Check(f.host.View().partner_life.received==3,"duplicate cannot refresh life freshness");
  auto down=Life(MessageType::PlayerDowned,world,1,2); f.host.Remote(down,4);
  Check(f.host.View().partner_life.downed && f.host.View().partner_life.notice==1,"death changes notice once");
  down.life.number=3; f.host.Remote(down,4.5);
  Check(f.host.View().partner_life.notice==1 && f.host.View().world.generation==world.generation &&
      f.host.View().phase==FlowPhase::Playing,"down heartbeat never reloads living peer");
  alive.life.number=4; f.host.Remote(alive,5);
  Check(f.host.View().partner_life.downed,"same life cannot reverse death");
  alive.life.incarnation=2; alive.life.number=1; f.host.Remote(alive,5);
  Check(!f.host.View().partner_life.downed && f.host.View().partner_life.notice==2,"private reload and placement announce new life");
  down.life.number=99; f.host.Remote(down,5.1);
  Check(!f.host.View().partner_life.downed,"delayed old death cannot kill a new life");
  for (auto stale : {FlowEvent{2,201,1,0},FlowEvent{1,101,1,0},FlowEvent{1,201,2,0}}) {
    down=Life(MessageType::PlayerDowned,stale,99,99); f.host.Remote(down,6);
    Check(!f.host.View().partner_life.downed,"wrong load generation, level or start cannot affect revive");
  }
  // Either role can publish its own condition. No authority is given to load a
  // partner by sending Downed/Revived, even when both observations say down.
  f.host.Local(Life(MessageType::PlayerDowned,world,3,1),6); Transfer(f.host,f.guest,6);
  f.guest.Local(Life(MessageType::PlayerDowned,world,3,1),6); Transfer(f.guest,f.host,6);
  Check(f.host.View().partner_life.downed && f.guest.View().partner_life.downed &&
      f.guest.View().phase==FlowPhase::Playing,"bidirectional life messages leave reload policy on game thread");
  f.host.Local(Event(MessageType::ReloadRequest,world),7); Transfer(f.host,f.guest,7);
  Check(!f.host.View().partner_life.life.incarnation && !f.guest.View().partner_life.life.incarnation,
      "coordinated reload invalidates old peer life");
  Flows solo(10); solo.Loaded();
  solo.host.Remote(Life(MessageType::PlayerDowned,solo.host.View().world),3);
  Check(!solo.host.View().partner_life.life.incarnation,"solo missions retain coordinated reload fallback");
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
  for (int chapter : {0,10,13,14,15,16,17,19,20,21,27,31,32,33,36,38,39}) {
    for (bool guest_first : {false,true}) {
      Flows f(chapter); f.Loaded(); auto next=f.host.View().world;
      next.level=kChapterRules[size_t(chapter+1)].level; next.start=chapter+1==40 ? 49 : 1;
      if (guest_first) {
        f.guest.Local(Event(MessageType::ChapterFinished,f.guest.View().world),3); Transfer(f.guest,f.host,3);
        Check(f.guest.View().phase==FlowPhase::Waiting && f.host.View().phase==FlowPhase::Playing,
            "guest first waits while unfinished host keeps playing");
        f.guest.Tick(1000); Check(f.guest.View().phase==FlowPhase::Waiting,"guest finish wait has no load deadline");
      }
      const double host_finish=guest_first ? 1001 : 4;
      f.host.Local(Event(MessageType::LevelChange,next),host_finish); Transfer(f.host,f.guest,host_finish);
      if (!guest_first) {
        Check(f.host.View().phase==FlowPhase::Waiting && f.host.View().world.level==Start(chapter).level,"first finisher waits at solo boundary");
        Check(f.guest.View().phase==FlowPhase::Playing,"unfinished guest keeps playing when host finishes first");
        f.host.Tick(1000); Check(f.host.View().phase==FlowPhase::Waiting,"solo completion wait has no load deadline");
        f.guest.Local(Event(MessageType::ChapterFinished,f.guest.View().world),1001); Transfer(f.guest,f.host,1001); Transfer(f.host,f.guest,1001);
      }
      Check(f.host.View().world.level==next.level && f.guest.View().world.level==next.level,"both solo completion orders advance");
      Check(f.host.View().policy==PolicyFor(next.level) && f.guest.View().policy==PolicyFor(next.level),
          "consecutive solo chapters remain separate until next together chapter");
      Check((f.guest.View().completed & (uint64_t(1)<<chapter))!=0,"solo completion credited only after both finish");
      Check(f.host.View().phase==FlowPhase::Loading && f.guest.View().phase==FlowPhase::Loading &&
          f.host.View().world.generation==2 && f.guest.View().world.generation==2,
          "both solo finish orders enter exactly one shared load barrier");
      f.Loaded(1002);
    }
  }
  for (int chapter : {37,40}) {
    Flows observer(chapter); observer.Loaded();
    Check(observer.guest.View().policy==ChapterPolicy::Observer,"ending guest uses observer status");
    Check(std::string_view(observer.host.View().status).empty() &&
        std::string_view(observer.guest.View().status)=="Observer section. Following host progress.",
        "only the observer guest is told it is following the host");
    observer.host.Local(Event(MessageType::LevelChange,{1,7,0,0}),3); Transfer(observer.host,observer.guest,3);
    Check(observer.guest.View().world.level==7 && observer.guest.View().world.generation==2,
        "observer follows without guest chapter finish");
  }
  Flows same_level(10); same_level.Loaded(); auto restart=same_level.host.View().world; restart.start=5;
  same_level.host.Local(Event(MessageType::LevelChange,restart),3); Transfer(same_level.host,same_level.guest,3);
  Check(same_level.guest.View().world.generation==1 && same_level.host.View().world.generation==1 &&
      same_level.host.View().phase==FlowPhase::Playing && same_level.guest.View().completed==0,
      "same-level solo retry is local, not a shared load or chapter finish");
}
void DeathRouting() {
  for (size_t chapter=0;chapter<kChapterRules.size();++chapter) {
    const auto policy=kChapterRules[chapter].policy;
    Check(DeathRouteFor(policy)==(policy==ChapterPolicy::Together ? DeathRoute::TogetherRecovery : DeathRoute::LocalCheckpoint),
        "every chapter routes deaths by policy, independent of hero type");
    for (bool host_dies : {false,true}) {
      Flows f{int(chapter)}; f.Loaded();
      auto& dead=host_dies ? f.host : f.guest;
      auto checkpoint=f.host.View().world; checkpoint.start=7;
      f.host.Local(Event(MessageType::Checkpoint,checkpoint),3); Transfer(f.host,f.guest,3);
      // The frame hook never emits this for local policy. Also exercise the
      // worker guard against an accidental or delayed peer reload request.
      dead.Local(Event(MessageType::ReloadRequest,dead.View().world),4);
      if (policy!=ChapterPolicy::Together) Check(dead.Outgoing().empty(),"independent death emits no shared reload packet");
      Transfer(f.guest,f.host,4); Transfer(f.host,f.guest,4);
      if (policy==ChapterPolicy::Together) {
        Check(f.host.View().world.generation==2 && f.guest.View().world.generation==2 &&
            f.host.View().world.start==7 && f.guest.View().world.start==7,
            "either together role retains coordinated host-checkpoint fallback");
      } else {
        Check(f.host.View().world.generation==1 && f.guest.View().world.generation==1 &&
            f.host.View().phase==FlowPhase::Playing && f.guest.View().phase==FlowPhase::Playing &&
            f.host.View().completed==0 && f.guest.View().completed==0,
            "either independent death leaves both flow generations and phases unchanged");
        f.host.Remote(Event(MessageType::ReloadRequest,f.host.View().world),5);
        f.guest.Remote(Event(MessageType::ReloadRequest,f.guest.View().world),5);
        Check(f.host.Outgoing().empty() && f.guest.Outgoing().empty() &&
            f.host.View().world.generation==1 && f.guest.View().world.generation==1,
            "independent policy rejects peer reloads without forwarding or generation bump");
      }
    }
  }
  for (bool guest_first : {false,true}) {
    Flows f(10); f.Loaded();
    auto next=f.host.View().world; next.level=401;
    auto& first=guest_first ? f.guest : f.host;
    first.Local(Event(guest_first ? MessageType::ChapterFinished : MessageType::LevelChange,
        guest_first ? first.View().world : next),3);
    Transfer(f.guest,f.host,3); Transfer(f.host,f.guest,3);
    f.host.Local(Event(MessageType::ReloadRequest,f.host.View().world),4);
    f.guest.Local(Event(MessageType::ReloadRequest,f.guest.View().world),4);
    f.host.Remote(Event(MessageType::ReloadRequest,f.host.View().world),4);
    f.guest.Remote(Event(MessageType::ReloadRequest,f.guest.View().world),4);
    Check(first.View().phase==FlowPhase::Waiting && first.View().world.generation==1 &&
        f.host.Outgoing().empty() && f.guest.Outgoing().empty(),
        "local or peer deaths cannot cancel a solo finisher's wait");
    if (guest_first) f.host.Local(Event(MessageType::LevelChange,next),5);
    else f.guest.Local(Event(MessageType::ChapterFinished,f.guest.View().world),5);
    Transfer(f.guest,f.host,5); Transfer(f.host,f.guest,5);
    Check(f.host.View().world.level==401 && f.guest.View().world.level==401 &&
        f.host.View().world.generation==2 && f.guest.View().world.generation==2,
        "either finisher order still regroups after independent deaths");
    f.Loaded(6);
  }
}
void PoliciesAndBehind() {
  Check(kChapterRules.size()==42,"42 policy rows");
  constexpr uint32_t levels[] = {100,101,201,203,204,301,302,303,305,306,304,401,402,403,404,501,502,504,
      702,705,901,1002,1101,1200,1201,703,1401,1402,1501,1701,1702,1703,1801,1803,1804,1805,1901,1902,2001,2003,2004,7};
  constexpr std::string_view policies = "STTTTTTTTTSTTSSSSSTSSSTTTTTSTTTSSSTTSOSSOO";
  static_assert(policies.size()==42);
  std::array<unsigned,3> counts{};
  for (size_t i=0;i<kChapterRules.size();++i) {
    const auto expected = policies[i]=='T' ? ChapterPolicy::Together :
        policies[i]=='S' ? ChapterPolicy::SoloRegroup : ChapterPolicy::Observer;
    Check(kChapterRules[i].level==levels[i] && PolicyFor(levels[i])==expected,"every C29 entry/level/policy matches");
    ++counts[size_t(kChapterRules[i].policy)];
    Check(FlowChapter(kChapterRules[i].level)==int(i),"unique policy level and entry mapping");
    Check(kChapterRules[i].uncertain,"no unverified chapter claimed certified");
  }
  Check(counts==std::array<unsigned,3>{22,17,3},"C29 counts: 22 together, 17 solo-regroup, 3 observer");
  Check(PolicyFor(1402)==ChapterPolicy::SoloRegroup && PolicyFor(1703)==ChapterPolicy::SoloRegroup &&
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
#if defined(KK_DEV_TOOLS)
void PolicyOverrideTests() {
  const auto parsed = ParsePolicyOverrides("13:together,27:observer,37:solo,41:together");
  Check(bool(parsed),"multiple dev policy overrides parse");
  dev_policy_overrides = *parsed;
  Check(PolicyFor(403)==ChapterPolicy::Together && PolicyFor(1402)==ChapterPolicy::Observer &&
      PolicyFor(1902)==ChapterPolicy::SoloRegroup && PolicyFor(7)==ChapterPolicy::Together,
      "dev overrides address chapter entries, not level IDs");
  Check(PolicyFor(101)==ChapterPolicy::Together && PolicyFor(99999)==ChapterPolicy::Observer,
      "unspecified and unknown levels retain defaults");
  Flows f(27); f.Loaded();
  Check(f.host.View().policy==ChapterPolicy::Observer && f.guest.View().policy==ChapterPolicy::Observer,
      "both flow controllers consume dev policy at load");
  f.host.Local(Event(MessageType::LevelChange,{1,1902,1,0}),3); Transfer(f.host,f.guest,3);
  Check(f.host.View().policy==ChapterPolicy::SoloRegroup && f.guest.View().policy==ChapterPolicy::SoloRegroup,
      "dev policy also applies on chapter transition");
  for (const auto text : {"42:solo","-1:solo","101:solo",":solo","1:","1:Solo","1:solo-regroup",
      "1:solo,","1:solo,,2:together","1:solo,2:bogus","1x:solo","+1:solo"," 1:solo","1:solo:observer",
      "9999999999999999999999999:solo"})
    Check(!ParsePolicyOverrides(text),"malformed override rejected as a whole");
  Check(!ParsePolicyOverrides(std::string(1025,'0')),"override input bounded");
  const auto duplicate = ParsePolicyOverrides("1:solo,1:together");
  Check(duplicate && (*duplicate)[1]==ChapterPolicy::Together,"last duplicate entry wins");
  const auto cleared = ParsePolicyOverrides("");
  Check(bool(cleared),"empty dev override is valid"); dev_policy_overrides = *cleared;
  Check(PolicyFor(403)==ChapterPolicy::SoloRegroup && PolicyFor(1902)==ChapterPolicy::Observer,
      "empty override restores C29 defaults");
}
#endif
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
  SessionConfig hc{true,Role::Host,55,compatibility},gc{true,Role::Guest,0,compatibility};
  hc.display_name="Host"; gc.display_name="Guest"; hc.cheats={0x125};
  CoopSession host(hc,a),guest(gc,b);
  FlowMailbox h,g;
  Check(h.Begin(Start(),{}),"start queued before handshake");
  for (int i=0;i<100;++i) { const double t=i*.01; host.Tick(t);guest.Tick(t);h.Exchange(host,t);g.Exchange(guest,t); }
  Check(h.Snapshot().host && !g.Snapshot().host && g.Snapshot().start_ready,"worker role and launch handoff");
  Check(host.PartnerName()=="Guest" && guest.PartnerName()=="Host" &&
      guest.HostCheats()==hc.cheats,"display names and live cheats survive SessionStart transfer");
  Check(h.Submit(Event(MessageType::Loaded,h.Snapshot().world)) && g.Submit(Event(MessageType::Loaded,g.Snapshot().world)),"game-thread loaded handoff");
  for (int i=100;i<150;++i) { const double t=i*.01; host.Tick(t);guest.Tick(t);h.Exchange(host,t);g.Exchange(guest,t); }
  Check(h.Snapshot().phase==FlowPhase::Playing && g.Snapshot().phase==FlowPhase::Playing,"barrier across real CoopSession queues");
  InventoryState inventory{{101,1,1},1.5,1,{8,0,0,0}};
  ShotIntent shot; shot.world=inventory.world; shot.time=1.5; shot.number=1;
  shot.weapon=1; shot.direction={1,0,0};
  Check(guest.QueueShot({shot,inventory}),"guest shot with inventory queued alongside flow");
  hc.cheats={0x101};
  Check(h.Submit(Event(MessageType::Checkpoint,{h.Snapshot().world.generation,101,5,0})),
      "checkpoint queued alongside live cheat update and guest shot");
  for (int i=150;i<180;++i) {
    const double t=i*.01; host.Tick(t,{},nullptr,&hc.cheats);guest.Tick(t);h.Exchange(host,t);g.Exchange(guest,t);
  }
  const auto shots=host.TakeShots();
  Check(shots.size()==1 && shots[0].inventory && SameInventory(*shots[0].inventory,inventory) &&
      shots[0].intent.number==1,"inventory and shot IDs do not collide with flow control");
  Check(guest.HostCheats()==hc.cheats && g.Snapshot().checkpoint==5 && guest.PartnerName()=="Host",
      "checkpoint and live cheat update preserve partner identity");
  Check(g.Submit(Life(MessageType::PlayerDowned,g.Snapshot().world)),"guest down queued in common handoff");
  Check(h.Submit(Life(MessageType::PlayerDowned,h.Snapshot().world)),"host down queued in common handoff");
  for (int i=180;i<195;++i) { const double t=i*.01; host.Tick(t);guest.Tick(t);h.Exchange(host,t);g.Exchange(guest,t); }
  Check(h.Snapshot().partner_life.downed && g.Snapshot().partner_life.downed,"both roles deliver reliable life events through CoopSession");
  auto forged=Life(MessageType::PlayerRevived,h.Snapshot().world,99,1); forged.epoch=56;
  std::vector<uint8_t> wire; Check(Encode(forged,wire),"old epoch fixture"); a.incoming.push_back(wire);
  host.Tick(1.96); h.Exchange(host,1.96);
  Check(h.Snapshot().partner_life.downed,"wrong epoch life event rejected by session");
  host.End(2); h.Exchange(host,2);
  Check(h.Snapshot().phase==FlowPhase::Failed,"disconnect cannot leave game waiting forever");
}
}
int main() {
  Codecs(); LifeObservations(); SaveTransfer(); BarriersAndFollow(); SoloAndObserver(); DeathRouting(); PoliciesAndBehind(); PersonalProgress(); WorkerHandoff();
#if defined(KK_DEV_TOOLS)
  PolicyOverrideTests();
#endif
  std::cout<<"coop session flow tests passed\n";
}
