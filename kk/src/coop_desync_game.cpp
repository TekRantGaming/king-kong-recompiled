#include "coop_desync_game.h"
#include "coop_story_game.h"
#include "coop_enemies_game.h"
#include "coop_objects_game.h"
#include "coop_partner_body.h"
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
desync::Withheld withheld;
EnemyWorld recovery_world;
std::atomic<uint64_t> summary_epoch{0},quiet_count{0},move_count{0},reload_count{0};
std::atomic<uint64_t> samples{0},captures{0};
std::array<std::atomic<uint64_t>,4> peer_summary{};
WorldAudit audit,peer_audit;
double peer_audit_at=0,next_audit=0;
uint64_t pages_received=0,pages_sent=0,rejected_age=0,rejected_pose=0,rejected_scope=0,capture_failed=0;
std::vector<WorldProbe> Probes(const uint8_t* b) {
  auto out=kk::PartnerNpcDigest(b),objects=ObjectsWorldDigest(b);
  out.insert(out.end(),objects.begin(),objects.end()); return out;
}
void Status(const FlowView& v,double now,const char* gate) {
  if (now<next_status) return;
  next_status=now+60;
  const auto& a=v.host ? peer_audit:audit;
  REXLOG_INFO("KK co-op sync minute: role={} generation={} level={} gate={} captures={} pages-sent={} pages-received={} samples={} diffs-enemies={} diffs-npcs={} diffs-story={} diffs-objects={} diffs-players={} quiet={} moves={} guest-reloads={} peer-report-age={:.1f} rejected-age={} rejected-pose={} rejected-scope={} capture-failed={}; NPCs=observed-human-slots objects=bound-controllers players=world-and-checkpoint",
      v.host ? "host":"guest",generation,level,gate,captures.load(),pages_sent,pages_received,a.samples,
      a.diffs[0],a.diffs[1],a.diffs[2],a.diffs[3],a.diffs[4],a.quiet,a.moves,a.reloads,
      v.host && peer_audit_at ? now-peer_audit_at:-1.,rejected_age,rejected_pose,rejected_scope,capture_failed);
}
void SendAudit(double now) {
  if (!audit.samples || now<next_audit) return;
  audit.quiet=quiet_count; audit.moves=move_count; audit.reloads=reload_count;
  Message m; m.type=MessageType::WorldAudit; m.epoch=epoch; m.audit=audit;
  if (SubmitStory(std::move(m))) next_audit=now+10;
}
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
    quiet_count=move_count=reload_count=0; samples=captures=0; generation=level=0; withheld={};
    REXLOG_INFO("KK co-op desync: session checker role={} auto-repair={} source={}; when off, drift moves and guest reloads are logged, not applied",
        v.host ? "host":"guest",REXCVAR_GET(kk_coop_auto_repair),int(rex::cvar::GetFlagSource("kk_coop_auto_repair")));
#if defined(KK_DEV_TOOLS)
    forced_done=0; forced_injected=false; fixture_started=now;
