#include "coop_runtime.h"
#include "coop_pose.h"
#include "coop_enemies_game.h"
#include "coop_observer.h"
#include "coop_story_game.h"
#include "play_internal.h"
#include "hfr.h"
#include "settings.h"
#include "mods.h"
#include "ui_paint.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

REX_EXTERN(__imp__sub_82413190);
namespace kk::coop {
bool BeginSessionFlow(SessionStart start, std::vector<uint8_t> save);
namespace {
std::filesystem::path personal, private_root;
play::StartRequest selected{play::StartKind::kContinue,-1,-1,{}};
bool requested=false, launched=false;
SocialId requested_lobby=0;
int progress_slot=-1;
std::atomic<int> session_cheats{-1};
std::atomic<bool> stopping{false};
std::atomic<bool> move_offer{false};
std::atomic<uint32_t> move_request{0};
uint32_t pending_move=0;
double move_until=0;
uint32_t loaded_generation=0, loading_generation=0, recorded_checkpoint=0;
uint64_t applied_progress=0;
BehindRule behind;
revive::Recovery recovery;
uint32_t recovery_generation=0, life_number=0, local_checkpoint=0;
uint64_t life_incarnation=1;
bool private_load=false, private_request=false, reload_sent=false;
std::optional<FlowEvent> independent_load;
bool sent_downed=false, life_sent=false;
double next_life=0;
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool EffectiveFlag(const char* name,const mods::Plan& plan) {
  auto value=rex::cvar::GetFlagByName(name);
  const auto source=rex::cvar::GetFlagSource(name);
  if (source!=rex::cvar::Source::kCommandLine && source!=rex::cvar::Source::kEnvironment)
    for (const auto& setting : plan.settings) if (setting.name==name) value=setting.value;
  return value=="true" || value=="1";
}
constexpr const char* cheats[]={"kk_cheat_healing","kk_cheat_one_hit","kk_cheat_ammo","kk_cheat_spears",
    "kk_cheat_revolver","kk_cheat_machine_gun","kk_cheat_shotgun","kk_cheat_sniper"};
bool Send(MessageType type, FlowEvent f) { Message m; m.type=type; m.flow=f; return SubmitFlowMessage(std::move(m)); }
void PublishLife(const FlowView& v, bool down, double now) {
  if (life_sent && down==sent_downed && now<next_life) return;
  if (life_number==UINT32_MAX) { Send(MessageType::FlowAbort,v.world); return; }
  Message m; m.type=down ? MessageType::PlayerDowned : MessageType::PlayerRevived;
  m.life={v.world,life_incarnation,life_number+1,local_checkpoint};
  if (SubmitFlowMessage(std::move(m))) {
#if defined(KK_DEV_TOOLS)
    if (!life_sent || down!=sent_downed)
      REXLOG_INFO("KK co-op revive: {} queued role={} incarnation={} number={} checkpoint={} initial={}",
          down ? "PlayerDowned" : "PlayerRevived",v.host ? "host" : "guest",
          life_incarnation,life_number+1,local_checkpoint,!life_sent);
#endif
    ++life_number; life_sent=true; sent_downed=down; next_life=now+.5;
  }
}
bool PartnerAliveHere(const FlowView& v, const EnemyLink& link, double now) {
  return link.connected && v.partner_life.Fresh(now) && !v.partner_life.downed &&
      link.partner && link.partner->level==v.world.level;
}
void RecordProgress(const FlowView& v) {
  if (v.host || v.completed==applied_progress) return;
  if (play::ApplyTogetherProgress(personal,progress_slot,v.completed)) applied_progress=v.completed;
  else { Send(MessageType::FlowAbort,v.world); REXLOG_ERROR("KK co-op flow: personal chapter unlock could not be recorded"); }
}
FlowView Wait(uint32_t generation, bool load_barrier) {
  const double until=Now()+125;
  double paint=0;
  bool waiting_logged=false;
  for (;;) {
    auto v=SessionFlowSnapshot();
    if (stopping || !v.active || v.phase==FlowPhase::Failed || v.world.generation!=generation ||
        (load_barrier && v.phase==FlowPhase::Playing)) {
      if (waiting_logged && v.world.generation!=generation)
        REXLOG_INFO("KK co-op flow: regroup released role={} generation {} -> {} level {}",
            v.host ? "host" : "guest",generation,v.world.generation,v.world.level);
      return v;
    }
    if (!waiting_logged && v.phase==FlowPhase::Waiting) {
      waiting_logged=true;
      REXLOG_INFO("KK co-op flow: chapter finished role={} generation {} level {}; waiting for partner to finish",
          v.host ? "host" : "guest",generation,v.world.level);
    }
    if (!load_barrier && v.phase==FlowPhase::Playing && !v.host &&
        v.policy==ChapterPolicy::Observer && !observer::HoldWorld()) return v;
    const bool chapter_wait=v.phase==FlowPhase::Waiting || (v.policy==ChapterPolicy::Observer && !v.host && v.phase==FlowPhase::Playing);
    if (Now()>=until && !chapter_wait) { Send(MessageType::FlowAbort,v.world); return v; }
    if (Now()>=paint) { RequestUiPaint(); paint=Now()+.1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
void AskLoad(uint8_t* base,uint32_t g,const FlowView& v) {
  using namespace hfr;
  Store32(base,g+8,v.world.level); Store32(base,g+12,v.world.start); Store32(base,g+4,1);
  Store32(base,0x828E3B48,1); Store32(base,g+0x5F1C,0);
  // Co-op chapter selection continues as a story, not the practice-mode menu route.
  Store32(base,g+0x5668,0);
  if (const int c=FlowChapter(v.world.level); c>=0) Store32(base,g+0x5670,uint32_t(c));
  Store32(base,0x82CCAA80,0); Store32(base,0x82CCBFD4,0);
  loading_generation=v.world.generation;
  move_offer=false;
  move_request=0; pending_move=0;
  behind=BehindRule{};
  recorded_checkpoint=v.world.start;
  REXLOG_INFO("KK co-op flow: load generation {} level {} start {}",v.world.generation,v.world.level,v.world.start);
}
}
int SessionCheats() { return session_cheats.load(); }
bool MoveToPartnerOffered() { return move_offer.load(); }
void MoveToPartner(bool checkpoint_recovery) {
  const auto v=SessionFlowSnapshot();
  if (move_offer.exchange(false) && !v.host && v.phase==FlowPhase::Playing && v.policy==ChapterPolicy::Together) {
    if (checkpoint_recovery && StoryBehind()) {
      if (Send(MessageType::ReloadRequest,v.world)) REXLOG_INFO("KK co-op story: checkpoint recovery accepted after catch-up offer");
    } else {
      // UI submits a generation only. Native memory/teleport stays on the AI thread.
      move_request=v.world.generation;
      REXLOG_INFO("KK co-op story: soft fix requested, move to partner without reload");
    }
  }
}
void StopSessionFlow() { stopping=true; move_request=0; observer::SetPartnerSpectating(false); }
bool SessionInProcess() { return launched; }
bool SessionPrivateReload() { return private_load; }
void ApplySessionSettings() {
  if (!launched) return;
  const auto v=SessionFlowSnapshot();
  SetModSetting("kk_cheats",(v.start.cheats & 0x100) ? "true" : "false");
  for (size_t i=0;i<std::size(cheats);++i) SetModSetting(cheats[i],(v.start.cheats & (1u<<i)) ? "true" : "false");
  // The port's only difficulty flag. Its health backend is still unwired in
  // hardcore.cpp; carrying it cannot claim a working no-regeneration mode.
  SetModSetting("kk_no_healing",v.start.difficulty ? "true" : "false");
}
std::filesystem::path SessionUserDir(const std::filesystem::path& root) { return private_root.empty() ? root : private_root; }
void SelectSessionStart(const play::StartRequest& r) { if (!requested) selected=r; }
bool RequestSessionStart() {
  if (!REXCVAR_GET(kk_coop_enabled) || requested) return false;
  auto& social=Social(); const auto model=social.Snapshot();
  if (!CanStartLobby(model)) return false;
  const auto root=play::UserDir();
  const auto slots=play::ListSaves(root);
  auto r=selected;
  if (r.kind!=play::StartKind::kChapter && r.kind!=play::StartKind::kContinue) return false;
  if (r.slot<0) r.slot=play::MostRecentSave(slots);
  if (r.slot>=play::kSlotCount) return false;
  SessionStart s; s.chapter_select=r.kind==play::StartKind::kChapter;
  auto save=play::ExportSessionSave(root,r.slot);
  if (s.chapter_select) {
    if (r.chapter<0 || r.chapter>=42) return false;
    s.chapter=uint16_t(r.chapter); s.level=kChapterRules[size_t(r.chapter)].level;
    s.start=r.chapter==0 || r.chapter==41 ? 0 : r.chapter==40 ? 49 : 1;
  } else {
    if (save.empty() || r.slot<0) return false;
    const auto& slot=slots[size_t(r.slot)]; const int chapter=FlowChapter(uint32_t(slot.continue_level));
    if (chapter<0) return false;
    s.chapter=uint16_t(chapter); s.level=uint32_t(slot.continue_level);
    s.start=slot.continue_start==-1 && s.level==100 ? 0 : uint32_t(slot.continue_start);
  }
  const auto all=mods::Scan(root);
  const auto plan=mods::BuildPlan(mods::EnabledInOrder(all,mods::ParseEnabled(REXCVAR_GET(kk_mods))));
  s.cheats=EffectiveFlag("kk_cheats",plan) ? 0x100 : 0;
  for (size_t i=0;i<std::size(cheats);++i) if (EffectiveFlag(cheats[i],plan)) s.cheats|=uint16_t(1u<<i);
  s.difficulty=EffectiveFlag("kk_no_healing",plan) ? 1 : 0;
  s.save_bytes=uint32_t(save.size()); s.save_hash=SessionSaveHash(save);
  if (!ValidSessionStart(s)) return false;
  // Steam starts the handshake here; dev link is already connected in the launcher.
  if (!social.Act({SocialAction::Start})) return false;
  if (!BeginSessionFlow(s,std::move(save))) return false;
  selected=r; requested=true; requested_lobby=model.lobby; return true;
}
bool TakeSessionLaunch(play::StartRequest& r) {
  if (launched || !REXCVAR_GET(kk_coop_enabled)) return false;
  const auto v=SessionFlowSnapshot();
  // An asynchronous Steam Start can fail or be left before either game launches.
  // Let the launcher select/start again after that lobby has ended.
  if (requested && Social().Snapshot().lobby!=requested_lobby) requested=false;
  if (!v.start_ready || v.phase==FlowPhase::Failed) return false;
  personal=play::UserDir(); progress_slot=play::MostRecentSave(play::ListSaves(personal));
  if (!v.host) {
    // The runtime mounts this root before SetupEnvironment. Every native profile
    // writer and the pause Settings use the same private root, not just save hooks.
    private_root=personal/"coop-sessions"/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::error_code ec; std::filesystem::create_directories(private_root,ec);
    if (ec || !play::ImportSessionSave(private_root,v.save)) {
      Send(MessageType::FlowAbort,v.world); return false;
    }
  }
  r=v.host ? selected : play::StartRequest{v.start.chapter_select ? play::StartKind::kChapter : play::StartKind::kContinue,0,int(v.start.chapter),{}};
  if (r.kind==play::StartKind::kChapter && r.slot<0) r.slot=0;
  play::SetUserDir(SessionUserDir(personal));
  session_cheats=v.start.cheats;
  launched=true;
  REXLOG_INFO("KK co-op flow: SessionStart accepted, chapter {} level {} start {} save bytes {}",v.start.chapter,v.start.level,v.start.start,v.save.size());
  return true;
}
bool SessionBeforeGo(PPCContext& ctx,uint8_t* base,uint32_t g) {
  using namespace hfr;
  if (!launched || !play::SessionActive() || !PoseAddress(g,0x6000)) return false;
  auto v=SessionFlowSnapshot();
  if (!v.active) return false;
  if (v.phase==FlowPhase::Failed) {
    RecordProgress(v);
    play::ReturnToLauncher("co-op session ended","play"); Store32(base,g+4,0); return false;
  }
  RecordProgress(v);
  if (v.world.generation>loaded_generation) {
    private_load=private_request=false;
    independent_load.reset();
    AskLoad(base,g,v); return false;
  }
  // A dev finish is submitted directly to the worker, without a native load
  // flag. Hold the first finisher just as for an authored chapter exit.
  if (v.phase==FlowPhase::Waiting) {
    v=Wait(v.world.generation,false);
    RecordProgress(v);
    if (v.world.generation>loaded_generation) { AskLoad(base,g,v); return false; }
    return true;
  }
  // An authored transition or the user's Quit request takes precedence over
  // recovery. Its normal flow handler below advances/ends the session.
  if (private_request && Load32(base,g+4)==0) {
    private_request=false;
    const auto link=EnemyLinkSnapshot();
    if (!PartnerAliveHere(v,link,Now())) {
      recovery.phase=revive::Phase::ReloadBoth;
      return true; // frame hook queues C24 reload; do not run a stale private request
    }
    // Deliberate fallback: native full load restores the authored player tracks,
    // health, control, camera and checkpoint inventory. No death-state writes.
    // The host's Checkpoint message is newer than an older heartbeat. If the
    // host is dead, use the living guest's latest observed checkpoint instead.
    local_checkpoint=v.host ? v.partner_life.life.checkpoint : v.checkpoint;
    Store32(base,g+8,v.world.level); Store32(base,g+12,local_checkpoint);
    Store32(base,g+4,1); Store32(base,g+0x5F1C,0); Store32(base,g+0x5668,0);
    Store32(base,0x828E3B48,1);
    Store32(base,0x82CCAA80,0); Store32(base,0x82CCBFD4,0);
    private_load=true; move_offer=false;
    REXLOG_INFO("KK co-op revive: private load generation {} level {} partner checkpoint {}",v.world.generation,v.world.level,local_checkpoint);
    return false; // bypass the shared LevelChange/Loaded barrier for this load
  }
  // Native re-init has restored this player's own checkpoint. Let uni_go
  // consume it locally, before observer holding or shared transition routing.
  if (DeathRouteFor(v.policy)==DeathRoute::LocalCheckpoint && Load32(base,g+4)!=0 &&
      Load32(base,g+8)==v.world.level) {
    independent_load=FlowEvent{v.world.generation,v.world.level,Load32(base,g+12),v.completed};
    REXLOG_INFO("KK co-op flow: local checkpoint load role={} policy={} generation {} level {} start {}; partner continues",
        v.host ? "host" : "guest",v.policy==ChapterPolicy::SoloRegroup ? "solo-regroup" : "observer",
        v.world.generation,v.world.level,Load32(base,g+12));
    return false;
  }
  if (!v.host && v.policy==ChapterPolicy::Observer && observer::HoldWorld()) {
    Store32(base,g+4,0);
    v=Wait(v.world.generation,false);
    RecordProgress(v);
    if (v.world.generation>loaded_generation) { AskLoad(base,g,v); return false; }
    return true;
  }
  if (!v.host && v.checkpoint!=recorded_checkpoint && v.policy==ChapterPolicy::Together) {
    recorded_checkpoint=v.checkpoint; Store32(base,g+12,v.checkpoint);
    local_checkpoint=v.checkpoint;
    const auto saved=ctx; __imp__sub_82413190(ctx,base); ctx=saved;
  }
  if (Load32(base,g+4)!=0) {
    uint32_t level=Load32(base,g+8),start=Load32(base,g+12);
    if (level==3 && play::LeftByQuit()) { Send(MessageType::FlowAbort,v.world); return false; }
    if (level==3) {
      const int next=FlowChapter(v.world.level)+1;
      if (next<=0 || next>=42) { Send(MessageType::FlowAbort,v.world); return false; }
      level=kChapterRules[size_t(next)].level; start=next==40 ? 49 : next==41 ? 0 : 1;
    }
    Store32(base,g+4,0);
    if (!v.host && v.policy==ChapterPolicy::Observer)
      observer::Fallback("Observer unavailable: local scripts requested a different load before the host.");
    if (v.host) Send(MessageType::LevelChange,{v.world.generation,level,start,v.completed});
    else if (v.policy==ChapterPolicy::SoloRegroup)
      Send(MessageType::ChapterFinished,v.world);
    v=Wait(v.world.generation,false);
    RecordProgress(v);
    if (v.world.generation>loaded_generation) AskLoad(base,g,v);
    return false;
  }
  // Offer recovery rather than silently resetting a living partner's world.
  if (!v.host && v.phase==FlowPhase::Playing && v.policy==ChapterPolicy::Together) {
    const auto link=EnemyLinkSnapshot();
    const auto local=ReadJackPose([base](uint32_t a){return Load32(base,a);},[base](uint32_t a){return Load16(base,a);},[base](uint32_t a){return LoadF(base,a);},Now());
    if (!link.connected) behind.Tick(Now(),{},{});
    else if (behind.Tick(Now(),local.pose,link.partner)) move_offer=true;
    if (StoryBehind() || StoryNeedsMove()) move_offer=true;
    if (!StoryBehind() && !StoryNeedsMove() && local.pose && link.partner && local.pose->level==link.partner->level &&
        PartnerDistance(*local.pose,*link.partner)<=100) move_offer=false;
  }
  return false;
}
void SessionAfterGo(uint8_t* base,uint32_t g,bool loaded) {
  if (launched && loaded && private_load) StoryLoaded(base);
  if (independent_load && loaded) {
    const auto own=*independent_load;
    independent_load.reset();
    const auto v=SessionFlowSnapshot();
    if (hfr::Load32(base,g+0x18)!=own.level) { Send(MessageType::FlowAbort,v.world); return; }
    local_checkpoint=own.start;
    observer::LevelLoaded();
    // An observer host may have advanced during this local synchronous load.
    // The next SessionBeforeGo follows that generation through the usual barrier.
    REXLOG_INFO("KK co-op flow: local checkpoint loaded generation {} level {} start {}; no shared barrier",
        own.generation,own.level,local_checkpoint);
    return;
  }
  if (private_load && loaded) {
    if (hfr::Load32(base,g+0x18)==SessionFlowSnapshot().world.level) recovery.Loaded();
    else recovery.phase=revive::Phase::ReloadBoth;
    return; // living partner keeps simulating; no Loaded or LoadRelease
  }
  if (!launched || !loaded || !loading_generation) return;
  auto v=SessionFlowSnapshot();
  if (v.world.generation!=loading_generation) return;
  if (hfr::Load32(base,g+0x18)!=v.world.level) { Send(MessageType::FlowAbort,v.world); return; }
  StoryLoaded(base);
  loaded_generation=loading_generation; loading_generation=0;
  observer::LevelLoaded();
  RecordProgress(v);
  Send(MessageType::Loaded,v.world);
  REXLOG_INFO("KK co-op flow: loaded generation {}, waiting for both",loaded_generation);
  // uni_go's loader is synchronous. Stay at this boundary, with the network
  // worker and UI running, before any next simulation step or AI pass.
  Wait(loaded_generation,true);
}
void SessionCheckpoint(uint32_t level,uint32_t checkpoint) {
  if (!launched) return;
  observer::Checkpoint(level,checkpoint);
  const auto v=SessionFlowSnapshot();
  if (v.phase==FlowPhase::Playing && v.world.level==level && checkpoint<=10000) local_checkpoint=checkpoint;
  if (v.host && v.phase==FlowPhase::Playing && v.world.level==level && checkpoint<=10000)
    Send(MessageType::Checkpoint,{v.world.generation,level,checkpoint,v.completed});
}
void SessionReloadFrame(uint8_t* base) {
  if (!REXCVAR_GET(kk_coop_enabled) || !launched || !base) {
    observer::SetPartnerSpectating(false); return;
  }
  const auto v=SessionFlowSnapshot();
  if (v.active && v.phase==FlowPhase::Failed) {
    // Disconnects can arrive while normal gameplay never calls uni_go. Use
    // the regular save-aware launcher return instead of waiting for a load.
    observer::SetPartnerSpectating(false);
    RecordProgress(v);
    play::ReturnToLauncher("co-op session ended","play");
    return;
  }
  if (!v.active || v.phase!=FlowPhase::Playing) {
    observer::SetPartnerSpectating(false); return;
  }
  observer::SetPartnerSpectating(false);
  if (recovery_generation!=v.world.generation) {
    recovery_generation=v.world.generation; recovery={}; private_load=private_request=reload_sent=false;
    local_checkpoint=v.checkpoint;
    ++life_incarnation; life_number=0; life_sent=false; next_life=0;
  }
  const double now=Now();
  const bool reset=hfr::Load32(base,0x82CCAA80)==1 && hfr::Load32(base,0x82CCBFD4)<=30;
  if (DeathRouteFor(v.policy)==DeathRoute::LocalCheckpoint) {
    // Leave the native reset intact: its re-init restores the LOCAL retry
    // snapshot, including checkpoint/script state, for Jack, Kong or vehicles.
    // SessionBeforeGo bypasses the shared barrier for the resulting local load.
    if (reset) REXLOG_INFO("KK co-op flow: local death/reset role={} policy={} generation {}; native own-checkpoint retry",
        v.host ? "host" : "guest",v.policy==ChapterPolicy::SoloRegroup ? "solo-regroup" : "observer",v.world.generation);
    return;
  }
  if (v.policy==ChapterPolicy::Together) {
    const auto condition=LocalPlayerCondition(base);
    const bool dead=condition && *condition==PlayerCondition::Dead;
    const auto link=EnemyLinkSnapshot();
    const auto action=recovery.Step(dead,PartnerAliveHere(v,link,now),v.partner_life.downed,reset,now);
    observer::SetPartnerSpectating(!v.host && recovery.Downed() && action!=revive::Action::ReloadBoth);
    if (condition || recovery.Downed()) PublishLife(v,dead || recovery.Downed(),now);
    if (action==revive::Action::LoadLocal) private_request=true;
    if (action==revive::Action::ReloadBoth) {
      hfr::Store32(base,0x82CCAA80,0); hfr::Store32(base,0x82CCBFD4,0);
      if (!reload_sent) {
        reload_sent=Send(MessageType::ReloadRequest,v.world);
        if (reload_sent) REXLOG_INFO("KK co-op revive: C24 reload, both dead or partner unavailable");
      }
      return;
    }
    if (recovery.Downed()) {
      // The original death track still runs/fades. Suppress every repeated
      // native reset until the private load or coordinated recovery owns it.
      hfr::Store32(base,0x82CCAA80,0); hfr::Store32(base,0x82CCBFD4,0);
      return;
    }
  }
  // sub_82857F28 calls the frame-time hook, then consumes this reset flag.
  // Intercept here, before its re-init, not later in uni_go after consumption.
  if (!reset) return;
  hfr::Store32(base,0x82CCAA80,0); hfr::Store32(base,0x82CCBFD4,0);
  Send(MessageType::ReloadRequest,v.world);
  REXLOG_INFO("KK co-op flow: death/reload fallback, both reload the host checkpoint");
}
#if defined(KK_DEV_TOOLS)
bool SessionDevChapterEnd() {
  const auto v=SessionFlowSnapshot();
  if (!launched || !v.active || v.phase!=FlowPhase::Playing || !play::SessionActive()) return false;
  const int next=FlowChapter(v.world.level)+1;
  if (next<=0 || next>=int(kChapterRules.size())) {
    REXLOG_WARN("KK dev jump: co-op chapter-end aid has no next chapter for level {}",v.world.level);
    return true;
  }
  const uint32_t level=kChapterRules[size_t(next)].level;
  const uint32_t start=next==40 ? 49 : next==41 ? 0 : 1;
  if (!v.host && v.policy!=ChapterPolicy::SoloRegroup) {
    REXLOG_INFO("KK dev jump: co-op chapter-end aid guest follows host, policy={} level {}",
        v.policy==ChapterPolicy::Observer ? "observer" : "together",v.world.level);
    return true;
  }
  const auto type=v.host ? MessageType::LevelChange : MessageType::ChapterFinished;
  if (!Send(type,v.host ? FlowEvent{v.world.generation,level,start,v.completed} : v.world)) return false;
  REXLOG_INFO("KK dev jump: co-op chapter-end aid queued {} role={} generation {} level {} -> entry {} level {}; solo waits for both",
      v.host ? "LevelChange" : "ChapterFinished",v.host ? "host" : "guest",v.world.generation,v.world.level,next,level);
  return true;
}
#endif
void SessionReviveBeforeAi(PPCContext& ctx,uint8_t* base) {
  const uint32_t requested_move=move_request.exchange(0);
  if (requested_move) { pending_move=requested_move; move_until=Now()+5; }
  if (pending_move) {
    const auto v=SessionFlowSnapshot(); const auto link=EnemyLinkSnapshot();
    if (!REXCVAR_GET(kk_coop_enabled) || stopping || !launched || !v.active || v.host ||
        v.phase!=FlowPhase::Playing || v.policy!=ChapterPolicy::Together || v.world.generation!=pending_move) {
      pending_move=0;
    } else if (!recovery.Downed() && !observer::Active() && PartnerAliveHere(v,link,Now()) &&
        ReviveTeleport(ctx,base,*link.partner)) {
      pending_move=0; move_offer=false; behind=BehindRule{};
      StoryPartnerMoved();
      REXLOG_INFO("KK co-op story: soft fix applied, native teleport beside host; no reload");
    } else if (Now()>move_until) {
      pending_move=0; move_offer=true;
      REXLOG_INFO("KK co-op story: soft fix deferred, safe live partner/Jack/controller unavailable; no reload");
    }
  }
  if (!REXCVAR_GET(kk_coop_enabled) || !launched || recovery.phase!=revive::Phase::Placing) return;
  const auto v=SessionFlowSnapshot(); const auto link=EnemyLinkSnapshot();
  if (!v.active || v.phase!=FlowPhase::Playing || v.world.generation!=recovery_generation ||
      !PartnerAliveHere(v,link,Now())) { recovery.phase=revive::Phase::ReloadBoth; return; }
  if (!ReviveTeleport(ctx,base,*link.partner)) return;
  recovery.Placed(); private_load=false;
  observer::SetPartnerSpectating(false);
  ++life_incarnation; life_number=0; life_sent=false;
  PublishLife(v,false,Now());
  REXLOG_INFO("KK co-op revive: native load and teleport complete, 1.5 behind partner at partner height");
}
}  // namespace kk::coop
