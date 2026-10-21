#include "coop_direct_udp.h"
#include "coop_desync.h"
#include "coop_session.h"
#include "coop_flow.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace kk::coop;
namespace {
void Check(bool value,const char* what) { if (!value) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); } }
Compatibility Compatible() { return {kProtocolVersion,"test","build","mods"}; }
SessionConfig Config(Role role,std::string code="TEST23") {
  SessionConfig config{true,role,role==Role::Host ? 123456789ull : 0,Compatible()};
  config.join_code=std::move(code); config.wait_for_peer=role==Role::Host;
  return config;
}
uint16_t next_port=31780;
std::unique_ptr<DirectUdpTransport> Host(std::string address="127.0.0.1") {
  for (int i=0;i<100;++i) {
    auto host=CreateDirectUdp({true,address,next_port++,"TEST23"});
    if (host->State()==TransportState::Ready) return host;
  }
  Check(false,"bind loopback host"); return {};
}
void Codec() {
  Message hello; hello.hello=Compatible(); std::vector<uint8_t> legacy,bytes;
  Check(Encode(hello,legacy),"legacy Hello");
  hello.join_code=NewJoinCode(); Check(ValidJoinCode(hello.join_code),"generated short code");
  Check(hello.join_code.size()==6 && hello.join_code.find_first_of("01OI")==std::string::npos,"six unambiguous generated characters");
  Check(NormalizeJoinCode("aBc234")=="ABC234","case independent input");
  Check(Encode(hello,bytes),"code Hello encode"); Message decoded;
  Check(Decode(bytes,decoded) && decoded.join_code==hello.join_code,"code round trip");
  Check(Decode(legacy,decoded) && decoded.join_code.empty(),"Steam Hello unchanged");
  for (size_t n=legacy.size()+1;n<bytes.size();++n) Check(!Decode(std::span(bytes).first(n),decoded),"truncated code rejected");
  bytes.push_back(0); Check(!Decode(bytes,decoded),"trailing bytes rejected");
  hello.join_code=std::string(7,'A'); Check(!Encode(hello,bytes),"code maximum");
  hello.join_code="bad code"; Check(!Encode(hello,bytes),"code controls/spaces rejected");
  Check(!ValidJoinCode("short") && !ValidJoinCode("has space") && !ValidJoinCode(std::string(7,'A')),"code UI bounds");
}
void Limits() {
  UdpRateLimit limit(2,4);
  for (int i=0;i<4;++i) Check(limit.Take(10),"rate burst");
  Check(!limit.Take(10) && !limit.Take(10.49),"flood bounded");
  Check(limit.Take(10.5) && !limit.Take(10.5),"rate refill");
  Check(!limit.Take(1) && !limit.Take(NAN) && !limit.Take(11,-1),"bad clocks/counts cannot refill");
  Check(limit.Take(100,4) && !limit.Take(100),"idle refill capped");
}
void Bounds() {
  auto host=Host();
  std::vector<uint8_t> too_large(kMaxPacketBytes+1,1);
  Check(!host->Send({},true) && !host->Send(too_large,false),"empty and oversized application packets rejected");
  Message hello; hello.hello=Compatible(); hello.join_code="TEST23";
  std::vector<uint8_t> bytes; Check(Encode(hello,bytes),"bounded packet fixture");
  for (int i=0;i<64;++i) Check(host->Send(bytes,true),"bounded reliable queue accepts window");
  Check(!host->Send(bytes,true) && host->State()==TransportState::Ready,"full reliable window backpressures without losing data or disconnecting");
  host->Close(); Check(host->State()==TransportState::Closed && !host->Send(bytes,false),"closed transport inert");
  Check(CreateDirectUdp({true,"127.0.0.1",0,"TEST23"})->State()==TransportState::Failed,"zero port rejected");
  Check(CreateDirectUdp({true,"127.0.0.1",31779,"short"})->State()==TransportState::Failed,"short code rejected before binding");
}
void Integration(std::string address) {
  auto a=Host(address); const auto port=a->LocalPort();
  auto b=CreateDirectUdp({false,address,port,"test23"});
  Check(b->State()==TransportState::Ready,"join loopback family");
  auto collision=CreateDirectUdp({true,address,port,"TEST23"});
  Check(collision->State()==TransportState::Failed,"exclusive host port");
  CoopSession host(Config(Role::Host),*a),guest(Config(Role::Guest,"test23"),*b);
  double now=1;
  // The host remains available after the ordinary 20-second connection deadline.
  for (;now<26;now+=.01) host.Tick(now);
  Check(host.State()==SessionState::Connecting,"host waits indefinitely without a peer");
  auto pump=[&](int count) {
    for (int i=0;i<count;++i,now+=.01) { host.Tick(now); guest.Tick(now); }
  };
  pump(200);
  Check(host.State()==SessionState::Connected && guest.State()==SessionState::Connected,"same protocol session handshake");
  Check(!a->PeerReady() && !b->PeerReady(),"readiness explicit");
  Check(a->SetReady(true) && b->SetReady(true),"ready enqueue"); pump(100);
  Check(a->PeerReady() && b->PeerReady(),"both ready reliable");
  Check(a->SetChoice({1,10,false}) && !a->PeerReady(),"chapter change immediately invalidates host view of guest readiness");
  pump(100);
  Check(b->Choice()==LobbyChoice{1,10,false} && !b->PeerReady(),"guest sees exact host chapter and readiness reset");
  Check(!b->SetChoice({2,1,false}),"guest cannot choose chapter");
  Check(!b->SetReady(true,99),"stale Ready revision rejected");
  Check(a->SetReady(true,1) && b->SetReady(true,1),"new choice acknowledged by both"); pump(100);
  Check(a->PeerReady() && b->PeerReady(),"both ready for new choice");
  Check(a->SetChoice({2,1,true}),"Continue is explicit choice"); pump(100);
  Check(b->Choice()==LobbyChoice{2,1,true} && !a->PeerReady() && !b->PeerReady(),"Continue resets both players and reaches guest");
  Check(!b->SetReady(true,1) && a->SetReady(true,2) && b->SetReady(true,2),"old chapter readiness cannot ready Continue"); pump(100);
  Check(b->SetReady(false),"unready enqueue"); pump(100); Check(!a->PeerReady(),"unready delivered");
  b->SetReady(true); pump(100);
  // All session message families share this transport, including a full private save and load barrier.
  SessionStart start; start.chapter=1; start.level=101; start.start=1;
  std::vector<uint8_t> save(kSessionSaveBytes,0x42); start.save_bytes=uint32_t(save.size()); start.save_hash=SessionSaveHash(save);
  FlowMailbox host_flow,guest_flow; Check(host_flow.Begin(start,save),"begin session flow");
  for (int i=0;i<600;++i,now+=.01) { host.Tick(now); guest.Tick(now); host_flow.Exchange(host,now); guest_flow.Exchange(guest,now); }
  const auto hv=host_flow.Snapshot(),gv=guest_flow.Snapshot();
  Check(hv.start_ready && gv.start_ready && gv.save==save,"full save and SessionStart via UDP");
  Message loaded; loaded.type=MessageType::Loaded; loaded.flow=hv.world;
  Check(host_flow.Submit(loaded) && guest_flow.Submit(loaded),"both loaded");
  for (int i=0;i<200;++i,now+=.01) { host.Tick(now); guest.Tick(now); host_flow.Exchange(host,now); guest_flow.Exchange(guest,now); }
  Check(host_flow.Snapshot().phase==FlowPhase::Playing && guest_flow.Snapshot().phase==FlowPhase::Playing,"shared load release");
  for (int i=0;i<200;++i,now+=.01) {
    PlayerPose pose{now,101,1,2,3,0,0}; host.Tick(now,pose); pose.x=4; guest.Tick(now,pose);
  }
  Check(host.Poses().received>0 && guest.Poses().received>0 && host.Partner(now) && guest.Partner(now),"bidirectional poses");
  // A third correct-code peer cannot claim or disturb an established host.
  auto c=CreateDirectUdp({false,address,port,"TEST23"}); CoopSession third(Config(Role::Guest),*c);
  for (int i=0;i<100;++i,now+=.01) { third.Tick(now); host.Tick(now); guest.Tick(now); }
  Check(third.State()==SessionState::Connecting && host.State()==SessionState::Connected,"one peer only");
  guest.End(now);
  for (int i=0;i<100;++i,now+=.01) { b->Poll(now); host.Tick(now); }
  Check(host.State()==SessionState::Ended,"Bye ends peer");
}
void WRONG2() {
  auto a=Host(); auto b=CreateDirectUdp({false,"127.0.0.1",a->LocalPort(),"WRONG2"});
  CoopSession host(Config(Role::Host),*a),bad(Config(Role::Guest,"WRONG2"),*b);
  double now=1;
  for (int i=0;i<200;++i,now+=.01) { host.Tick(now); bad.Tick(now); }
  Check(b->CodeRejected() && bad.State()==SessionState::Ended,"wrong code rejected explicitly");
  Check(host.State()==SessionState::Connecting && a->Stats().received==0,"wrong code cannot pin host");
  auto c=CreateDirectUdp({false,"127.0.0.1",a->LocalPort(),"TEST23"}); CoopSession good(Config(Role::Guest),*c);
  for (int i=0;i<200;++i,now+=.01) { host.Tick(now); good.Tick(now); }
  Check(host.State()==SessionState::Connected && good.State()==SessionState::Connected,"valid guest after wrong code");
}
// Local proxy deliberately loses, reorders and duplicates the real wire. It never binds publicly.
class LossyLoopback {
 public:
  explicit LossyLoopback(uint16_t host_port) {
    socket_=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    local_.sin_family=AF_INET; local_.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    Check(bind(socket_,reinterpret_cast<sockaddr*>(&local_),sizeof(local_))==0,"proxy loopback bind");
    host_=local_; host_.sin_port=htons(host_port);
#ifdef _WIN32
    int length=sizeof(local_); u_long mode=1; ioctlsocket(socket_,FIONBIO,&mode);
#else
    socklen_t length=sizeof(local_); fcntl(socket_,F_SETFL,O_NONBLOCK);
#endif
    getsockname(socket_,reinterpret_cast<sockaddr*>(&local_),&length);
  }
  ~LossyLoopback() {
#ifdef _WIN32
    closesocket(socket_);
#else
    close(socket_);
#endif
  }
  uint16_t Port() const { return ntohs(local_.sin_port); }
  void Flood() {
    const std::array<uint8_t,3> bad{1,2,3};
    for (int i=0;i<128;++i) Send(bad,host_);
  }
  void Poll() {
    if (!held_.empty()) { Send(held_,held_target_); held_.clear(); }
    for (int i=0;i<128;++i) {
      std::array<uint8_t,1100> bytes{}; sockaddr_in from{};
#ifdef _WIN32
      int length=sizeof(from);
#else
      socklen_t length=sizeof(from);
#endif
      const int n=int(recvfrom(socket_,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&from),&length));
      if (n<0) break;
      const bool from_host=from.sin_port==host_.sin_port;
      if (!from_host) guest_=from;
      const auto target=from_host ? guest_ : host_;
      if (!target.sin_port || n<28) continue;
      const uint8_t kind=bytes[10];
      if (kind==1 && !(from_host ? dropped_host_ : dropped_guest_)) {
        (from_host ? dropped_host_ : dropped_guest_)=true; continue;
      }
      if (kind==2 && !dropped_ack_) { dropped_ack_=true; continue; }
      const std::span<const uint8_t> wire(bytes.data(),size_t(n));
      if (kind==1 && ++reliable_%4==0) { held_.assign(wire.begin(),wire.end()); held_target_=target; }
      else Send(wire,target);
      if (kind==1 && reliable_%7==0) Send(wire,target);
    }
  }
 private:
  void Send(std::span<const uint8_t> wire,const sockaddr_in& target) {
    sendto(socket_,reinterpret_cast<const char*>(wire.data()),int(wire.size()),0,reinterpret_cast<const sockaddr*>(&target),sizeof(target));
  }