#endif
  }
  if (generation==v.world.generation && level==v.world.level) return;
  generation=v.world.generation; level=v.world.level; guard={}; assembly={}; received.reset(); outgoing.clear();
  audit={}; audit.generation=generation; audit.level=level; peer_audit={}; peer_audit_at=next_audit=0;
  for (auto& count:peer_summary) count=0;
  pages_received=pages_sent=rejected_age=rejected_pose=rejected_scope=capture_failed=0;
  pending=in_flight=desync::Tier::None; recovery_world={}; sweep=0; grace=now+20; next_sample=now+3; next_status=now;
}
void Capture(uint8_t* base,const FlowView& v) {
  auto scripts=StoryWorldDigest(base); auto enemies=EnemyWorldDigest(base);
  auto probes=Probes(base);
  if (scripts.size()>kStorySnapshotScripts || enemies.size()>kMaxEnemies || probes.size()>kMaxWorldProbes || sweep==UINT32_MAX) { ++capture_failed; return; }
  WorldDigestPage header; header.generation=v.world.generation; header.level=v.world.level; header.start=v.world.start;
  header.captured=Now();
  header.enemy_world=EnemyCurrentWorld();
  if (!ValidEnemyWorld(header.enemy_world) || header.enemy_world.level!=header.level || header.enemy_world.start!=header.start) { ++capture_failed; return; }
  header.checkpoint=v.checkpoint; header.completed=v.completed; header.sweep=++sweep;
  ++captures;
  header.script_count=uint16_t(scripts.size()); header.enemy_count=uint16_t(enemies.size());
  header.probe_count=uint16_t(probes.size());
  const auto core=DigestCoreParts(header);
  header.parts=uint16_t(core+(probes.size()+kDigestProbesPerPage-1)/kDigestProbesPerPage);
  for (uint16_t part=0;part<header.parts;++part) {
    auto page=header; page.part=part;
    const auto sb=std::min(scripts.size(),size_t(part)*kDigestScriptsPerPage);
    const auto eb=std::min(enemies.size(),size_t(part)*kDigestEnemiesPerPage);
    page.scripts.assign(scripts.begin()+sb,scripts.begin()+std::min(scripts.size(),sb+kDigestScriptsPerPage));
    page.enemies.assign(enemies.begin()+eb,enemies.begin()+std::min(enemies.size(),eb+kDigestEnemiesPerPage));
    if (part>=core) {
      const auto pb=std::min(probes.size(),size_t(part-core)*kDigestProbesPerPage);
      page.probes.assign(probes.begin()+pb,probes.begin()+std::min(probes.size(),pb+kDigestProbesPerPage));
    }
    Message m; m.type=MessageType::WorldDigest; m.epoch=epoch; m.digest=std::move(page); outgoing.push_back(std::move(m));
  }
}
}
void WorldGuardReceive(const Message& m) {
  const auto v=SessionFlowSnapshot(); const double now=Now();
  if (m.type==MessageType::WorldAudit) {
    if (!v.host || !v.active || v.policy!=ChapterPolicy::Together || m.epoch!=StorySessionEpoch() ||
        m.audit.generation!=v.world.generation || m.audit.level!=v.world.level) return;
    Scope(v,m.epoch,now);
    if (!desync::AuditFollows(m.audit,peer_audit,sweep)) return;
    peer_audit=m.audit; peer_audit_at=now;
    peer_summary[0]=m.audit.samples; peer_summary[1]=m.audit.quiet;
    peer_summary[2]=m.audit.moves; peer_summary[3]=m.audit.reloads; return;
  }
  if (v.host || !v.active || v.policy!=ChapterPolicy::Together || m.epoch!=StorySessionEpoch() ||
      m.digest.generation!=v.world.generation || m.digest.level!=v.world.level || m.digest.start!=v.world.start) { ++rejected_scope; return; }
  Scope(v,m.epoch,now);
  ++pages_received;
  if (auto complete=assembly.Push(m.digest,now)) { received=std::move(complete); received_at=now; }
}
void WorldGuardFrame(uint8_t* base) {
  const auto v=SessionFlowSnapshot(); const auto link=EnemyLinkSnapshot(); const double now=Now();
  if (now-pending_at>4) pending=desync::Tier::None;
  if (!REXCVAR_GET(kk_coop_enabled) || !base || !v.active || v.phase==FlowPhase::Failed ||
      link.session_state==SessionState::Ended) { WorldGuardSummary(); pending=desync::Tier::None; return; }
  const auto session_epoch=StorySessionEpoch();
  if (!v.world.generation || !session_epoch) return;
  Scope(v,session_epoch,now);
  const char* gate=v.phase!=FlowPhase::Playing ? "flow" : v.policy!=ChapterPolicy::Together ? "independent" :
      !link.connected ? "link" : SessionPrivateReload() || SessionCheckpointCatchup() ? "private-load" :
      play::StartLevelLoading() || play::CoverScreen() ? "loading" :
      play::PauseMenuOpen() || play::GamePausePageUp() ? "pause" :
      LocalPlayerCondition(base)==PlayerCondition::Dead ? "dead" : nullptr;
  const bool cinematic=observer::Active() || observer::InCutscene(base);
  Status(v,now,gate ? gate : cinematic ? "cinematic-observe" : "playing");
  if (gate) {
    guard.Suspend(); received.reset(); pending=desync::Tier::None; return;
  }
  if (v.host) {
    if (outgoing.empty() && now>=next_sample) { Capture(base,v); next_sample=now+3; }
    for (unsigned i=0;i<4 && !outgoing.empty();++i) {
      if (!SubmitStory(outgoing.front())) break;
      ++pages_sent;
      outgoing.pop_front();
    }
    return;
  }
  SendAudit(now);
  if (!received || in_flight!=desync::Tier::None) return;
  auto state=std::move(*received); received.reset();
  if (now-received_at>1) { ++rejected_age; guard.Suspend(); pending=desync::Tier::None; return; }
  if (!desync::FreshForPose(state.header,link.partner)) { ++rejected_pose; guard.Suspend(); pending=desync::Tier::None; return; }
  auto evidence=StoryCompareDigest(base,state,false);
  evidence.progress^=uint64_t(SessionNativeCheckpoint())*1099511628211ull;
#if defined(KK_DEV_TOOLS)
  const auto forced=Fixture(now);
  if (forced==desync::Tier::Quiet) EnemyDevWorldDrift(base);
  if (forced==desync::Tier::Reload && !forced_injected) forced_injected=EnemyDevWorldGenerationDrift();
#endif
  const auto enemy=EnemyCompareDigest(base,state,false);
  const auto probe_diffs=desync::CompareProbes(state.probes,Probes(base));
  ++audit.samples; audit.sweep=state.header.sweep;
  audit.diffs[0]+=enemy.differences!=0; audit.diffs[1]+=probe_diffs[0]!=0;
  audit.diffs[2]+=evidence.Any(); audit.diffs[3]+=probe_diffs[1]!=0;
  audit.diffs[4]+=state.header.checkpoint!=SessionNativeCheckpoint() || state.header.completed!=v.completed || enemy.wrong_level;
  evidence.progress^=enemy.progress;
  evidence.quiet|=enemy.quiet; evidence.structural|=enemy.structural;
    evidence.structural_anchor=enemy.structural_anchor;
  evidence.wrong_level|=enemy.wrong_level;
  const auto game=hfr::Load32(base,0x82CC97C0);
  // This is the same validated game global used by the flow adapter.
  if (PoseAddress(game,28)) evidence.wrong_level|=hfr::Load32(base,game+0x18)!=v.world.level;
  if (evidence.wrong_level) evidence.progress=1; // Progress in the wrong world cannot repair its identity.
#if defined(KK_DEV_TOOLS)
  if (forced==desync::Tier::Move) { evidence.lag=true; evidence.progress=1; }
#endif
  ++samples;
  // Observe during cinematics and initial grace too. Only recovery needs the
  // quiet presentation window; native AI combat and movement do not stop sampling.
  if (cinematic || now<grace || state.header.checkpoint!=v.checkpoint) {
    guard.Suspend(); pending=desync::Tier::None; return;
  }
  const auto decision=guard.Observe(generation,level,v.checkpoint,evidence,now);
  if (decision==desync::Tier::None) pending=desync::Tier::None;
  if (desync::Applied(decision,REXCVAR_GET(kk_coop_auto_repair))!=decision) {
    // RO1: measured and logged, never applied. The guard budget is not consumed,
    // so persistent drift keeps reporting once a minute and quiet repairs keep
    // their own cadence.
    pending=desync::Tier::None;
    const auto kind=decision==desync::Tier::Move ? desync::Withheld::kMove : desync::Withheld::kReload;
    if (withheld.Note(kind,now))
      REXLOG_INFO("KK co-op desync: decision={} not applied (auto repair off) generation={} level={} checkpoint={} quiet={} lag={} structural={} wrong-world={} scripts={} enemies={} withheld-moves={} withheld-reloads={}; guest stays, host continues",
          desync::Name(decision),generation,level,v.checkpoint,evidence.quiet,evidence.lag,evidence.structural,evidence.wrong_level,
          state.scripts.size(),state.enemies.size(),withheld.Count(desync::Withheld::kMove),withheld.Count(desync::Withheld::kReload));
#if defined(KK_DEV_TOOLS)
    // A forced fixture sequence continues to its next tier, as after a completed repair.
    if (Fixture(now)==decision) forced_done|=1u<<unsigned(decision);
#endif
    return;
  }
  if (decision!=desync::Tier::None && pending==desync::Tier::None) {
    REXLOG_INFO("KK co-op desync: decision={} generation={} level={} checkpoint={} quiet={} lag={} structural={} wrong-world={} scripts={} enemies={}",
        desync::Name(decision),generation,level,v.checkpoint,evidence.quiet,evidence.lag,evidence.structural,evidence.wrong_level,
        state.scripts.size(),state.enemies.size());
    if (decision==desync::Tier::Quiet) {
      StoryCompareDigest(base,state,true); EnemyCompareDigest(base,state,true);
      WorldGuardCompleted(decision);
    } else { pending=decision; pending_at=now; recovery_world=state.header.enemy_world; }
  }
}
desync::Tier WorldGuardPending() { return Now()-pending_at<=4 ? pending : desync::Tier::None; }
bool WorldGuardWithhold(desync::Withheld::Kind kind) { return withheld.Note(kind,Now()); }
uint64_t WorldGuardWithheld(desync::Withheld::Kind kind) { return withheld.Count(kind); }
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
  next_audit=0; SendAudit(Now());
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
  if (ended) REXLOG_INFO("KK co-op desync session-end: local quiet={} moves={} guest-reloads={} captures={} samples={}; guest-report samples={} quiet={} moves={} guest-reloads={}; auto-repair={} not-applied moves={} guest-reloads={} story-catchups={}",
      quiet_count.load(),move_count.load(),reload_count.load(),captures.load(),samples.load(),
      peer_summary[0].load(),peer_summary[1].load(),peer_summary[2].load(),peer_summary[3].load(),
      REXCVAR_GET(kk_coop_auto_repair),withheld.Count(desync::Withheld::kMove),withheld.Count(desync::Withheld::kReload),
      withheld.Count(desync::Withheld::kStoryCatchup));
}
} // namespace kk::coop
