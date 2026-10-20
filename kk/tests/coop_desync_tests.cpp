#include "coop_desync.h"
#include "coop_flow.h"
#include "coop_session.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace kk::coop;
namespace {
void Check(bool ok,const char* why) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); } }
WorldDigestPage Page(uint16_t part=0) {
  WorldDigestPage p; p.generation=1; p.level=101; p.start=1; p.checkpoint=2; p.sweep=1;
  p.parts=2; p.part=part; p.script_count=17; p.enemy_count=13;
  const size_t ns=part ? 1:16,ne=part ? 1:12;
  for (size_t i=0;i<ns;++i) p.scripts.push_back({1+i+part*16,99,2,3,5});
  for (size_t i=0;i<ne;++i) p.enemies.push_back({{1+i+part*12,0,EnemyFamily::Raptor},EnemyOwner::Host,0,100,0});
  return p;
}
void Codec() {
  for (auto part:{0,1}) {
    Message m; m.type=MessageType::WorldDigest; m.epoch=9; m.digest=Page(uint16_t(part));
    std::vector<uint8_t> bytes; Check(Encode(m,bytes) && bytes.size()<=kMaxPacketBytes,"maximum digest fits datagram");
    Message out; Check(Decode(bytes,out) && out.digest==m.digest,"canonical digest roundtrip");
    for (size_t n=0;n<bytes.size();++n) {
      out.epoch=777;
      Check(!Decode(std::span(bytes).first(n),out) && out.epoch==777,"all truncations fail atomically");
    }
    bytes.push_back(0); Check(!Decode(bytes,out),"trailing byte rejected");
  }
  for (int bad=0;bad<11;++bad) {
    Message m; m.type=MessageType::WorldDigest; m.digest=Page();
    switch (bad) {
      case 0:m.digest.scripts[0].record=kStoryRecords+1;break;
      case 1:m.digest.enemies[0].life=std::numeric_limits<float>::quiet_NaN();break;
      case 2:m.digest.parts=257;break;
      case 3:m.digest.part=2;break;
      case 4:m.digest.extensions=1;break;
      case 5:m.digest.enemies[0].owner=EnemyOwner(2);break;
      case 6:m.digest.scripts[0].flags=8;break;
      case 7:m.digest.scripts.pop_back();break;
      case 8:m.digest.enemy_count=513;break;
      case 9:m.digest.scripts[0].source=0;break;
      case 10:m.digest.captured=std::numeric_limits<double>::quiet_NaN();break;
    }
    std::vector<uint8_t> bytes; Check(!Encode(m,bytes),"invalid digest bounded on send");
  }
}
void Assembly() {
  desync::Assembly a;
  Check(!a.Push(Page(1),1),"missing begin never compares partial world");
  Check(!a.Push(Page(),2) && !a.Push(Page(),3),"duplicate begin ignored");
  auto state=a.Push(Page(1),4);
  Check(state && state->scripts.size()==17 && state->enemies.size()==13,"complete sweep assembles");
  Check(!a.Push(Page(),5) && !a.Push(Page(1),5),"completed sweep cannot replay");
  a={}; a.Push(Page(),1); auto mixed=Page(1); ++mixed.checkpoint;
  Check(!a.Push(mixed,2),"mixed checkpoint cannot assemble");
  Check(!a.Push(Page(1),10),"expired complete sweep never creates evidence");
  a={}; auto duplicate=Page(1); duplicate.enemies[0]=Page().enemies[0];
  a.Push(Page(),1); Check(!a.Push(duplicate,2),"duplicate enemy identities rejected across pages");
}
void Hysteresis() {
  auto page=Page(); page.captured=100000;
  Check(desync::FreshForPose(page,PlayerPose{100000.3,101}),"host clocks compare without receiver clock offset");
  Check(!desync::FreshForPose(page,PlayerPose{100006,101}) && !desync::FreshForPose(page,{}),"reliable backlog or missing pose cannot become fresh drift evidence");
  using desync::Tier; desync::Guard g; desync::Evidence lag; lag.lag=true; lag.progress=7;
  for (double t=1;t<=19;t+=3) Check(g.Observe(1,101,1,lag,t)==Tier::None,"normal lag grace");
  Check(g.Observe(1,101,1,lag,22)==Tier::Move,"persistent stationary lag moves automatically");
  g.Acted(Tier::Move,22);
  for (double t=25;t<=43;t+=3) Check(g.Observe(1,101,1,lag,t)==Tier::None,"post-move grace");
  Check(g.Observe(1,101,1,lag,46)==Tier::Reload,"persistent post-move lag reloads guest");
  g.Acted(Tier::Reload,46);
  for (double t=49;t<300;t+=3) Check(g.Observe(1,101,1,lag,t)==Tier::None,"same checkpoint never loops");
  for (double t=301;t<=319;t+=3) Check(g.Observe(1,101,2,lag,t)==Tier::None,"new checkpoint starts fresh grace");
  Check(g.Observe(1,101,2,lag,322)==Tier::Move,"new checkpoint has bounded recovery budget");
  for (double t=325;t<380;t+=3) Check(g.Observe(1,101,1,lag,t)==Tier::None,"checkpoint rollback cannot renew a consumed budget");
  g={};
  for (double t=1;t<180;t+=3) { ++lag.progress; Check(g.Observe(1,101,1,lag,t)==Tier::None,"native progress prevents false recovery"); }
  g={}; lag.progress=1;
  for (double t=1;t<180;t+=9) Check(g.Observe(1,101,1,lag,t)==Tier::None,"packet silence is not desync");
  g={}; desync::Evidence quiet; quiet.quiet=true;
  Check(g.Observe(1,101,1,quiet,1)==Tier::None && g.Observe(1,101,1,quiet,4)==Tier::None &&
      g.Observe(1,101,1,quiet,7)==Tier::Quiet,"small drift reconciles after repeated samples");
  quiet.quiet=false;
  Check(g.Observe(1,101,1,quiet,10)==Tier::None,"convergence clears evidence");
  g={}; desync::Evidence wrong; wrong.wrong_level=true;
  for (double t=1;t<=19;t+=3) Check(g.Observe(1,101,1,wrong,t)==Tier::None,"wrong-level grace survives transitions");
  Check(g.Observe(1,101,1,wrong,22)==Tier::Reload,"large drift bypasses pointless teleport");
  CheckpointCatchup catchup; catchup.Reset(1,1);
  Check(!catchup.Begin() && catchup.Begin(false,true) && !catchup.Begin(false,true),"same initial checkpoint reload allowed once only for guard");
}
void Mailbox() {
  StoryMailbox box; Message m; m.type=MessageType::WorldDigest; m.epoch=17; m.digest=Page();
  Check(box.Submit(m),"digest shares bounded value-only story mailbox");
  Check(StorySender(m.type,Role::Host) && !StorySender(m.type,Role::Guest),"only host can send digest");
  m.digest.extensions=1; Check(!box.Submit(m),"bad extension rejected before queue");
}
}
int main() { Codec(); Assembly(); Hysteresis(); Mailbox(); std::puts("World digest codec, complete census, recovery tiers and loop guards passed"); }
