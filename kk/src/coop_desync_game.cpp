#include "coop_desync_game.h"
#include "coop_story_game.h"
#include "coop_enemies_game.h"
#include "coop_runtime.h"
#include "coop_pose.h"
#include "coop_observer.h"
#include "play_internal.h"
#include "hfr.h"
#include "settings.h"
#include <rex/logging.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>

namespace kk::coop {
namespace {
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
desync::Guard guard;
desync::Assembly assembly;
std::optional<desync::State> received;
std::deque<Message> outgoing;
uint64_t epoch=0;
uint32_t generation=0,level=0,sweep=0;
double next_sample=0,grace=0,received_at=0,next_status=0,pending_at=0;
desync::Tier pending=desync::Tier::None,in_flight=desync::Tier::None;
EnemyWorld recovery_world;
std::atomic<uint64_t> summary_epoch{0},quiet_count{0},move_count{0},reload_count{0};
uint64_t samples=0;
#if defined(KK_DEV_TOOLS)
unsigned forced_done=0;
bool forced_injected=false;
double fixture_started=0;
desync::Tier Fixture(double now) {
  const char* value=std::getenv("KK_DEV_DESYNC");
  if (!value) return desync::Tier::None;
  // Optional comma-separated sequence exercises all tiers in one private run.
  const std::string_view script(value); if (script.size()>256) return desync::Tier::None;
  for (size_t start=0;start<script.size();) {
    const auto end=script.find(',',start);
    const auto v=script.substr(start,end==script.npos ? script.size()-start : end-start);
    const auto colon=v.find(':'); if (colon==v.npos) return desync::Tier::None;
    const auto name=v.substr(0,colon);
    const auto tier=name=="quiet" ? desync::Tier::Quiet : name=="move" ? desync::Tier::Move :
        name=="reload" ? desync::Tier::Reload : desync::Tier::None;
    const double delay=std::strtod(value+start+colon+1,nullptr);
    if (tier==desync::Tier::None || !std::isfinite(delay) || delay<1) return desync::Tier::None;
    if (!(forced_done & (1u<<unsigned(tier))) && now-fixture_started>=delay) return tier;
    if (end==script.npos) break;
    start=end+1;
  }
  return desync::Tier::None;
}
#endif
void Scope(const FlowView& v,uint64_t current,double now) {
  if (epoch!=current) {
    WorldGuardSummary(); epoch=current; summary_epoch=current;
    quiet_count=move_count=reload_count=0; samples=0; generation=level=0;
#if defined(KK_DEV_TOOLS)
    forced_done=0; forced_injected=false; fixture_started=now;
#endif
  }
  if (generation==v.world.generation && level==v.world.level) return;
  generation=v.world.generation; level=v.world.level; guard={}; assembly={}; received.reset(); outgoing.clear();
  pending=in_flight=desync::Tier::None; recovery_world={}; sweep=0; grace=now+20; next_sample=now+3; next_status=now;
}
void Capture(uint8_t* base,const FlowView& v) {
  auto scripts=StoryWorldDigest(base); auto enemies=EnemyWorldDigest(base);
  if (scripts.size()>kStorySnapshotScripts || enemies.size()>kMaxEnemies || sweep==UINT32_MAX) return;
  WorldDigestPage header; header.generation=v.world.generation; header.level=v.world.level; header.start=v.world.start;
  header.captured=Now();
  header.enemy_world=EnemyCurrentWorld();
  if (!ValidEnemyWorld(header.enemy_world) || header.enemy_world.level!=header.level || header.enemy_world.start!=header.start) return;
  header.checkpoint=v.checkpoint; header.completed=v.completed; header.sweep=++sweep;
  header.script_count=uint16_t(scripts.size()); header.enemy_count=uint16_t(enemies.size());
  header.parts=uint16_t(std::max<size_t>({1,(scripts.size()+15)/16,(enemies.size()+11)/12}));
  for (uint16_t part=0;part<header.parts;++part) {
    auto page=header; page.part=part;
    const auto sb=std::min(scripts.size(),size_t(part)*kDigestScriptsPerPage);
    const auto eb=std::min(enemies.size(),size_t(part)*kDigestEnemiesPerPage);
    page.scripts.assign(scripts.begin()+sb,scripts.begin()+std::min(scripts.size(),sb+kDigestScriptsPerPage));
    page.enemies.assign(enemies.begin()+eb,enemies.begin()+std::min(enemies.size(),eb+kDigestEnemiesPerPage));
    Message m; m.type=MessageType::WorldDigest; m.epoch=epoch; m.digest=std::move(page); outgoing.push_back(std::move(m));
  }
}
}
void WorldGuardReceive(const Message& m) {
  const auto v=SessionFlowSnapshot(); const double now=Now();
  if (v.host || !v.active || v.policy!=ChapterPolicy::Together || m.epoch!=StorySessionEpoch() ||
      m.digest.generation!=v.world.generation || m.digest.level!=v.world.level || m.digest.start!=v.world.start) return;
  Scope(v,m.epoch,now);
  if (auto complete=assembly.Push(m.digest,now)) { received=std::move(complete); received_at=now; }
}
void WorldGuardFrame(uint8_t* base) {
  const auto v=SessionFlowSnapshot(); const auto link=EnemyLinkSnapshot(); const double now=Now();
  if (now-pending_at>4) pending=desync::Tier::None;
  if (!REXCVAR_GET(kk_coop_enabled) || !base || !v.active || v.phase==FlowPhase::Failed ||
      link.session_state==SessionState::Ended) { WorldGuardSummary(); pending=desync::Tier::None; return; }
  if (!v.world.generation || !link.epoch) return;
  Scope(v,link.epoch,now);
  if (v.phase!=FlowPhase::Playing || v.policy!=ChapterPolicy::Together || !link.connected ||
      SessionPrivateReload() || SessionCheckpointCatchup() || observer::Active() || observer::InCutscene(base) ||
      play::StartLevelLoading() || play::CoverScreen() || play::PauseMenuOpen() || play::GamePausePageUp() ||
      LocalPlayerCondition(base)==PlayerCondition::Dead) {
    guard.Suspend(); received.reset(); pending=desync::Tier::None; return;
  }
  if (v.host) {
    if (outgoing.empty() && now>=next_sample) { Capture(base,v); next_sample=now+3; }
    for (unsigned i=0;i<4 && !outgoing.empty();++i) {
      if (!SubmitStory(outgoing.front())) break;
      outgoing.pop_front();
    }
    return;
  }
  if (!received || in_flight!=desync::Tier::None) return;
  auto state=std::move(*received); received.reset();
  if (now<grace || now-received_at>1 || state.header.checkpoint!=v.checkpoint ||
      !desync::FreshForPose(state.header,link.partner)) { guard.Suspend(); pending=desync::Tier::None; return; }
  auto evidence=StoryCompareDigest(base,state,false);
  evidence.progress^=uint64_t(SessionNativeCheckpoint())*1099511628211ull;
#if defined(KK_DEV_TOOLS)
  const auto forced=Fixture(now);
  if (forced==desync::Tier::Quiet) EnemyDevWorldDrift(base);
  if (forced==desync::Tier::Reload && !forced_injected) forced_injected=EnemyDevWorldGenerationDrift();
#endif
  const auto enemy=EnemyCompareDigest(base,state,false);
  evidence.progress^=enemy.progress;
  evidence.quiet|=enemy.quiet; evidence.structural|=enemy.structural;
  evidence.wrong_level|=enemy.wrong_level;
  const auto game=hfr::Load32(base,0x82CC97C0);
  // This is the same validated game global used by the flow adapter.
  if (PoseAddress(game,28)) evidence.wrong_level|=hfr::Load32(base,game+0x18)!=v.world.level;
  if (evidence.wrong_level) evidence.progress=1; // Progress in the wrong world cannot repair its identity.
#if defined(KK_DEV_TOOLS)
  if (forced==desync::Tier::Move) { evidence.lag=true; evidence.progress=1; }
#endif
  ++samples;
  const auto decision=guard.Observe(generation,level,v.checkpoint,evidence,now);
  if (decision==desync::Tier::None) pending=desync::Tier::None;
  if (decision!=desync::Tier::None && pending==desync::Tier::None) {
    REXLOG_INFO("KK co-op desync: decision={} generation={} level={} checkpoint={} quiet={} lag={} structural={} wrong-world={} scripts={} enemies={}",
        desync::Name(decision),generation,level,v.checkpoint,evidence.quiet,evidence.lag,evidence.structural,evidence.wrong_level,
        state.scripts.size(),state.enemies.size());
    if (decision==desync::Tier::Quiet) {
      StoryCompareDigest(base,state,true); EnemyCompareDigest(base,state,true);
      WorldGuardCompleted(decision);
    } else { pending=decision; pending_at=now; recovery_world=state.header.enemy_world; }
  }
  if (now>=next_status) {
    next_status=now+15;
    REXLOG_INFO("KK co-op desync: samples={} generation={} checkpoint={} pending={} quiet={} moves={} guest-reloads={}",
        samples,generation,v.checkpoint,desync::Name(pending),quiet_count.load(),move_count.load(),reload_count.load());
  }
}
desync::Tier WorldGuardPending() { return Now()-pending_at<=4 ? pending : desync::Tier::None; }
void WorldGuardStarted(desync::Tier tier) {
  if (tier==desync::Tier::Reload) EnemyGuardRecoveryWorld(recovery_world);
  pending=desync::Tier::None; in_flight=tier;
  // Consume the checkpoint budget on scheduling too, so a failed/deferred
  // native action cannot be requested every frame.
  guard.Acted(tier,Now());
}
void WorldGuardCompleted(desync::Tier tier) {
  if (tier==desync::Tier::Quiet) ++quiet_count;
  if (tier==desync::Tier::Move) ++move_count;
  if (tier==desync::Tier::Reload) ++reload_count;
  if (tier==desync::Tier::Move || tier==desync::Tier::Reload) { in_flight=pending=desync::Tier::None; guard.Acted(tier,Now()); }
  REXLOG_INFO("KK co-op desync: completed={} generation={} level={} quiet={} moves={} guest-reloads={}; host continues",
      desync::Name(tier),generation,level,quiet_count.load(),move_count.load(),reload_count.load());
#if defined(KK_DEV_TOOLS)
  if (Fixture(Now())==tier) forced_done|=1u<<unsigned(tier);
#endif
}
void WorldGuardLoaded() { assembly={}; received.reset(); guard.Suspend(); pending=desync::Tier::None; grace=Now()+20; }
void WorldGuardCancelled(desync::Tier tier) {
  if (tier==desync::Tier::Reload) EnemyGuardRecoveryWorld({});
  if (in_flight==tier) in_flight=pending=desync::Tier::None;
  REXLOG_INFO("KK co-op desync: deferred={} generation={} level={}; checkpoint attempt consumed, host continues",desync::Name(tier),generation,level);
}
void WorldGuardSummary() {
  const auto ended=summary_epoch.exchange(0);
  if (ended) REXLOG_INFO("KK co-op desync session-end: quiet={} moves={} guest-reloads={}",quiet_count.load(),move_count.load(),reload_count.load());
}
} // namespace kk::coop