#ifdef _WIN32
  SOCKET socket_=INVALID_SOCKET;
#else
  int socket_=-1;
#endif
  sockaddr_in local_{},host_{},guest_{},held_target_{};
  bool dropped_host_=false,dropped_guest_=false,dropped_ack_=false;
  int reliable_=0;
  std::vector<uint8_t> held_;
};
void ReliabilityAndFlood() {
  auto a=Host(); LossyLoopback proxy(a->LocalPort());
  proxy.Flood();
  for (int i=0;i<4;++i) a->Poll(1);
  Check(a->Stats().malformed>0 && a->Stats().malformed<=40,"unauthenticated flood bounded by burst");
  auto b=CreateDirectUdp({false,"127.0.0.1",proxy.Port(),"TEST23"});
  CoopSession host(Config(Role::Host),*a),guest(Config(Role::Guest),*b);
  double now=5;
  for (int i=0;i<600;++i,now+=.01) { proxy.Poll(); host.Tick(now); guest.Tick(now); }
  Check(host.State()==SessionState::Connected && guest.State()==SessionState::Connected,"handshake survives lost Hello and ACK");
  Check(a->Stats().resent>0 && b->Stats().resent>0,"actual resend path exercised");
  Message m; m.type=MessageType::SaveChunk; m.epoch=host.Epoch();
  // Consumer receives reliable data once in sequence despite duplicates and reorder.
  for (uint32_t i=0;i<40;++i) {
    m.sequence=i+100; m.save_chunk={i,{uint8_t(i)}}; std::vector<uint8_t> bytes;
    Check(Encode(m,bytes) && a->Send(bytes,true),"queue reliability samples");
  }
  uint32_t count=0;
  for (int i=0;i<600;++i,now+=.01) {
    proxy.Poll(); a->Poll(now); b->Poll(now); std::vector<uint8_t> bytes;
    while (b->Receive(bytes)) {
      Message received;
      if (!Decode(bytes,received) || received.type!=MessageType::SaveChunk) continue;
      Check(received.save_chunk.offset==count && received.save_chunk.bytes[0]==count,"ordered once-only data"); ++count;
    }
  }
  Check(count==40,"all reliable samples delivered");
}
}
void SimulationIntegration() {
  DirectUdpOptions h{true,"127.0.0.1",next_port++,"TEST23"}; h.simulation={150,30,3};
  auto a=CreateDirectUdp(h);
  DirectUdpOptions j{false,"127.0.0.1",a->LocalPort(),"TEST23"}; j.simulation={150,30,3};
  auto b=CreateDirectUdp(j);
  CoopSession host(Config(Role::Host),*a),guest(Config(Role::Guest),*b);
  desync::Assembly assembly; desync::Guard guard;
  uint32_t sweep=0; unsigned digests=0; double next=16;
  bool burst_queued=false,burst_received=false;
  std::deque<Message> burst;
  enemies::Frame frame; frame.world={101,1,1};
  EnemyState enemy{{777,0,EnemyFamily::Raptor},1,2,3,0,100};
  frame.records.push_back({enemy,EnemyEventKind::Spawned});
  for (double now=1;now<121;now+=.01) {
    frame.time=now;
    host.Tick(now,PlayerPose{now,101,1,2,3,0,0},&frame);
    guest.Tick(now,PlayerPose{now,101,4,2,3,0,0});
    if (now>10) Check(host.State()==SessionState::Connected && guest.State()==SessionState::Connected,"150/30/3 never Lost");
    if (now>=10 && !burst_queued && host.State()==SessionState::Connected) {
      burst_queued=true; ++sweep;
      for (uint16_t part=0;part<256;++part) {
        Message m; m.type=MessageType::WorldDigest; m.epoch=host.Epoch();
        auto& p=m.digest; p.generation=1; p.level=101; p.start=1; p.checkpoint=1; p.sweep=sweep;
        p.captured=now;
        p.enemy_world={101,1,1};
        p.part=part; p.parts=256; p.script_count=kStorySnapshotScripts;
        for (size_t i=0;i<kDigestScriptsPerPage;++i) p.scripts.push_back({1+part*kDigestScriptsPerPage+i,99,1,0,1});
        burst.push_back(std::move(m));
      }
    }
    while (!burst.empty() && host.QueueStory(burst.front())) burst.pop_front();
    if (now>=next && host.State()==SessionState::Connected) {
      next=now+3; Message m; m.type=MessageType::WorldDigest; m.epoch=host.Epoch();
      m.digest.generation=1; m.digest.level=101; m.digest.start=1; m.digest.checkpoint=1;
      m.digest.sweep=++sweep; m.digest.enemy_count=1;
      m.digest.captured=now;
      m.digest.enemy_world={101,1,1};
      m.digest.enemies.push_back({enemy.id,EnemyOwner::Host,0,100,0});
      Check(host.QueueStory(m) && !guest.QueueStory(m),"digest direction enforced on real UDP");
    }
    for (const auto& m:guest.TakeStory()) if (auto complete=assembly.Push(m.digest,now)) {
      Check(desync::FreshForPose(complete->header,guest.Partner(now)),"150/30/3 complete sweeps remain fresh against host poses");
      if (complete->scripts.size()==kStorySnapshotScripts) burst_received=true;
      ++digests; const auto remote=guest.Enemies(now); desync::Evidence evidence;
      evidence.structural=remote.records.size()!=1;
      evidence.quiet=!remote.records.empty() && remote.records[0].enemy.life!=100;
      Check(guard.Observe(1,101,1,evidence,now)==desync::Tier::None,"150/30/3 no false recovery and enemies agree");
    }
  }
  Check(burst_received && digests>=30 && a->Stats().resent>0 && b->Stats().resent>0,"maximum census, latency, loss, ACK and digest paths exercised");
}
int main() {
  Codec(); Limits(); Bounds(); WRONG2(); Integration("127.0.0.1"); Integration("::1"); ReliabilityAndFlood();
  SimulationIntegration();
  std::puts("Direct UDP codec, code, rate, IPv4/IPv6, readiness, save/load, poses and single-peer checks passed");
}
