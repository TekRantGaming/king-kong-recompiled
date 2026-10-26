#include "coop_objects.h"
#include "coop_support.h"
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
  for (auto type : {MessageType::ObjectState,MessageType::ObjectRequest}) for (auto kind : {ObjectKind::Breakable,ObjectKind::Door,ObjectKind::Fire,ObjectKind::Raft,ObjectKind::Ode,ObjectKind::Tree,ObjectKind::SupportBody}) {
    if (type==MessageType::ObjectRequest && (kind==ObjectKind::Raft || kind==ObjectKind::SupportBody)) continue;
    auto m=Packet(type,kind);
    if (type==MessageType::ObjectRequest && kind==ObjectKind::Breakable) { m.object.value=0x800010; m.object.position=42; m.object.seconds=.125f; }
    if (type==MessageType::ObjectState && kind==ObjectKind::Door) { m.object.position=.375f; m.object.seconds=2.5f; m.object.value=511; }
    if (type==MessageType::ObjectState && kind==ObjectKind::Fire) { m.object.position=37.5f; m.object.seconds=-1; }
    if (kind==ObjectKind::Raft) { m.object.raft.route=123; m.object.raft.pose[3]=m.object.raft.pose[8]=1; }
    if (objects::SupportRoot(kind)) {
      m.object.collapse.topology=123;
      if (type==MessageType::ObjectRequest) m.object.collapse.part=3;
      else m.object.collapse.trigger=1;
    }
    if (kind==ObjectKind::SupportBody) { m.object.body.topology=123; m.object.body.pose[3]=m.object.body.pose[8]=1; }
    std::vector<uint8_t> bytes; Check(Encode(m,bytes),"object encodes");
    Check(bytes.size()==kHeaderBytes+(kind==ObjectKind::Raft ? 91 : objects::SupportRoot(kind) ? 67 : kind==ObjectKind::SupportBody ? 81 : 33) && bytes[5]==kProtocolVersion,"bounded protocol 20 object wire");
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
  raft=Packet(MessageType::ObjectState,ObjectKind::Raft).object; raft.value=2;
  Check(ValidObjectState(raft) && !ValidObjectRequest(raft),"host may withdraw a no longer supported route");
  auto withdrawal=Packet(); withdrawal.object=raft; Message decoded;
  Check(Encode(withdrawal,bytes) && Decode(bytes,decoded) && decoded.object==raft,"route withdrawal survives the complete object codec");
  raft.raft.route=1; Check(!ValidObjectState(raft),"raft withdrawal cannot smuggle a stale pose");
}
void Policies() {
  std::array<float,9> basis{1,0,0,0,1,0,0,0,1};
  Check(objects::RigidSupportBasis(basis),"unit support rotations can retain their complete orientation");
  basis={0,1,0,-1,0,0,0,0,1};
  Check(objects::RigidSupportBasis(basis),"a rotated unit support basis remains representable");
  basis[8]=-1;
  Check(!objects::RigidSupportBasis(basis),"a reflected support cannot lose handedness through forward and up replay");
  basis={1,0,0,0,1,.1f,0,0,1};
  Check(!objects::RigidSupportBasis(basis),"sheared support transforms remain outside rigid replay");
  basis={1,0,0,0,1,0,0,0,std::numeric_limits<float>::quiet_NaN()};
  Check(!objects::RigidSupportBasis(basis),"nonfinite support transforms cannot acquire physical ownership");
  std::map<uint32_t,uint32_t> physics{{0x42000070,0x43000000},{0x42000074,0x44000000},{0x43000010,0x44000000}};
  const auto body_read=[&](uint32_t a){return physics[a];};
  Check(objects::BodyHandle(body_read,0x42000000)==0x44000000,"physical body must belong to the actor's own geom");
  physics[0x42000074]=physics[0x43000010]=0;
  Check(objects::BodyHandle(body_read,0x42000000)==0,"native scene body detachment preserves the physical record identity");
  physics[0x42000074]=physics[0x43000010]=0x45000000;
  Check(objects::BodyHandle(body_read,0x42000000)==0x45000000,"native reactivation can bind a new owned body without retaining a freed handle");
  physics[0x43000010]=0x44000000;
  Check(!objects::BodyHandle(body_read,0x42000000),"mismatching geom and body refuse before native writes");
  std::map<uint32_t,uint32_t> model{{0x41000000,0x42000000},{0x42000000,0x43000000},
      {0x4300001C,0x44000000},{0x44000014,0x826E6390}};
  const auto model_read=[&](uint32_t a){return model[a];};
  Check(objects::DefaultTrack(model_read,0x41000000,0x826E6390),"a stopped tree retains its native model initial track identity");
  Check(!objects::DefaultTrack(model_read,0x41000000,0x82690148),"unrelated native initial tracks cannot acquire tree physics ownership");
  model[0x42000000]=0xFFFFFFF0;
  Check(!objects::DefaultTrack(model_read,0x41000000,0x826E6390),"invalid model headers refuse before following an initial track");
  const std::vector<objects::PartRoot> aliases{{200,{40,50,40}},{100,{30,40}}};
  const auto canonical=objects::CanonicalParts(aliases);
  Check(canonical.size()==3,"aliases within and across roots produce one physical body each");
  const auto relocated=objects::CanonicalParts({{100,{3000,4000}},{200,{4000,5000,4000}}});
  Check(relocated.at(4000).source==canonical.at(40).source && relocated.at(4000).topology==canonical.at(40).topology,
      "canonical physical identity survives heap relocation and census order");
  const auto missing_alias=objects::CanonicalParts({{100,{30,40}},{200,{40,50}}});
  Check(missing_alias.at(40).topology!=canonical.at(40).topology,"different alias ownership cannot reuse an accepted topology");
  Check(objects::CanonicalParts({{100,{30}},{100,{40}}}).empty(),"ambiguous root identities refuse the complete physical map");
  Check(objects::RootTopology(aliases[0],canonical)!=objects::RootTopology({200,{40,40,50}},canonical),"part reordering changes the native topology contract");
  auto collapse=Packet(MessageType::ObjectState,ObjectKind::Ode).object;
  collapse.collapse.topology=123; collapse.collapse.trigger=2; collapse.collapse.part=3;
  Check(ValidObjectState(collapse),"host part collapse input is bounded and typed");
  collapse.position=1; Check(ValidObjectState(collapse),"host tracked-member scene notification survives collapse replay");
  collapse.position=.5f; Check(!ValidObjectState(collapse),"tracked-member scene notification is a boolean");
  collapse.position=0;
  collapse.collapse.part=255; Check(!ValidObjectState(collapse),"hit trigger requires an own part ordinal");
  collapse.collapse.part=32; Check(!ValidObjectState(collapse),"native collapse part list bounded at32");
  collapse.collapse.part=3; collapse.collapse.impulse[0]=std::numeric_limits<float>::quiet_NaN();
  Check(!ValidObjectState(collapse),"nonfinite native impulses cannot enter replay");
  collapse.collapse.impulse[0]=0; collapse.value=0;
  Check(!ValidObjectState(collapse),"waiting authority cannot smuggle an activation");
  collapse.collapse.trigger=0; collapse.collapse.part=255;
  Check(ValidObjectState(collapse),"waiting host authority is explicit");
  collapse.value=1; collapse.collapse.part=2;
  Check(ValidObjectRequest(collapse),"guest can request a bounded hit on an exact support part");
  collapse.collapse.trigger=1; Check(!ValidObjectRequest(collapse),"guest cannot choose collapse phase or trigger");
  collapse.value=3; collapse.collapse={};
  Check(ValidObjectState(collapse) && !ValidObjectRequest(collapse),"unsupported or retired support authority explicitly releases the guest adapter");
  collapse.collapse.topology=123; Check(!ValidObjectState(collapse),"withdrawal cannot retain a hidden collapse input");
  auto body=Packet(MessageType::ObjectState,ObjectKind::SupportBody).object;
  body.body.topology=123; body.body.pose[3]=body.body.pose[8]=1;
  Check(ValidObjectState(body) && !ValidObjectRequest(body),"settled physical poses are host only");
  body.body.flags=0x80000000; Check(!ValidObjectState(body),"unrelated native body flags cannot cross the wire");
  body.body.flags=0; body.body.pose[8]=2;
  Check(!ValidObjectState(body),"scaled bodies cannot be silently normalized on the guest");
  std::map<uint32_t,objects::Binding> targets{{100,{100,200,0x42000000,300,10,ObjectKind::Breakable}},
      {101,{101,201,0x42001000,301,11,ObjectKind::Breakable}}};
  std::map<uint32_t,uint32_t> pieces{{0x42000014,500},{0x42001014,501}};
  const auto find_target=[&](uint32_t actor) { return objects::StrikeTarget(targets,[&](uint32_t a){return pieces[a];},[](const auto&){return true;},actor); };
  Check(find_target(500)==&targets.at(100) && find_target(101)==&targets.at(101),"thrown impacts resolve exact authored pieces and roots");
  pieces[0x42001014]=500;
  Check(!find_target(500) && !find_target(999) && !find_target(0),"shared and unrelated hit targets refuse provenance normalization");
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
  target=objects::RaftPathTarget(path_read,path_read,0x42000000,0);
  Check(target && target->key==123 && target->actor==0x47000000,"unspecified raft key resolves the checked first local group");
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
  auto raft=Packet(MessageType::ObjectState,ObjectKind::Raft);
  raft.object.raft.route=11; raft.object.raft.pose[3]=raft.object.raft.pose[8]=1;
  const auto transfer=[&](const Message& state,double t) {
    Check(hm.Submit(state),"raft authority control queued"); hm.Exchange(host);
    host.Tick(t); guest.Tick(t); gm.Exchange(guest); const auto arrived=gm.Take();
    Check(arrived.size()==1 && arrived[0].object==state.object,"raft pose or withdrawal reaches the real guest mailbox");
    return arrived[0].object;
  };
  objects::Replica raft_replica;
  Check(raft_replica.Observe(transfer(raft,.24)),"first raft pose retained for local binding");
  auto withdrawal=raft; withdrawal.object.value=2; withdrawal.object.raft={}; ++withdrawal.object.number;
  Check(raft_replica.Observe(transfer(withdrawal,.25)) && raft_replica.Find(ObjectKind::Raft,raft.object.source)->value==2,
      "route withdrawal replaces a retained moving pose");
  raft.object.number=3; raft.object.raft.route=12; raft.object.raft.pose[0]=37;
  Check(raft_replica.Observe(transfer(raft,.26)) && raft_replica.Find(ObjectKind::Raft,raft.object.source)->raft.route==12,
      "a new supported route replaces withdrawn authority");
  auto unavailable=withdrawal; unavailable.object.source^=0x1234; unavailable.object.number=1;
  Check(raft_replica.Observe(transfer(unavailable,.27)) && raft_replica.Find(ObjectKind::Raft,unavailable.object.source)->value==2,
      "an unsupported initial route needs no earlier pose to release authority");
  auto terminal=Packet(MessageType::ObjectState,ObjectKind::Ode);
  terminal.object.value=2; terminal.object.collapse.topology=77; terminal.object.collapse.trigger=2; terminal.object.collapse.part=3;
  terminal.object.collapse.impulse={1,2,3,40,50,60};
  objects::Replica support_replica;
  Check(support_replica.Observe(transfer(terminal,.28)),"terminal collapse retains its first activation even when intermediate events were coalesced");
  auto settled=Packet(MessageType::ObjectState,ObjectKind::SupportBody);
  settled.object.source=999; settled.object.body.topology=88; settled.object.body.pose={70,80,90,1,0,0,0,0,1};
  Check(support_replica.Observe(transfer(settled,.281)),"settled body survives real reliable transport and mailbox");
  Check(support_replica.Find(ObjectKind::Ode,terminal.object.source)->collapse.part==3 &&
      support_replica.Find(ObjectKind::SupportBody,999)->body.pose[1]==80,"private native identity replacement does not clear retained collapse or final pose");
  auto released=terminal; released.object.number=2; released.object.value=3; released.object.collapse={};
  Check(support_replica.Observe(transfer(released,.282)) && !support_replica.Observe(terminal.object),"withdrawn support cannot be resurrected by an older activation");
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
