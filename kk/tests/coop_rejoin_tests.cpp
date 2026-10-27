#include "coop_direct_udp.h"
#include "coop_flow.h"
#include "coop_handover.h"
#include "steam_transport.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace kk::coop;
namespace {
int checks=0;
void Check(bool ok,const char* why) { ++checks; if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
SessionConfig Config(bool host,uint64_t process) {
  SessionConfig c{true,host ? Role::Host : Role::Guest,host ? 9600u : 0u,{kProtocolVersion,"v","b","mods"}};
  c.resume_token=9600; c.join_code="ABC234"; c.process_id=process; return c;
}
Message Loaded(const SessionFlow& f) { Message m; m.type=MessageType::Loaded; m.flow=f.View().world; return m; }
void Transfer(SessionFlow& a,SessionFlow& b,double now) {
  while (!a.Outgoing().empty()) {
    Message decoded; std::vector<uint8_t> bytes;
    Check(Encode(a.Outgoing().front(),bytes) && Decode(bytes,decoded),"flow wire roundtrip");
    a.Sent(); b.Remote(decoded,now);
  }
}
void FlowRejoin(bool fresh_guest) {
  SessionFlow h(true),g;
  SessionStart s; s.chapter=7; s.level=303; s.start=1;
  Check(h.Begin(s,{},0),"begin campaign"); Transfer(h,g,0);
  h.Local(Loaded(h),1); g.Local(Loaded(g),1); Transfer(g,h,1); Transfer(h,g,1);
  Check(h.View().phase==FlowPhase::Playing && g.View().phase==FlowPhase::Playing,"initial release");
  Message cp; cp.type=MessageType::Checkpoint; cp.flow=h.View().world; cp.flow.start=4;
  h.Local(cp,2); Transfer(h,g,2);
  s.weapon=2; s.stock={5,6,7,8,15,16,17,18}; s.visit=3;
  h.PublishRejoinState(s);
  h.Connection(SessionState::Lost,0,3); g.Connection(SessionState::Lost,0,3);
  Check(h.View().phase==FlowPhase::Playing && g.View().reconnecting,"host plays and guest waits");
  h.Connection(SessionState::Connected,1,63); g.Connection(SessionState::Connected,1,63);
  if (fresh_guest) g=SessionFlow(false);
  Transfer(h,g,63);
  Check(g.View().phase==FlowPhase::Loading && g.View().checkpoint==4 && g.View().world.start==1,"guest loads current checkpoint retaining logical identity");
  Check(g.View().start.weapon==2 && g.View().start.stock==s.stock && g.View().start.visit==3,"equipment and enemy visit transferred");
  Check(h.View().phase==FlowPhase::Playing,"host never enters load barrier");
  g.Local(Loaded(g),64); Transfer(g,h,64); Transfer(h,g,64);
  Check(g.View().phase==FlowPhase::Playing && h.View().phase==FlowPhase::Playing,"only guest reload releases");
  g.Connection(SessionState::Lost,1,65); g.Connection(SessionState::Connected,2,66);
  Transfer(g,h,66); Transfer(h,g,66);
  Check(h.View().phase==FlowPhase::Playing && g.View().phase==FlowPhase::Loading,"guest only interruption explicitly requests a host snapshot");
  g.Local(Loaded(g),67); Transfer(g,h,67); Transfer(h,g,67);
  Check(g.View().phase==FlowPhase::Playing,"asymmetric interruption releases after the guest load");
  h.Connection(SessionState::Lost,1,70); g.Connection(SessionState::Lost,1,70);
  h.Connection(SessionState::Ended,1,190); g.Connection(SessionState::Ended,1,190);
  Check(h.View().phase==FlowPhase::Playing && h.View().reconnect_expired,"host continues after bounded timeout");
  Check(g.View().phase==FlowPhase::Failed && g.View().reconnect_expired,"guest gets clean failure");
}
struct Backend final : ISteamMessagesBackend {
  Backend* other=nullptr; uint64_t self=0; bool cut=false;
  uint64_t packet_epoch=0;
  std::deque<SteamBackendMessage> incoming[2];
  SteamSendResult Send(uint64_t,int channel,std::span<const uint8_t> bytes,bool reliable) override {
    if (cut) return SteamSendResult::Failed;
    Message m; if (Decode(bytes,m) && m.type!=MessageType::Hello && m.type!=MessageType::Reject) packet_epoch=m.epoch;
    other->incoming[channel].push_back({self,channel,reliable,false,{bytes.begin(),bytes.end()}});
    return SteamSendResult::Sent;
  }
  bool Receive(int channel,SteamBackendMessage& out) override {
    if (incoming[channel].empty()) return false;
    out=std::move(incoming[channel].front()); incoming[channel].pop_front(); return true;
  }
  bool Accept(uint64_t) override { return true; }
  void Close(uint64_t) override {}
  TransportStats Stats(uint64_t) const override { return {}; }
};
void SteamOutage(double duration,bool one_way) {
  Backend a,b; a.self=1; b.self=2; a.other=&b; b.other=&a;
  SteamMessagesTransport at(a,2,true),bt(b,1,true);
  CoopSession h(Config(true,11),at),g(Config(false,22),bt);
  FlowMailbox hb,gb; double next_life=0; uint32_t life_number=0;
  auto pump=[&](double from,double until) { for (double t=from;t<until;t+=.02) {
    if (hb.Snapshot().phase==FlowPhase::Playing && t>=next_life) {
      Message life; life.type=MessageType::PlayerRevived;
      life.life={hb.Snapshot().world,1,++life_number,1}; hb.Submit(life); next_life=t+.5;
    }
    h.Tick(t); hb.Exchange(h,t); g.Tick(t); gb.Exchange(g,t);
  } };
  pump(0,1); Check(h.State()==SessionState::Connected && g.State()==SessionState::Connected,"Steam initial handshake");
  SessionStart start; start.chapter=7; start.level=303; start.start=1; hb.Begin(start,{});
  pump(1,2);
  Message loaded; loaded.type=MessageType::Loaded; loaded.flow=hb.Snapshot().world;
  hb.Submit(loaded); gb.Submit(loaded); pump(2,3);
  Check(hb.Snapshot().phase==FlowPhase::Playing && gb.Snapshot().phase==FlowPhase::Playing,"runtime flow starts before the outage");
  const auto old_epoch=a.packet_epoch;
  a.cut=true; b.cut=!one_way; at.SessionFailed(2);
  pump(3,3+duration);
  Check(h.State()==SessionState::Lost && g.State()==SessionState::Lost,"both players detect full or one way outage without ending the campaign");
  Check(hb.Snapshot().phase==FlowPhase::Playing,"host life heartbeats cannot overflow control flow during an outage");
  a.cut=b.cut=false; pump(3+duration,6+duration);
  Check(h.State()==SessionState::Connected && g.State()==SessionState::Connected,"Steam outage recovers");
  auto gc=Config(false,33); CoopSession restarted(gc,bt);
  // The replacement process has its own monotonic clock starting at zero.
  for (double t=6+duration;t<10+duration;t+=.02) { h.Tick(t); restarted.Tick(t-6-duration); }
  Check(h.State()==SessionState::Connected && restarted.State()==SessionState::Connected && h.ReconnectCount()>0,"Steam guest process restart renegotiates");
  Check(a.packet_epoch!=old_epoch && h.Epoch()==9600 && restarted.Epoch()==9600,"packet incarnation changes while campaign identity stays stable");
  Message delayed; delayed.type=MessageType::PlayerPose; delayed.epoch=old_epoch; delayed.sequence=100000;
  delayed.pose.time=1000000; delayed.pose.level=303; delayed.pose.x=17;
  std::vector<uint8_t> bytes; Check(Encode(delayed,bytes),"encode delayed old process pose");
  b.incoming[1].push_back({1,1,false,false,bytes}); restarted.Tick(4.1);
  Check(!restarted.Partner(4.1),"old process packet cannot cross the rejoin boundary");
  delayed.epoch=a.packet_epoch; ++delayed.sequence;
  Check(Encode(delayed,bytes),"encode new incarnation pose");
  b.incoming[1].push_back({1,1,false,false,bytes}); restarted.Tick(4.2);
  Check(restarted.Partner(4.2).has_value(),"new process packet is accepted");
}
void UdpRestart(bool host_crash) {
  static uint16_t next_port=31960;
  DirectUdpOptions ho{true,"127.0.0.1",next_port++,"ABC234"}; ho.resume_token=9600;
  auto ht=CreateDirectUdp(ho); const auto port=ht->LocalPort();
  DirectUdpOptions go{false,"127.0.0.1",port,"ABC234"}; go.resume_token=9600;
  auto gt=CreateDirectUdp(go);
  auto h=std::make_unique<CoopSession>(Config(true,101),*ht),g=std::make_unique<CoopSession>(Config(false,202),*gt);
  auto pump=[&](double from,double end) {
    for (double t=from;t<end;t+=.01) { h->Tick(t); g->Tick(t); }
  };
  pump(0,2); Check(h->State()==SessionState::Connected && g->State()==SessionState::Connected,"UDP initial handshake");
  if (host_crash) {
    h.reset(); ht->Close();
    for (double t=2;t<6;t+=.01) g->Tick(t);
    ho.port=port; ht=CreateDirectUdp(ho); h=std::make_unique<CoopSession>(Config(true,303),*ht);
  } else {
    g.reset(); gt->Close();
    for (double t=2;t<6;t+=.01) h->Tick(t);
    gt=CreateDirectUdp(go); g=std::make_unique<CoopSession>(Config(false,404),*gt);
  }
  pump(6,12);
  if (h->State()!=SessionState::Connected || g->State()!=SessionState::Connected)
    std::fprintf(stderr,"UDP restart host-crash=%d host=%s guest=%s\n",host_crash,StateName(h->State()),StateName(g->State()));
  Check(h->State()==SessionState::Connected && g->State()==SessionState::Connected,"UDP process restart replaces nonce and endpoint");
}
void Record() {
  handover::File f; f.host=true; f.plan={9600,StartTransport::Direct,7,0,0,0,27196};
  f.address="127.0.0.1"; f.code="ABC234"; f.version="v"; f.written_ms=1000;
  f.checkpoint.rejoin=true; f.checkpoint.chapter=7; f.checkpoint.level=303; f.checkpoint.start=4;
  f.checkpoint.world_start=1; f.checkpoint.generation=9; f.checkpoint.visit=2; f.checkpoint.weapon=2;
  handover::File parsed;
  Check(handover::Parse(handover::Format(f),61000,"v",parsed)==handover::ParseResult::Ok && parsed.checkpoint.start==4 && parsed.checkpoint.generation==9,"durable record survives restart");
  Check(handover::Parse(handover::Format(f),122000,"v",parsed)==handover::ParseResult::Expired,"durable record expires");
}
}
int main() {
  Record(); FlowRejoin(false); FlowRejoin(true);
  for (auto seconds : {10.,60.}) for (bool one_way : {false,true}) SteamOutage(seconds,one_way);
  UdpRestart(false); UdpRestart(true);
  std::printf("coop rejoin: %d checks passed\n",checks);
}
