#include "coop_objects.h"
#include "coop_session.h"
#include "coop_stimulus_debug.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace kk::coop;
namespace {
void Check(bool value,const char* text) { if (!value) { std::fprintf(stderr,"FAIL: %s\n",text); std::exit(1); } }
Message Packet(MessageType type=MessageType::ObjectState,ObjectKind kind=ObjectKind::Breakable) {
  Message m; m.type=type; m.epoch=17; m.object={2,203,1,0x123456789abcdef0ull,kind,type==MessageType::ObjectRequest && kind==ObjectKind::Breakable ? 0u : 1u}; return m;
}
void Codec() {
  for (auto type : {MessageType::ObjectState,MessageType::ObjectRequest}) for (auto kind : {ObjectKind::Breakable,ObjectKind::Door,ObjectKind::Fire,ObjectKind::Raft}) {
    if (type==MessageType::ObjectRequest && kind==ObjectKind::Raft) continue;
    auto m=Packet(type,kind);
    if (type==MessageType::ObjectRequest && kind==ObjectKind::Breakable) { m.object.value=0x800010; m.object.position=42; m.object.seconds=.125f; }
    if (type==MessageType::ObjectState && kind==ObjectKind::Door) { m.object.position=.375f; m.object.seconds=2.5f; m.object.value=511; }
    if (type==MessageType::ObjectState && kind==ObjectKind::Fire) { m.object.position=37.5f; m.object.seconds=-1; }
    if (kind==ObjectKind::Raft) { m.object.raft.route=123; m.object.raft.pose[3]=m.object.raft.pose[8]=1; }
    std::vector<uint8_t> bytes; Check(Encode(m,bytes),"object encodes");
    Check(bytes.size()==kHeaderBytes+(kind==ObjectKind::Raft ? 91 : 33) && bytes[5]==kProtocolVersion,"bounded protocol 19 object wire");
    Message out; Check(Decode(bytes,out) && out.object==m.object,"object round trip");
    for (size_t i=0;i<bytes.size();++i) {
      Message sentinel; sentinel.sequence=123;
      Check(!Decode(std::span(bytes).first(i),sentinel) && sentinel.sequence==123,"truncation is atomic");
    }
    auto bad=bytes; bad.push_back(0); ++bad[23]; Check(!Decode(bad,out),"trailing data refused");
    bad=bytes; bad[kHeaderBytes+20]=0; Check(!Decode(bad,out),"unknown kind refused");
    bad=bytes; bad[kHeaderBytes+25]=0x7f; bad[kHeaderBytes+26]=0x80; bad[kHeaderBytes+27]=bad[kHeaderBytes+28]=0;
    Check(!Decode(bad,out),"nonfinite float refused");
  }
  auto m=Packet(MessageType::ObjectRequest); std::vector<uint8_t> bytes;
  m.object.value=0x01000000; Check(!Encode(m,bytes),"unsupported native hit flag range refused");
  m=Packet(MessageType::ObjectRequest,ObjectKind::Door); m.object.value=8;
  Check(!Encode(m,bytes),"lever slot bounded"); m.object.value=0; m.object.position=.1f;
  Check(!Encode(m,bytes),"request cannot choose door progress");
  auto fire=Packet(MessageType::ObjectState,ObjectKind::Fire).object;
  fire.seconds=-1; Check(ValidObjectState(fire),"indefinite native burn lifetime admitted");
  fire.seconds=-.5f; Check(!ValidObjectState(fire),"negative fractional fire lifetime refused");
  fire.value=2; fire.seconds=0; fire.position=40;
  Check(ValidObjectState(fire),"retained extinguished fire preserves spread for late replay");
  fire.seconds=1; Check(!ValidObjectState(fire),"extinguished fire cannot keep burning");
  fire=Packet(MessageType::ObjectRequest,ObjectKind::Fire).object;
  Check(ValidObjectRequest(fire),"guest ignition has a bounded semantic request");
  fire.position=1; Check(!ValidObjectRequest(fire),"guest cannot choose fire extent");
  fire.position=0; fire.seconds=1; Check(!ValidObjectRequest(fire),"guest cannot choose fire lifetime");
  fire.seconds=0; fire.value=2; Check(!ValidObjectRequest(fire),"guest cannot extinguish an authoritative fire");
  auto raft=Packet(MessageType::ObjectState,ObjectKind::Raft).object;
  raft.raft.route=1; raft.raft.pose[3]=raft.raft.pose[8]=1;
  Check(ValidObjectState(raft) && !ValidObjectRequest(raft),"only host raft poses and typed path ordinals admitted");
  raft.raft.current=128; Check(!ValidObjectState(raft),"raft path ordinals bounded"); raft.raft.current=0;
  raft.raft.pose[8]=0; Check(!ValidObjectState(raft),"degenerate raft orientation refused"); raft.raft.pose[8]=1;
  raft.raft.pose[6]=1; Check(!ValidObjectState(raft),"nonorthogonal raft orientation refused"); raft.raft.pose[6]=0;
  raft.raft.pose[9]=1001; Check(!ValidObjectState(raft),"raft velocities bounded");
}
void Policies() {
  Check(objects::StaticFireParameters(0,0,0,0,60),"dormant fire layout admits deferred local path setup");
  Check(!objects::StaticFireParameters(1,0,0,1,60) && !objects::StaticFireParameters(0,1,0,1,60) &&
      !objects::StaticFireParameters(0,0,0x82500000,1,60),"write, moving and arbitrary completion fire paths excluded");
  Check(!objects::StaticFireParameters(0,0,0,140,60) && !objects::StaticFireParameters(0,0,0,1,-2),
      "native fire effect table and lifetime bounds enforced");
  std::map<uint32_t,uint32_t> path{{0x42000018,0x43000000},{0x43000018,0x44000000},
      {0x44000000,0x45000000},{0x44000004,1},{0x45000000,123},{0x45000004,0x46000000},
      {0x45000008,1},{0x46000008,0x47000000}};
  const auto path_read=[&](uint32_t a){ const auto it=path.find(a); return it==path.end() ? 0u : it->second; };
  auto target=objects::FirstPathTarget(path_read,0x42000000);
  Check(target && target->key==123 && target->actor==0x47000000,"static fire binds its local authored first target");
  Check(!objects::FirstPathTarget(path_read,0x42000000,456),"absent path key refuses native group[count] access");
  path[0x44000004]=2; path[0x4500000c]=123;
  Check(!objects::FirstPathTarget(path_read,0x42000000),"duplicate target groups refuse path replay");
  path[0x44000004]=1; path[0x45000008]=0;
  target=objects::FirstPathTarget(path_read,0x42000000);
  Check(target && !target->actor,"empty later group represents the native path end");
  path[0x45000008]=objects::kCensus+1;
  Check(!objects::FirstPathTarget(path_read,0x42000000),"malformed local target counts refuse before native lookup");
  path[0x45000008]=2; path[0x46000018]=0x47001000;
  target=objects::RaftPathTarget(path_read,path_read,0x42000000,123);
  Check(target && target->actor==0x47000000,"raft uses native first target without flags");
  path[0x46000010]=1;
  target=objects::RaftPathTarget(path_read,path_read,0x42000000,123);
  Check(target && target->actor==0x47001000,"raft follows the unique native flagged target");
  path[0x46000000]=1;
  Check(!objects::RaftPathTarget(path_read,path_read,0x42000000,123),"random flagged raft branches remain unsupported");
  path[0x45000008]=61;
  Check(!objects::RaftPathTarget(path_read,path_read,0x42000000,123),"raft path protects the native sixty entry scratch bound");
  std::map<uint32_t,uint32_t> scheduler{{0x42000050,1},{0x42000052,3},
      {0x4200001c,0x43000000},{0x42000020,0x43000100},{0x42000024,0x43000200},
      {0x43000014,0x826718B8},{0x43000114,0x82543478},{0x43000214,0x82574D80}};
  const auto schedule=[&](uint32_t a){ auto it=scheduler.find(a); return it==scheduler.end() ? 0u : it->second; };
  Check(objects::DoorSlots(schedule,schedule,0x42000000)==std::vector<unsigned>({2,3}),
      "isolated endpoint retires only the bound door's own known tracks");
  Check(!objects::BreakableConsumer(schedule,schedule,0x42000000),"door/human callbacks cannot be woken as breakables");
  scheduler[0x43000114]=0x82516128;
  const auto consumer=objects::BreakableConsumer(schedule,schedule,0x42000000);
  Check(consumer && consumer->init && consumer->slot==2 && consumer->block==0x43000100,
      "private replay selects the obstacle's own initializer and state word");
  scheduler[0x43000214]=0x82515CF0;
  Check(!objects::BreakableConsumer(schedule,schedule,0x42000000),"ambiguous native consumers are refused");
  scheduler[0x43000114]=0x82543478; scheduler[0x43000214]=0x82574D80;
  std::map<uint32_t,uint32_t> checkpoint{{0x44000234,201},{0x82CC92F8,0x45000000},
      {0x42001004,0x00600000},{0x45000c20,1u<<9}};
  const auto checkpoint_read=[&](uint32_t a){ const auto it=checkpoint.find(a); return it==checkpoint.end() ? 0u : it->second; };
  Check(objects::BreakableCheckpointGone(checkpoint_read,0x42001000,0x44000000),
      "native checkpoint terminal cleanup admits semantic damage restoration");
  checkpoint[0x42001004]=0x00400000;
  Check(!objects::BreakableCheckpointGone(checkpoint_read,0x42001000,0x44000000),"one removal flag alone cannot certify native terminal cleanup");
  checkpoint[0x42001004]=0x00600000; checkpoint[0x45000c20]=0;
  Check(!objects::BreakableCheckpointGone(checkpoint_read,0x42001000,0x44000000),"terminal replay requires this object's exact native checkpoint bit");
  scheduler[0x42000052]=5;
  Check(objects::DoorSlots(schedule,schedule,0x42000000).empty(),"native stop cannot address a sixth slot");
  scheduler[0x42000052]=0;
  Check(objects::DoorSlots(schedule,schedule,0x42000000).empty(),"reversed native scheduler range refuses retirement");
  auto raw=Packet(MessageType::ObjectRequest).object;
  raw.value=0x800010; raw.position=6;
  Check(ValidObjectRequest(raw) && !ValidObjectState(raw),"raw hit is distinct from absolute damage");
  raw.position=6.5f; Check(!ValidObjectRequest(raw),"raw magnitude must be an integer");
  raw.position=100001; Check(!ValidObjectRequest(raw),"raw magnitude bounded");
  auto weak=Packet().object; weak.position=5;
  Check(ValidObjectState(weak),"snapshot retains native weak-hit count");
  weak.position=6; Check(!ValidObjectState(weak),"native sixth weak hit must reset before snapshot");
  auto hold=Packet(MessageType::ObjectRequest,ObjectKind::Door).object; hold.seconds=1;
  Check(ValidObjectRequest(hold),"installed lever hold admitted");
  hold.seconds=2; Check(ValidObjectRequest(hold),"installed lever release admitted");
  hold.seconds=3; Check(!ValidObjectRequest(hold),"unknown lever operation refused");
  objects::LeverHold timer;
  Check(objects::NearLever(1,0,1,1,0),"use fallback accepts a nearby facing installed marker");
  Check(!objects::NearLever(1,0,1,-1,0) && !objects::NearLever(1,0,2,1,0) &&
      !objects::NearLever(3,0,0,1,0),"use fallback refuses behind, other floor and distant markers");
  timer.Touch(true,1);
  Check(timer.Active(1) && timer.Active(1.75) && !timer.Active(1.751) &&
      !timer.Active(.9) && !timer.Active(std::numeric_limits<double>::quiet_NaN()),
      "isolated door endpoint requires a fresh monotonic hold");
  for (int i=0;i<4;++i) Check(!timer.Step(1,.05f,.25f),"hold uses native simulation time");
  Check(timer.Step(1,.05f,.25f) && !timer.Step(1,.05f,.25f),"one endpoint per continuous hold");
  timer.Touch(false,1.1); timer.Touch(true,2);
  Check(!timer.Step(2,.05f,.1f),"release resets partial progress");
  Check(!timer.Step(3,.05f,.1f),"lost heartbeat cannot open a door");
  timer.Touch(true,4);
  Check(!timer.Step(4,1,.1f) && !timer.Step(4,.05f,-1),"bad delta or duration cannot force endpoint");
  Check(timer.Step(4,.05f,0) && !timer.Step(4,.05f,0),"native zero-duration opening completes once after a valid hold");
  std::map<uint32_t,uint32_t> pool{{stimulus_debug::kPools+64,0x45000000},
      {stimulus_debug::kPools+72,1},{stimulus_debug::kPools+76,4},
      {0x45000000,0},{0x45000008,0x42000000},{0x4500000c,0x42001000},
      {0x45000070,9},{0x46000000,0x04000000},{0x46000004,9}};
  const auto read_pool=[&](uint32_t a){ const auto it=pool.find(a); return it==pool.end() ? 0u : it->second; };
  auto record=stimulus_debug::ReadHandle(read_pool,0x46000000);
  Check(record.valid && record.source==0x42000000 && record.target==0x42001000,"exact record retains separate shooter and target");
  pool[0x45000060]=64; pool[0x45000064]=6;
  const auto strike=[&](uint64_t previous=0){ return objects::StrikeRecord(read_pool,0x46000000,previous,0x42000000,0x42001000,64,6); };
  Check(strike(),"new successful native raw hit accepted");
  pool[0x45000000]=1;
  Check(strike(),"new pending native record is accepted before the next pool activation pass");
  pool[0x45000000]=0;
  pool[0x45000004]=std::bit_cast<uint32_t>(.125f);
  Check(!strike() && objects::StrikeRecord(read_pool,0x46000000,0,0x42000000,0x42001000,64,6,.125f),"raw lifetime must match the exact native record");
  pool[0x45000004]=0;
  Check(!strike(uint64_t(0x04000000)<<32|9),"pool-full unchanged result cannot replay the previous shot");
  pool[0x45000008]=0x42002000;
  Check(!strike(),"environmental hit cannot borrow a previous local shooter latch");
  pool[0x45000008]=0x42000000; pool[0x45000064]=1000;
  Check(!strike(),"mixed hit magnitudes cannot share provenance");
  pool[0x45000064]=6;
  pool[0x46000004]=8;
  Check(!stimulus_debug::ReadHandle(read_pool,0x46000000).valid,"reused pool slot cannot authorize a stale hit");
  pool[0x46000004]=9; pool[0x45000000]=4;
  Check(!stimulus_debug::ReadHandle(read_pool,0x46000000).valid,"freed record cannot authorize a hit");
  std::map<uint32_t,uint32_t> memory{{0x82C4EDC0,0x43000000},{0x82C4EDC4,2},
      {0x43000000,7},{0x43000004,0x42000000},{0x43000008,8},{0x4300000c,0x42001000},
      {0x82C4ED94,0x44000000},{0x82C4ED98,3},{0x44000000,0xabcd1234},{0x44000004,7},
      {0x44000008,0xabcd5678},{0x4400000c,8},{0x44000010,0xabcd5679},{0x44000014,8}};
  auto read=[&](uint32_t address){ const auto it=memory.find(address); return it==memory.end() ? 0u : it->second; };
  auto keys=objects::FileKeys(read);
  Check(keys.size()==1 && keys[0x42000000]==0xabcd1234,"native file key join refuses address aliases");
  const auto asset=objects::AssetSource(ObjectKind::Door,keys[0x42000000]);
  Check(asset && asset!=objects::AssetSource(ObjectKind::Breakable,keys[0x42000000]),"asset identity is family scoped and independent of heap address or live pose");
  memory[0x82C4EDC4]=100001;
  Check(objects::FileKeys(read).empty(),"malformed native file table is bounded before reading entries");
  const auto id=objects::Source(ObjectKind::Breakable,-0.f,2,3,1);
  Check(id && id==objects::Source(ObjectKind::Breakable,0.f,2,3,1),"identity canonicalizes signed zero");
  Check(id!=objects::Source(ObjectKind::Door,0,2,3,1),"families do not alias");
  Check(!objects::Source(ObjectKind::Door,std::numeric_limits<float>::quiet_NaN(),2,3,1),"nonfinite authored position refused");
  std::map<uint32_t,objects::Binding> bindings{{1,{1,2,3,4,id,ObjectKind::Breakable}}};
  Check(objects::Match(id,ObjectKind::Breakable,bindings)!=nullptr,"exact native binding found");
  bindings[2]={5,6,7,8,id,ObjectKind::Breakable};
  Check(!objects::Match(id,ObjectKind::Breakable,bindings),"duplicate authored ID refuses both actors");
  bindings[2].source=id+1; bindings[2].vars=3;
  Check(!objects::Match(id,ObjectKind::Breakable,bindings),"aliased mutable component refuses drive");
  bindings.erase(2);
  Check(!objects::Match(id,ObjectKind::Door,bindings),"same numeric source cannot select another family");
  objects::Identities identities; const auto original=bindings[1];
  Check(identities.Record(original),"first authored object claims local identity");
  auto reused=original; reused.actor=5; reused.ai=6; reused.vars=7; reused.matrix=8;
  Check(!identities.Record(reused) && !identities.Allows(original.kind,id),"same authored ID cannot migrate to another allocation");
  Check(!identities.Record(original),"retiring a duplicate cannot make its survivor unambiguous again");
  identities.Clear();
  Check(identities.Record(reused),"private native reload rebinds addresses in a fresh local identity ledger");
  objects::Replica replica; auto s=Packet().object; s.value=100;
  Check(replica.Observe(s) && !replica.Observe(s),"absolute snapshot duplicate idempotent");
  auto old=s; old.number=2; old.generation=1;
  Check(!replica.Observe(old) && replica.Find(s.kind,s.source)->value==100,"old world cannot replace active object");
  Check(objects::DamageDelta(0,100)==100 && objects::DamageDelta(100,100)==0 && objects::DamageDelta(101,100)==0,"no repeated damage or rewind");
  Check(objects::DamageDelta(0,replica.Find(s.kind,s.source)->value)==100,"private native reload can reapply absolute outcome");
  objects::Requests requests; auto r=Packet(MessageType::ObjectRequest).object;
  Check(requests.Admit(r,1,3) && !requests.Admit(r,1.1,3),"request replay rejected");
  ++r.number; Check(!requests.Admit(r,1.1,81),"distant strike refused");
  r.kind=ObjectKind::Door; r.value=0; ++r.number; Check(!requests.Admit(r,1.1,6.1),"lever needs near partner");
  ++r.number; Check(requests.Admit(r,2.2,6),"near lever admitted");
  for (unsigned n=0;n<15;++n) { ++r.number; Check(requests.Admit(r,2.2,3),"bounded burst admitted"); }
  ++r.number; Check(!requests.Admit(r,2.2,3),"request rate cap");
  ++r.number; Check(requests.Admit(r,3.3,3),"rate window renews");
}
struct Transport final : ICoopTransport {
  Transport* peer=nullptr; bool blocked=false;
  std::deque<std::vector<uint8_t>> incoming;
  bool Send(std::span<const uint8_t> bytes,bool reliable) override {
    Message m; Check(Decode(bytes,m),"session wire valid");
    if (IsObjectMessage(m.type)) { Check(reliable,"objects always reliable"); if (blocked) return false; }
    peer->incoming.emplace_back(bytes.begin(),bytes.end()); return true;
  }
  bool Receive(std::vector<uint8_t>& out) override { if (incoming.empty()) return false; out=std::move(incoming.front()); incoming.pop_front(); return true; }
  void Poll(double) override {}
  TransportState State() const override { return TransportState::Ready; }
  TransportStats Stats() const override { return {}; }
  void Close() override {}
};
void Network() {
  Transport ht,gt; ht.peer=&gt; gt.peer=&ht;
  SessionConfig hc; hc.enabled=true; hc.role=Role::Host; hc.epoch=17; hc.compatibility={kProtocolVersion,"test","same","same"};
  auto gc=hc; gc.role=Role::Guest; gc.epoch=0;
  CoopSession host(hc,ht),guest(gc,gt);
  for (int n=0;n<20;++n) { host.Tick(n*.01); guest.Tick(n*.01); }
  Check(host.State()==SessionState::Connected && guest.State()==SessionState::Connected,"real session handshake");
  ObjectMailbox hm,gm;
  auto invalid=Packet(MessageType::ObjectRequest); invalid.object.value=0x01000000;
  Check(!gm.Submit(invalid),"malformed request cannot poison mailbox head");
  Check(hm.Submit(Packet()),"host outcome queued"); hm.Exchange(host); ht.blocked=true; host.Tick(.21); guest.Tick(.21); gm.Exchange(guest);
  Check(gm.Take().empty(),"backpressure retains outcome");
  ht.blocked=false; host.Tick(.22); guest.Tick(.22); gm.Exchange(guest);
  auto received=gm.Take(); Check(received.size()==1 && received[0].object==Packet().object,"reliable retry reaches guest");
  auto raw=Packet(MessageType::ObjectRequest); raw.object.value=0x800010; raw.object.position=42; raw.object.seconds=.125f;
  Check(gm.Submit(raw),"guest raw hit queued"); gm.Exchange(guest); guest.Tick(.23); host.Tick(.23); hm.Exchange(host);
  auto hits=hm.Take();
  Check(hits.size()==1 && hits[0].object==raw.object,"raw classification and magnitude survive real mailboxes and wire");
  Check(!guest.QueueObjects(Packet()),"guest cannot author outcomes");
  Check(!host.QueueObjects(Packet(MessageType::ObjectRequest)),"host cannot impersonate guest intent");
  auto old=Packet(); old.epoch=18; Check(!host.QueueObjects(old),"stale epoch refused");
  for (unsigned n=0;n<kStoryQueue;++n) Check(gm.Submit(Packet(MessageType::ObjectRequest)),"bounded mailbox capacity");
  Check(!gm.Submit(Packet(MessageType::ObjectRequest)),"mailbox overflow refused"); gm.Clear();
  Check(gm.Submit(Packet(MessageType::ObjectRequest)),"clear releases bounded queue");
  gm.Clear();
  for (unsigned n=0;n<600;++n) {
    auto update=Packet(); update.object.number=n+2; update.object.value=n;
    Check(hm.Submit(update),"authority update queued during simulated native load"); hm.Exchange(host);
    host.Tick(.3+n*.01); guest.Tick(.3+n*.01); gm.Exchange(guest);
  }
  received=gm.Take();
  Check(received.size()==1 && received[0].object.value==599,"slow guest load coalesces repeated snapshots without losing final destruction");
  auto next=Packet(); next.object.generation=3;
  Check(hm.Submit(next) && hm.Submit(Packet()),"new shared load followed by delayed old state queued"); hm.Exchange(host);
  host.Tick(6.31); guest.Tick(6.31); gm.Exchange(guest); received=gm.Take();
  Check(received.size()==1 && received[0].object.generation==3,"new generation discards old pending snapshot states");
  Transport nt,nu; nt.peer=&nu; nu.peer=&nt; hc.epoch=18;
  CoopSession next_host(hc,nt),next_guest(gc,nu);
  for (int n=0;n<20;++n) { next_host.Tick(n*.01); next_guest.Tick(n*.01); }
  auto replacement=Packet(); replacement.epoch=18;
  Check(hm.Submit(replacement),"replacement session outcome submitted before mailbox first sees new connected session");
  hm.Exchange(next_host); next_host.Tick(.21); next_guest.Tick(.21); gm.Exchange(next_guest); received=gm.Take();
  Check(received.size()==1 && received[0].epoch==18 && received[0].object.generation==2,"new epoch resets old native generation even if Connecting was not sampled");
}
}
int main() { Codec(); Policies(); Network(); std::puts("world object tests passed"); }
