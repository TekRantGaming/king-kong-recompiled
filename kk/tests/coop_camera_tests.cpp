// Source-only handoff to Claude. No game, SDK or network dependencies.
#include "coop_camera.h"
#include "coop_session.h"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace kk::coop;
namespace {
void Check(bool ok,const char* why) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
bool Near(double a,double b) { return std::abs(a-b)<.0002; }
CameraPose Pose(double time=1) {
  CameraPose p; p.time=time; p.generation=1; p.level=102; p.cut=1; return p;
}
std::vector<uint8_t> Packet(Message m) { std::vector<uint8_t> b; Check(Encode(m,b),"encode fixture"); return b; }
void TogetherCinematics() {
  TogetherCutscene follow;
  auto p=Pose();
  follow.Update(p,1,102,1);
  Check(!follow.Active() && !follow.Skip(1),"Together gameplay keeps its own camera and input");
  p.scripted=true; p.cut++;
  follow.Update(p,1,102,2);
  Check(follow.Active(),"host in-engine cinematic enables temporary camera follow without a native guest trigger");
  follow.Update(p,1,102,3);
  Check(follow.Active() && !follow.Skip(3),"camera cuts alone never synthesize a skip");
  ++p.skip; p.scripted=false; ++p.cut;
  follow.Update(p,1,102,4);
  Check(!follow.Active() && follow.Skip(4.1),"host skip reaches native input even when the same packet ends the cinematic");
  Check(!follow.Skip(4.26),"skip injection expires rather than skipping a later unrelated scene");
  p.skip=9; p.generation=2; p.scripted=true;
  follow.Update(p,2,102,5);
  Check(follow.Active() && !follow.Skip(5),"new generation rebases historical skip counters");
  follow.Update(p,2,203,5.1);
  Check(!follow.Active() && !follow.Skip(5.1),"different level camera never claims cinematic control");
  follow.Update(p,2,102,6);
  follow.Update({},2,102,7);
  Check(!follow.Active() && !follow.Skip(7),"expired stream, disconnect or leaving Together releases its independent lease");
}
void Codec() {
  for (auto type : {MessageType::CameraPose,MessageType::CameraCut}) {
    Message m; m.type=type; m.epoch=0x123456789abcdef0ull; m.sequence=0xabcdef01;
    m.camera=Pose(132.75); m.camera.x=-27.5f; m.camera.y=3.125f; m.camera.z=999;
    m.camera.qx=.5f; m.camera.qy=-.5f; m.camera.qz=.5f; m.camera.qw=.5f;
    m.camera.cut=19; m.camera.skip=7; m.camera.scripted=true; m.camera.paused=true; m.camera.fov=2.2f;
    auto bytes=Packet(m); Message decoded;
    Check(bytes.size()==kHeaderBytes+57 && bytes[5]==kProtocolVersion && bytes[7]==uint16_t(type),"bounded v18 camera payload");
    Check(Decode(bytes,decoded) && Packet(decoded)==bytes,"exact camera fields round trip");
    for (size_t size=0;size<bytes.size();++size) {
      Message out; out.sequence=19;
      Check(!Decode(std::span(bytes.data(),size),out) && out.sequence==19,"all camera truncations preserve destination");
    }
    auto trailing=bytes; trailing.push_back(0); Check(!Decode(trailing,decoded),"reject trailing camera bytes");
    auto bad=bytes; bad.back()=4; Check(!Decode(bad,decoded),"unknown camera state bit rejected");
    for (size_t offset : {size_t(24),size_t(28),size_t(32),size_t(36),size_t(40),size_t(44),size_t(48),size_t(52)}) {
      // All eight floats in the payload, after 16 bytes scope and 8 bytes clock.
      bad=bytes; bad[kHeaderBytes+offset]=0x7f; bad[kHeaderBytes+offset+1]=0xc0;
      bad[kHeaderBytes+offset+2]=bad[kHeaderBytes+offset+3]=0;
      Check(!Decode(bad,decoded),"reject NaN in every camera float");
    }
    bad=bytes; bad[kHeaderBytes+16]=0x7f; bad[kHeaderBytes+17]=0xf8;
    Check(!Decode(bad,decoded),"reject NaN camera clock");
  }
  auto p=Pose(); std::vector<uint8_t> b;
  auto invalid=[&](CameraPose q) { Message m; m.type=MessageType::CameraPose; m.camera=q; return !Encode(m,b); };
  p.generation=0; Check(invalid(p),"zero generation rejected");
  p=Pose(); p.level=0; Check(invalid(p),"zero level rejected");
  p=Pose(); p.cut=0; Check(invalid(p),"zero cut rejected");
  p=Pose(); p.qw=0; Check(invalid(p),"zero quaternion rejected");
  p=Pose(); p.qw=2; Check(invalid(p),"non-unit quaternion rejected");
  p=Pose(); p.x=1000000; Check(invalid(p),"unbounded position rejected");
  p=Pose(); p.fov=3; Check(invalid(p),"unbounded FOV rejected");
  p=Pose(); p.time=-1; Check(invalid(p),"negative camera time rejected");
}
void RotationAndBuffer() {
  for (float angle : {0.f,.5f,1.7f,3.14159265f,4.7f}) for (unsigned axis=0;axis<3;++axis) {
    auto p=Pose(); p.qw=std::cos(angle/2);
    if (axis==0) p.qx=std::sin(angle/2); else if (axis==1) p.qy=std::sin(angle/2); else p.qz=std::sin(angle/2);
    auto copy=Pose(); Check(CameraRotation(copy,CameraBasis(p)),"matrix to quaternion at all diagonal branches");
    Check(Near(std::abs(CameraDot(p,copy)),1),"roll/pitch/yaw matrix round trip");
  }
  auto p=Pose(); auto axes=CameraBasis(p); axes[0][0]=-1;
  Check(!CameraRotation(p,axes),"reflected basis rejected");
  axes=CameraBasis(Pose()); axes[0][1]=.5f; Check(!CameraRotation(p,axes),"skewed basis rejected");
  auto a=Pose(100),b=Pose(100.1); b.x=10; b.fov=2;
  a.qz=std::sin(179.f*3.14159265f/360); a.qw=std::cos(179.f*3.14159265f/360);
  b.qz=std::sin(-179.f*3.14159265f/360); b.qw=std::cos(-179.f*3.14159265f/360);
  const auto mid=BlendCamera(a,b,.5f);
  Check(Near(mid.x,5) && Near(mid.fov,1.6) && std::abs(mid.qz)>.9999,"shortest quaternion arc and linear position/lens");
  Check(ValidCameraPose(mid),"interpolated quaternion stays normalized");
  auto opposite=a; opposite.qx=-a.qx; opposite.qy=-a.qy; opposite.qz=-a.qz; opposite.qw=-a.qw;
  Check(Near(std::abs(CameraDot(BlendCamera(a,opposite,.5f),a)),1),"q and -q interpolate without collapse");
  CameraBuffer buffer;
  Check(!buffer.Sample(1),"empty buffer");
  Check(buffer.Push(a,2) && buffer.Push(b,2.1),"remote and local clocks independent");
  auto sample=buffer.Sample(2.125);
  Check(sample && Near(sample->x,5),"75 ms delayed interpolation");
  Check(!buffer.Push(a,2.13),"out of order camera rejected");
  Check(!buffer.Push(b,2.13),"duplicate camera cannot refresh lease");
  Check(buffer.Sample(2.3) && Near(buffer.Sample(2.3)->x,10),"loss holds latest, no extrapolation");
  Check(buffer.Sample(8.4) && Near(buffer.Sample(8.4)->x,10),"6.3 second frame hitch freezes last camera");
  Check(buffer.Sample(10.1) && !buffer.Sample(10.101),"camera expires only after eight seconds");
  Check(!buffer.Push(b,10.2) && !buffer.Sample(10.2),"duplicate cannot revive expired stream");
  Check(!buffer.Sample(1.9),"backwards local clock rejected");
  auto next=b; next.time+=.1; next.cut++; next.x=12;
  Check(buffer.Push(next,2.2) && Near(buffer.Sample(2.2)->x,12),"cut snaps without interpolating through scene");
  next.time+=.1; next.generation++; next.cut=1; next.x=20;
  Check(buffer.Push(next,2.3) && buffer.Sample(2.3)->generation==2,"same-level reload clears prior visit");
  b.time=999; Check(!buffer.Push(b,2.4),"late previous generation cannot replace camera");
  next.time+=.1; next.level=103; Check(!buffer.Push(next,2.4),"level cannot change inside same generation");
  next.level=102; next.x=21;
  Check(buffer.Push(next,12) && Near(buffer.Sample(12)->x,21),"resumed stream snaps across long receipt gap");
  buffer.Clear(); Check(!buffer.Sample(2.4),"disconnect clears interpolation history");
}
struct Transport final : ICoopTransport {
  Transport* peer=nullptr;
  std::deque<std::vector<uint8_t>> incoming;
  unsigned poses=0,cuts=0;
  bool Send(std::span<const uint8_t> bytes,bool reliable) override {
    Message m; Check(Decode(bytes,m),"session sends valid codec");
    if (m.type==MessageType::CameraPose) { Check(!reliable,"camera poses unreliable"); ++poses; }
    if (m.type==MessageType::CameraCut) { Check(reliable,"camera cuts/skips reliable"); ++cuts; }
    if (peer) peer->incoming.emplace_back(bytes.begin(),bytes.end()); return true;
  }
  bool Receive(std::vector<uint8_t>& out) override { if (incoming.empty()) return false; out=std::move(incoming.front()); incoming.pop_front(); return true; }
  void Poll(double) override {}
  TransportState State() const override { return TransportState::Ready; }
  TransportStats Stats() const override { return {}; }
  void Close() override {}
};
SessionConfig Config(Role role) { return {true,role,role==Role::Host ? 1234ull : 0,{kProtocolVersion,"test","test","test"}}; }
void Sessions() {
  Transport ht,gt; ht.peer=&gt; gt.peer=&ht;
  CoopSession host(Config(Role::Host),ht),guest(Config(Role::Guest),gt);
  for (unsigned i=0;i<100;++i) { host.Tick(i*.005); guest.Tick(i*.005); }
  Check(host.State()==SessionState::Connected && guest.State()==SessionState::Connected,"handshake for camera fixture");
  CameraMailbox send,receive;
  for (unsigned i=100;i<300;++i) {
    const double now=i*.005;
    auto p=Pose(now); p.x=float(now); send.Publish(p); send.Exchange(host,now);
    host.Tick(now); guest.Tick(now); receive.Exchange(guest,now);
  }
  auto sample=receive.Sample(1.495);
  Check(sample && sample->generation==1 && sample->level==102,"UDP/Steam shared camera mailbox delivery");
  Check(ht.poses>=28 && ht.poses<=31 && !gt.poses && !gt.cuts,"approximately 30 Hz host only");
  Check(ht.cuts==1,"steady camera sends no redundant reliable cuts");
  auto p=Pose(1.5); p.scripted=true; p.cut=2; p.skip=1; send.Publish(p); send.Exchange(host,1.5);
  host.Tick(1.5); guest.Tick(1.5); receive.Exchange(guest,1.5);
  sample=receive.Sample(1.5);
  Check(ht.cuts==2 && sample && sample->cut==2 && sample->skip==1 && sample->scripted,"reliable local cinematic start and host skip state");
  // A guest cannot publish a camera for the host, and a different epoch cannot supply one to the guest.
  guest.PublishCamera(Pose(1.51)); guest.Tick(1.51); host.Tick(1.51);
  Check(!host.HostCamera(1.51) && !gt.poses,"guest camera has no authority");
  Message forged; forged.type=MessageType::CameraPose; forged.epoch=1235; forged.sequence=9000;
  forged.camera=Pose(1.52); forged.camera.x=999;
  gt.incoming.push_back(Packet(forged)); guest.Tick(1.52);
  Check(guest.HostCamera(1.52) && guest.HostCamera(1.52)->x!=999,"wrong epoch rejected");
  forged.epoch=1234; forged.sequence=0;
  gt.incoming.push_back(Packet(forged)); guest.Tick(1.53);
  Check(guest.HostCamera(1.53)->x!=999,"reordered camera sequence rejected");
  auto info=guest.CameraInfo();
  Check(info.rejected_count>=2 && info.sequence==0 && info.accepted_sequence!=0 &&
      std::string(info.rejection)=="old/duplicate sequence","sequence rejection diagnostics copied");
  forged.sequence=9001; forged.camera=Pose(1.4);
  gt.incoming.push_back(Packet(forged)); guest.Tick(1.54);
  Check(std::string(guest.CameraInfo().rejection)=="duplicate/backwards source clock" &&
      Near(guest.CameraInfo().accepted,info.accepted),"source clock rejection does not renew receipt lease");
  host.PublishCamera({}); host.Tick(2.1); guest.Tick(2.1); receive.Exchange(guest,2.1);
  Check(receive.Sample(2.1).has_value(),"brief publication gap freezes last accepted camera");
  Check(receive.Sample(7.8) && !receive.Sample(9.501),"stalled mailbox worker cannot extend packet lease");
  for (unsigned i=43;i<=200;++i) {
    const double now=i*.05; host.Tick(now); guest.Tick(now); receive.Exchange(guest,now);
  }
  Check(host.State()==SessionState::Connected && guest.State()==SessionState::Connected &&
      !receive.Sample(10),"non-camera traffic keeps connection but cannot renew camera lease");
  p=Pose(10.01); p.cut=2; p.scripted=true; p.skip=1; p.x=20;
  host.PublishCamera(p); host.Tick(10.01); guest.Tick(10.01); receive.Exchange(guest,10.01);
  sample=receive.Sample(10.01);
  Check(sample && Near(sample->x,20),"fresh host pose automatically recovers after prolonged camera loss");
  Check(receive.Info().accepted_count>info.accepted_count && Near(receive.Info().accepted,10.01),
      "mailbox carries actual accepted receipt time and counters");
  TogetherCutscene together;
  together.Update(sample,1,102,10.01);
  Check(together.Active(),"Together consumes the existing authoritative camera mailbox");
  p.time=10.02; ++p.cut; ++p.skip; p.scripted=false;
  host.PublishCamera(p); host.Tick(10.02); guest.Tick(10.02); receive.Exchange(guest,10.02);
  together.Update(receive.Sample(10.02),1,102,10.02);
  Check(!together.Active() && together.Skip(10.02),"reliable CameraCut end/skip restores local view and retains the host skip edge");
  guest.End(10.03); receive.Exchange(guest,10.03); Check(!receive.Sample(10.03),"session end clears camera mailbox");
  auto disabled=Config(Role::Host); disabled.enabled=false; Transport dt; CoopSession off(disabled,dt);
  off.PublishCamera(Pose(1)); off.Tick(1); Check(!dt.poses && !dt.cuts,"co-op off emits no camera");
}
}
int main() { Codec(); RotationAndBuffer(); TogetherCinematics(); Sessions(); std::puts("observer camera tests passed"); }
