#include "coop_runtime.h"
#include "coop_pose.h"
#include "coop_enemies_game.h"
#include "coop_observer.h"
#include "coop_story_game.h"
#include "coop_desync_game.h"
#include "coop_dev_follow.h"
#include "benchmark.h"
#include "play_internal.h"
#include "hfr.h"
#include "settings.h"
#include "mods.h"
#include "ui_paint.h"
#include "menu_scene.h"
#include "coop_handover.h"
#include "coop_resume.h"
#include <random>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <atomic>
#include <bit>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <thread>

REX_EXTERN(__imp__sub_82413190);
REX_EXTERN(__imp__sub_8241A158);
namespace kk::coop {
bool BeginSessionFlow(SessionStart start, std::vector<uint8_t> save);
namespace {
std::filesystem::path personal, private_root;
play::StartRequest selected;
bool requested=false, launched=false;
std::atomic<bool> rejoin_cancel{false};
uint32_t loaded_rejoin_revision=0;
double next_rejoin_publish=0;
SocialId requested_lobby=0;
int progress_slot=-1;
std::atomic<int> session_cheats{-1};
std::atomic<bool> stopping{false};
std::atomic<bool> move_offer{false};
std::atomic<uint32_t> move_request{0};
std::atomic<bool> catchup_offer{false};
std::atomic<uint32_t> catchup_request{0};
std::atomic<double> catchup_notice_until{0};
CheckpointCatchup catchup;
enum class CatchupPhase { None, Requested, Loading, Placing };
CatchupPhase catchup_phase=CatchupPhase::None;
uint32_t catchup_target=0;
bool catchup_from_offer=false;
bool catchup_from_guard=false;
double catchup_deadline=0;
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
#if defined(KK_DEV_TOOLS)
DevFollowHost dev_follow;
DevFollowResult dev_follow_pad;
double dev_follow_at=-1, dev_follow_log=0;
uint32_t dev_follow_generation=0;
bool DevFollowEnabled() {
  static const bool enabled=[] {
    const char* value=std::getenv("KK_DEV_FOLLOW_HOST");
    return value && std::string_view(value)=="1";
  }();
  return enabled;
}
#endif
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
bool CatchupSafe(uint8_t* base,double now) {
  if (observer::Active() || observer::InCutscene(base) || play::StartLevelLoading() ||
      play::CoverScreen() || play::PauseMenuOpen() || play::GamePausePageUp()) return false;
  const auto local=ReadJackPose([base](uint32_t a){return hfr::Load32(base,a);},
      [base](uint32_t a){return hfr::Load16(base,a);},[base](uint32_t a){return hfr::LoadF(base,a);},now);
  return local.pose && !EnemyNearPlayer(base,*local.pose,15);
}
void CancelCatchup() {
  if (catchup_from_guard) WorldGuardCancelled(desync::Tier::Reload);
  catchup_from_guard=false;
  catchup_phase=CatchupPhase::None; catchup_offer=false; catchup_request=0;
  private_load=private_request=false; catchup_notice_until=0; catchup_from_offer=false;
}
void BeginCatchup(const FlowView& v,double now,const char* reason,bool checkpoint_offer=false,bool desync_recovery=false) {
  if (!catchup.Begin(checkpoint_offer,desync_recovery)) {
    if (desync_recovery) { WorldGuardStarted(desync::Tier::Reload); WorldGuardCancelled(desync::Tier::Reload); }
    return;
  }
  catchup_from_guard=desync_recovery;
  if (desync_recovery) WorldGuardStarted(desync::Tier::Reload);
  catchup_from_offer=checkpoint_offer;
  catchup_target=catchup.Target(); catchup_phase=CatchupPhase::Requested;
  catchup_deadline=now+revive::kLoadTimeout; private_request=true;
  catchup_offer=move_offer=false; move_request=0; pending_move=0;
  catchup_notice_until=now+6;
  REXLOG_INFO("KK co-op catch-up: private reload queued reason={} generation {} level {} guest checkpoint {} host checkpoint {}; host continues",
      reason,v.world.generation,v.world.level,catchup.WorldCheckpoint(),catchup_target);
}
void RecordProgress(const FlowView& v) {
  if (v.host || v.completed==applied_progress) return;
  if (play::ApplyTogetherProgress(personal,progress_slot,v.completed)) applied_progress=v.completed;
  else { Send(MessageType::FlowAbort,v.world); REXLOG_ERROR("KK co-op flow: personal chapter unlock could not be recorded"); }
}
FlowView Wait(uint32_t generation, bool load_barrier, bool host_transition=false) {
  // Use the shared load budget for an early observer script exit too. Steam
  // relay queueing and host frame stalls can exceed the old two second grace.
  const double began=Now(),until=began+125;
  double next_wait_log=began+5;
  double paint=0;
  bool waiting_logged=false;
  for (;;) {
    auto v=SessionFlowSnapshot();
    if (Now()>=next_wait_log) {
      next_wait_log=Now()+5;
      REXLOG_INFO("KK co-op flow wait: role={} generation={} level={} phase={} save={}/{} ready={} barrier={} host-transition={} elapsed={:.1f}s",
          v.host ? "host":"guest",v.world.generation,v.world.level,int(v.phase),v.save.size(),v.start.save_bytes,
          v.start_ready,load_barrier,host_transition,Now()-began);
    }
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
    if (!host_transition && !load_barrier && v.phase==FlowPhase::Playing && !v.host &&
        v.policy==ChapterPolicy::Observer && !observer::HoldWorld()) return v;
    if (host_transition && Now()>=until) {
      observer::Fallback("Observer unavailable: the host's next load did not arrive.");
      return v;
    }
    const bool chapter_wait=v.phase==FlowPhase::Waiting || (v.policy==ChapterPolicy::Observer && !v.host && v.phase==FlowPhase::Playing);
    if (Now()>=until && !chapter_wait) { Send(MessageType::FlowAbort,v.world); return v; }
    if (Now()>=paint) { RequestUiPaint(); paint=Now()+.1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
void AskLoad(PPCContext& ctx,uint8_t* base,uint32_t g,const FlowView& v) {
  using namespace hfr;
  // ES_enter_map grants the chapter selection's authored equipment only while
  // G+22120 is set. We clear that practice-mode flag below so the campaign can
  // continue. Perform its exact inventory initializer once for the initial
  // chapter selection, before loading, rather than losing the grant entirely.
  // Story transitions and private checkpoint revives retain their saved stock.
  if (!v.start.rejoin && !loaded_generation && !loading_generation && v.start.chapter_select && v.start.chapter<42) {
    const auto saved=ctx;
    const auto bank=Load32(base,0x82CC9C98);
    Store32(base,0x82CC9C98,g);
    ctx.r3.u64=v.start.chapter;
    __imp__sub_8241A158(ctx,base);
    Store32(base,0x82CC9C98,bank);
    ctx=saved;
    REXLOG_INFO("KK co-op equipment: authored chapter={} weapon={} ammo={}/{}/{}/{} reserve={}/{}/{}/{}",
        v.start.chapter,Load32(base,g+13128),Load32(base,g+13432),Load32(base,g+13504),
        Load32(base,g+13576),Load32(base,g+13648),Load32(base,g+14880),Load32(base,g+14952),
        Load32(base,g+15024),Load32(base,g+15096));
  }
  const bool rejoin=v.start.rejoin && v.start.generation==v.world.generation;
  const auto start=rejoin ? v.start.start : v.world.start;
  if (rejoin) {
    Store32(base,g+13128,v.start.weapon);
    for (unsigned i=0;i<4;++i) {
      Store32(base,g+13432+i*72,v.start.stock[i]);
      Store32(base,g+14880+i*72,v.start.stock[i+4]);
    }
    REXLOG_INFO("KK co-op rejoin: equipment restored weapon={} magazines={}/{}/{}/{}",v.start.weapon,
        v.start.stock[0],v.start.stock[1],v.start.stock[2],v.start.stock[3]);
  }
  Store32(base,g+8,v.world.level); Store32(base,g+12,start); Store32(base,g+4,1);
  Store32(base,0x828E3B48,1); Store32(base,g+0x5F1C,0);
  // Co-op chapter selection continues as a story, not the practice-mode menu route.
  Store32(base,g+0x5668,0);
  if (const int c=FlowChapter(v.world.level); c>=0) Store32(base,g+0x5670,uint32_t(c));
  Store32(base,0x82CCAA80,0); Store32(base,0x82CCBFD4,0);
  loading_generation=v.world.generation;
  move_offer=false;
  move_request=0; pending_move=0;
  CancelCatchup(); catchup_notice_until=0;
  catchup.Reset(v.world.generation,start);
  behind=BehindRule{};
  recorded_checkpoint=v.world.start;
  REXLOG_INFO("KK co-op flow: load generation {} level {} start {}",v.world.generation,v.world.level,v.world.start);
}
}
int SessionCheats() { return session_cheats.load(); }
bool MoveToPartnerOffered() { return move_offer.load() || catchup_offer.load(); }
bool PrivateCatchupOffered() { return catchup_offer.load(); }
bool CatchupNotice() { return Now()<catchup_notice_until.load(); }
void MoveToPartner(bool checkpoint_recovery) {
  const auto v=SessionFlowSnapshot();
  if (checkpoint_recovery) {
    if (catchup_offer.exchange(false) && !v.host && v.phase==FlowPhase::Playing && v.policy==ChapterPolicy::Together)
      catchup_request=v.world.generation;
    return;
  }
  if (move_offer.exchange(false) && !v.host && v.phase==FlowPhase::Playing && v.policy==ChapterPolicy::Together) {
    // UI submits a generation only. Native memory/teleport stays on the AI thread.
    move_request=v.world.generation;
    REXLOG_INFO("KK co-op story: soft fix requested, move to partner without reload");
  }
}
void StopSessionFlow() { WorldGuardSummary(); stopping=true; move_request=catchup_request=0; catchup_offer=false; catchup_notice_until=0; observer::SetPartnerSpectating(false); }
bool SessionInProcess() { return launched; }
bool SessionPrivateReload() { return private_load; }
bool SessionCheckpointCatchup() { return catchup_phase==CatchupPhase::Loading || catchup_phase==CatchupPhase::Placing; }
uint32_t SessionNativeCheckpoint() { return catchup.WorldCheckpoint(); }
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
bool SelectSessionStart(const play::StartRequest& r) {
  if (requested || (r.kind!=play::StartKind::kContinue && r.kind!=play::StartKind::kChapter)) return false;
  const auto model=Social().Snapshot();
  if (!model.lobby || model.owner!=model.self || model.start_requested) return false;
  auto request=r; int chapter=r.chapter;
  if (r.kind==play::StartKind::kContinue) {
    const auto slots=play::ListSaves(play::UserDir());
    if (request.slot<0) request.slot=play::MostRecentSave(slots);
    if (request.slot<0 || request.slot>=int(slots.size()) || !slots[size_t(request.slot)].exists) return false;
    chapter=FlowChapter(uint32_t(slots[size_t(request.slot)].continue_level));
  }
  if (chapter<0 || chapter>=42) return false;
  if (!Social().Act({SocialAction::SelectChapter,0,false,{0,chapter,r.kind==play::StartKind::kContinue}})) return false;
  selected=request;
  REXLOG_INFO("KK co-op lobby: host selected {} chapter {}",r.kind==play::StartKind::kContinue ? "Continue" : "chapter",chapter);
  return true;
}
namespace {
// The SessionStart (and the host's save) for a chapter or Continue request. Shared by the launcher's Start
// and the resumed host after the Play restart (C80).
bool BuildSessionStart(play::StartRequest& r, SessionStart& s, std::vector<uint8_t>& save) {
  const auto root=play::UserDir();
  const auto slots=play::ListSaves(root);
  if (r.kind!=play::StartKind::kChapter && r.kind!=play::StartKind::kContinue) return false;
  if (r.slot<0) r.slot=play::MostRecentSave(slots);
  if (r.slot>=play::kSlotCount) return false;
  s=SessionStart{}; s.chapter_select=r.kind==play::StartKind::kChapter;
  save=play::ExportSessionSave(root,r.slot);
  if (s.chapter_select) {
    if (r.chapter<0 || r.chapter>=42) return false;
    s.chapter=uint16_t(r.chapter); s.level=kChapterRules[size_t(r.chapter)].level;
    s.start=r.chapter==0 || r.chapter==41 ? 0 : r.chapter==40 ? 49 : 1;
#if defined(KK_DEV_TOOLS)
    // The normal chapter fixture supports alternate native checkpoints. Carry
    // the host's explicit dev override in SessionStart so both worlds agree.
    if (const char* value=std::getenv("KK_DEV_START_POINT"); value && *value) {
      char* end=nullptr;
      const auto point=std::strtoul(value,&end,10);
      if (end!=value && !*end && point<=10000) {
        s.start=uint32_t(point);
        REXLOG_INFO("KK dev co-op: shared chapter start override entry={} level={} start={}",s.chapter,s.level,s.start);
      } else REXLOG_WARN("KK dev co-op: ignored invalid KK_DEV_START_POINT");
    }
#endif
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
  return ValidSessionStart(s);
}
// C80 hand over state of the old (launcher) process.
std::string handover_name;
bool handover_taken=false, handover_failed_logged=false;
long long UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace
bool RequestSessionStart() {
  if (!REXCVAR_GET(kk_coop_enabled) || requested) return false;
  auto& social=Social(); const auto model=social.Snapshot();
  if (!CanStartLobby(model)) return false;
  if (!model.choice.valid()) return false;
  auto r=selected;
  SessionStart s; std::vector<uint8_t> save;
  if (!BuildSessionStart(r,s,save)) return false;
  if (model.choice.chapter!=s.chapter || model.choice.continue_game==s.chapter_select) return false;
  REXLOG_INFO("KK co-op lobby: Start uses shown choice revision {} chapter {} level {}",model.choice.revision,s.chapter,s.level);
  // Steam starts the handshake here; dev link is already connected in the launcher.
  if (!social.Act({SocialAction::Start,0,false,model.choice})) return false;
  {
    // C80: the live menu scene is the game running in this process, so Play restarts. Agree on a plan with
    // the guest; both restart and reconnect, and the resumed host sends this SessionStart then.
    // C81: always, also when this host has the scene off. The guest may have it on, and a launcher with the
    // scene cannot start a chapter in its own process; with the plan both restart and every mix works.
    StartPlan plan;
    std::random_device random;
    plan.token=(uint64_t(random())<<32)^random(); if (!plan.token) plan.token=1;
    plan.chapter=uint8_t(s.chapter); plan.continue_game=s.chapter_select ? 0 : 1;
    const auto link=CurrentLink();
    if (link.direct) { plan.transport=StartTransport::Direct; plan.port=link.port; }
    else {
      plan.transport=StartTransport::Steam; plan.host_id=model.self;
      for (const auto& m : model.members) if (m.id!=model.self) plan.guest_id=m.id;
    }
    if (!BeginStartPlan(plan)) {
      // The local dev link has no hand over; without the scene it still starts in this process.
      if (!menu_scene::Active() && BeginSessionFlow(s,std::move(save))) {
        selected=r; requested=true; requested_lobby=model.lobby; return true;
      }
      REXLOG_WARN("KK co-op hand over: the start plan could not be queued"); return false;
    }
    REXLOG_INFO("KK co-op hand over: host start plan queued ({}, chapter {}, {}); waiting for the guest's answer",
        plan.transport==StartTransport::Steam ? "Steam" : "Direct IP",plan.chapter,plan.continue_game ? "Continue" : "chapter select");
    selected=r; requested=true; requested_lobby=model.lobby; return true;
  }
}
bool HandoverPending() { return handover_taken; }
bool PrepareMenuRejoin(const std::filesystem::path& root) {
  handover::File record;
  if (handover_taken || !ReadRejoinRecord(root,record)) return false;
  handover_name="coop-rejoin.txt"; handover_taken=true;
  REXLOG_INFO("KK co-op rejoin: menu selected the previous campaign");
  return true;
}
std::wstring HandoverArg() {
  return handover_taken ? L"--kk_coop_resume=" + std::wstring(handover_name.begin(),handover_name.end()) : std::wstring();
}
bool TakeHandover() {
  if (handover_taken || !REXCVAR_GET(kk_coop_enabled)) return false;
  StartPlan plan; const char* failure="";
  const auto phase=StartPlanState(&plan,&failure);
  if (phase==handover::PlanPhase::Failed) {
    if (!handover_failed_logged) { handover_failed_logged=true; REXLOG_WARN("KK co-op hand over: start plan failed: {}",failure); }
    requested=false;
    return false;
  }
  handover_failed_logged=false;
  if (phase!=handover::PlanPhase::Ready) return false;
  const auto link=CurrentLink();
  handover::File f;
  f.host=link.role==Role::Host;
  f.plan=plan;
  f.slot=f.host ? selected.slot : -1;
  f.partner_name=SanitizeDisplayName(link.partner_name.empty() ? (f.host ? "Guest" : "Host") : link.partner_name);
  f.address=link.address; f.code=link.code;
  f.version=KK_VERSION; f.written_ms=UnixMs();
  handover_name=handover::FileName(f);
  const auto path=play::UserDir()/handover_name;
  {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out<<handover::Format(f);
    if (!out) { REXLOG_ERROR("KK co-op hand over: could not write the hand over file"); requested=false; return false; }
  }
  handover_taken=true;
  REXLOG_INFO("KK co-op hand over: {} wrote {}; restarting into the game to reconnect",f.host ? "host" : "guest",handover_name);
  return true;
}
bool BeginResumedStart(const play::StartRequest& request) {
  auto r=request;
  SessionStart s; std::vector<uint8_t> save;
  if (!BuildSessionStart(r,s,save)) { REXLOG_WARN("KK co-op hand over: the resumed start could not be built"); return false; }
  SessionStart recorded;
  if (RejoinCheckpoint(recorded)) {
    recorded.save_bytes=s.save_bytes; recorded.save_hash=s.save_hash;
    s=recorded; r={play::StartKind::kChapter,request.slot,int(s.chapter),{}};
    REXLOG_INFO("KK co-op rejoin: restarted host restores generation={} level={} checkpoint={}",s.generation,s.level,s.start);
  }
  if (!BeginSessionFlow(s,std::move(save))) return false;
  selected=r; requested=true; requested_lobby=Social().Snapshot().lobby;
  REXLOG_INFO("KK co-op hand over: resumed host sent SessionStart chapter {} level {} start {}",s.chapter,s.level,s.start);
  return true;
}
bool TakeSessionLaunch(play::StartRequest& r) {
  if (launched || !REXCVAR_GET(kk_coop_enabled)) return false;
  const auto v=SessionFlowSnapshot();
  // An asynchronous Steam Start can fail or be left before either game launches.
  // Let the launcher select/start again after that lobby has ended.
  if (requested && Social().Snapshot().lobby!=requested_lobby) requested=false;
  if (!v.start_ready || v.phase==FlowPhase::Failed) return false;
  if (menu_scene::Active()) {
    // The live menu scene holds this process's game: an in-process start (a host without the scene) cannot
    // run here. C80 keeps that case out: the session stops with a message instead of a broken start.
    Send(MessageType::FlowAbort,v.world);
    REXLOG_WARN("KK co-op flow: SessionStart in a process with the live menu scene; the host must start with the hand over");
    return false;
  }
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
  if ((!v.host && v.rejoin_revision!=loaded_rejoin_revision && v.start_ready) || v.world.generation>loaded_generation) {
    loaded_rejoin_revision=v.rejoin_revision;
    private_load=private_request=false;
    independent_load.reset();
    AskLoad(ctx,base,g,v); return false;
  }
  // A dev finish is submitted directly to the worker, without a native load
  // flag. Hold the first finisher just as for an authored chapter exit.
  if (v.phase==FlowPhase::Waiting) {
    v=Wait(v.world.generation,false);
    RecordProgress(v);
    if (v.world.generation>loaded_generation) { AskLoad(ctx,base,g,v); return false; }
    return true;
  }
  // An authored transition or the user's Quit request takes precedence over
  // recovery. Its normal flow handler below advances/ends the session.
  if (private_request && Load32(base,g+4)==0) {
    private_request=false;
    const auto link=EnemyLinkSnapshot();
    if (catchup_phase==CatchupPhase::Requested) {
      const auto condition=LocalPlayerCondition(base);
      if ((condition && *condition==PlayerCondition::Dead) || (!catchup_from_guard && v.checkpoint<catchup.WorldCheckpoint()) ||
          (!catchup_from_offer && !catchup_from_guard && v.checkpoint==catchup.WorldCheckpoint())) {
        CancelCatchup(); return false; // The normal death route wins this race.
      }
    }
    if (!PartnerAliveHere(v,link,Now())) {
      if (catchup_phase!=CatchupPhase::None) {
        // A stale pose or a host private revive can recover. Keep the bounded
        // local request pending; its frame deadline never reloads the host.
        private_request=true;
        return false;
      }
      recovery.phase=revive::Phase::ReloadBoth;
      return true; // frame hook queues C24 reload; do not run a stale private request
    }
    // Deliberate fallback: native full load restores the authored player tracks,
    // health, control, camera and checkpoint inventory. No death-state writes.
    // The host's Checkpoint message is newer than an older heartbeat. If the
    // host is dead, use the living guest's latest observed checkpoint instead.
    local_checkpoint=v.host ? v.partner_life.life.checkpoint : v.checkpoint;
    if (catchup_phase==CatchupPhase::Requested) {
      catchup.Observe(v.world.generation,v.checkpoint,Now());
      catchup.Begin(catchup_from_offer,catchup_from_guard); // Also consume a checkpoint that advanced after queuing.
      catchup_target=local_checkpoint; catchup_phase=CatchupPhase::Loading;
      REXLOG_INFO("KK co-op catch-up: loading guest only generation {} level {} checkpoint {}",v.world.generation,v.world.level,catchup_target);
    }
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
    if (v.world.generation>loaded_generation) { AskLoad(ctx,base,g,v); return false; }
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
    REXLOG_INFO("KK co-op native load request: role={} from={}/{} to={}/{} condition={} reset={}/{} weapon={} ammo={}/{}/{}/{}",
        v.host ? "host":"guest",v.world.level,local_checkpoint,level,start,
        int(LocalPlayerCondition(base).value_or(PlayerCondition::Dead)),Load32(base,0x82CCAA80),Load32(base,0x82CCBFD4),
        Load32(base,g+13128),Load32(base,g+13432),Load32(base,g+13504),Load32(base,g+13576),Load32(base,g+13648));
    REXLOG_INFO("KK co-op native load human status: role={} slots2-7={}/{}/{}/{}/{}/{}",
        v.host ? "host":"guest",LoadF(base,g+7100),LoadF(base,g+7104),LoadF(base,g+7108),
        LoadF(base,g+7112),LoadF(base,g+7116),LoadF(base,g+7120));
    if (level==3) {
      const int next=FlowChapter(v.world.level)+1;
      if (next<=0 || next>=42) { Send(MessageType::FlowAbort,v.world); return false; }
      level=kChapterRules[size_t(next)].level; start=next==40 ? 49 : next==41 ? 0 : 1;
    }
    Store32(base,g+4,0);
    // An authored guest exit can precede the host's reliable LevelChange by
    // a few frames. Hold at this boundary before declaring a divergence.
    const bool host_transition=!v.host && v.policy==ChapterPolicy::Observer;
    if (host_transition)
      REXLOG_INFO("KK co-op observer: local transition to level {} start {} waits for host generation {}",
          level,start,v.world.generation);
    if (v.host) Send(MessageType::LevelChange,{v.world.generation,level,start,v.completed});
    else if (v.policy==ChapterPolicy::SoloRegroup)
      Send(MessageType::ChapterFinished,v.world);
    v=Wait(v.world.generation,false,host_transition);
    RecordProgress(v);
    if (v.world.generation>loaded_generation) AskLoad(ctx,base,g,v);
    return false;
  }
  // Offer recovery rather than silently resetting a living partner's world.
  if (!v.host && v.phase==FlowPhase::Playing && v.policy==ChapterPolicy::Together) {
    const auto link=EnemyLinkSnapshot();
    const auto local=ReadJackPose([base](uint32_t a){return Load32(base,a);},[base](uint32_t a){return Load16(base,a);},[base](uint32_t a){return LoadF(base,a);},Now());
    if (!link.connected) behind.Tick(Now(),{},{});
    else if (behind.Tick(Now(),local.pose,link.partner)) move_offer=true;
    if (StoryNeedsMove()) move_offer=true;
    if (!StoryNeedsMove() && local.pose && link.partner && local.pose->level==link.partner->level &&
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
#if defined(KK_DEV_TOOLS)
    benchmark::DevCoopLoaded(hfr::Load32(base,g+0x18));
#endif
    if (catchup_phase==CatchupPhase::Loading) {
      if (hfr::Load32(base,g+0x18)!=SessionFlowSnapshot().world.level) {
        CancelCatchup(); Send(MessageType::FlowAbort,SessionFlowSnapshot().world);
        REXLOG_WARN("KK co-op catch-up: private load failed level validation; session stopped, host not reloaded");
      } else {
        catchup.NativeCheckpoint(catchup_target); catchup_phase=CatchupPhase::Placing;
        observer::LevelLoaded();
      }
    } else if (hfr::Load32(base,g+0x18)==SessionFlowSnapshot().world.level) {
      catchup.NativeCheckpoint(local_checkpoint); recovery.Loaded();
    } else recovery.phase=revive::Phase::ReloadBoth;
    return; // living partner keeps simulating; no Loaded or LoadRelease
  }
  if (!launched || !loaded || !loading_generation) return;
  auto v=SessionFlowSnapshot();
  if (v.world.generation!=loading_generation) return;
  if (hfr::Load32(base,g+0x18)!=v.world.level) { Send(MessageType::FlowAbort,v.world); return; }
#if defined(KK_DEV_TOOLS)
  benchmark::DevCoopLoaded(v.world.level);
#endif
  StoryLoaded(base);
  loaded_generation=loading_generation; loading_generation=0;
  observer::LevelLoaded();
  RecordProgress(v);
  Send(MessageType::Loaded,v.world);
  REXLOG_INFO("KK co-op flow: loaded generation {}, waiting for both",loaded_generation);
  // uni_go's loader is synchronous. Stay at this boundary, with the network
  // worker and UI running, before any next simulation step or AI pass.
  const auto released=Wait(loaded_generation,true);
  if (released.phase==FlowPhase::Playing && released.world.generation==loaded_generation)
    REXLOG_INFO("KK co-op flow: load barrier released role={} generation {} level {} policy={}",
        released.host ? "host" : "guest",loaded_generation,released.world.level,
        released.policy==ChapterPolicy::Together ? "together" :
        released.policy==ChapterPolicy::SoloRegroup ? "solo-regroup" : "observer");
}
void SessionCheckpoint(uint32_t level,uint32_t checkpoint) {
  if (!launched) return;
  observer::Checkpoint(level,checkpoint);
  const auto v=SessionFlowSnapshot();
  if (v.phase==FlowPhase::Playing && v.world.level==level && checkpoint<=10000) {
    local_checkpoint=checkpoint;
    if (catchup.WorldCheckpoint()!=checkpoint)
      REXLOG_INFO("KK co-op catch-up: native checkpoint role={} generation {} level {} checkpoint {} -> {}",
          v.host ? "host" : "guest",v.world.generation,level,catchup.WorldCheckpoint(),checkpoint);
    catchup.NativeCheckpoint(checkpoint);
  }
  if (v.host && v.phase==FlowPhase::Playing && v.world.level==level && checkpoint<=10000) {
    Send(MessageType::Checkpoint,{v.world.generation,level,checkpoint,v.completed});
    if (v.checkpoint!=checkpoint) StoryHostCheckpoint();
  }
}
void SessionReloadFrame(uint8_t* base) {
#if defined(KK_DEV_TOOLS)
  dev_follow_at=-1;
  struct ResetInactiveFollow {
    ~ResetInactiveFollow() { if (dev_follow_at<0) dev_follow={}; }
  } reset_inactive_follow;
#endif
  if (!REXCVAR_GET(kk_coop_enabled) || !launched || !base) {
    observer::SetPartnerSpectating(false); return;
  }
  const auto v=SessionFlowSnapshot();
  if (rejoin_cancel.exchange(false)) {
    stopping=true;
    play::ReturnToLauncher("co-op reconnect cancelled","play"); return;
  }
  if (v.active && !v.host && (v.reconnecting || v.reconnect_expired)) {
    REXLOG_INFO("KK co-op rejoin: guest waits for host; native simulation held");
    while (!stopping) {
      const auto wait=SessionFlowSnapshot();
      if (!wait.reconnecting && !wait.reconnect_expired && wait.start_ready && wait.phase!=FlowPhase::Failed) break;
      if (rejoin_cancel.exchange(false)) {
        stopping=true; play::ReturnToLauncher("co-op reconnect cancelled","play"); return;
      }
      RequestUiPaint(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REXLOG_INFO("KK co-op rejoin: host reachable; preparing guest checkpoint load");
  }
  const auto current=SessionFlowSnapshot();
  if (current.active && !current.host && current.start_ready && current.rejoin_revision!=loaded_rejoin_revision) {
    const auto g=hfr::Load32(base,0x82CC97C0);
    if (PoseAddress(g,0x6000)) hfr::Store32(base,g+4,1);
    return;
  }
  if (v.active && v.phase==FlowPhase::Failed && !v.reconnect_expired) {
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
  if (!v.reconnect_expired && Now()>=next_rejoin_publish) {
    next_rejoin_publish=Now()+.5;
    auto checkpoint=v.start;
    checkpoint.rejoin=true; checkpoint.level=v.world.level;
    checkpoint.chapter=uint16_t(FlowChapter(v.world.level)); checkpoint.start=v.checkpoint;
    checkpoint.generation=v.world.generation; checkpoint.world_start=v.world.start; checkpoint.completed=v.completed;
    const auto identity=EnemyCurrentWorld(); checkpoint.visit=identity.serial ? identity.serial : 1;
    const auto g=hfr::Load32(base,0x82CC97C0);
    if (PoseAddress(g,0x6000)) {
      checkpoint.weapon=hfr::Load32(base,g+13128);
      for (unsigned i=0;i<4;++i) {
        checkpoint.stock[i]=hfr::Load32(base,g+13432+i*72);
        checkpoint.stock[i+4]=hfr::Load32(base,g+14880+i*72);
      }
      PublishRejoinState(checkpoint); UpdateRejoinCheckpoint(checkpoint);
    }
  }
#if defined(KK_DEV_TOOLS)
  // Background sweep equivalent of pressing each existing catch-up button once.
  // Never creates an offer or escalates a move into a checkpoint reload.
  static const bool dev_catchup=[] {
    const char* value=std::getenv("KK_DEV_CATCHUP");
    return value && (std::string_view(value)=="1" || std::string_view(value)=="move");
  }();
  // "move" exercises the automatic post-soft-fix path without accepting an
  // early private checkpoint offer. "1" still accepts either visible button.
  static const bool dev_move_only=[] {
    const char* value=std::getenv("KK_DEV_CATCHUP");
    return value && std::string_view(value)=="move";
  }();
  static bool dev_moved=false, dev_recovered=false;
  if (dev_catchup && !v.host && v.policy==ChapterPolicy::Together && MoveToPartnerOffered()) {
    const bool checkpoint=PrivateCatchupOffered();
    bool& accepted=checkpoint ? dev_recovered : dev_moved;
    if (!accepted && !(checkpoint && dev_move_only)) {
      accepted=true;
      REXLOG_INFO("KK dev catch-up: accepting existing {} offer once",checkpoint ? "private checkpoint catch-up" : "move");
      MoveToPartner(checkpoint);
    }
  }
#endif
  observer::SetPartnerSpectating(false);
  if (recovery_generation!=v.world.generation) {
    recovery_generation=v.world.generation; recovery={}; private_load=private_request=reload_sent=false;
    CancelCatchup(); catchup_notice_until=0; catchup.Reset(v.world.generation,v.world.start);
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
    catchup.Observe(v.world.generation,v.checkpoint,now);
    if (catchup_phase!=CatchupPhase::None) {
      if (dead) CancelCatchup(); // Actual death still owns the both-dead path.
      else {
        hfr::Store32(base,0x82CCAA80,0); hfr::Store32(base,0x82CCBFD4,0);
        if (now>catchup_deadline) {
          CancelCatchup(); Send(MessageType::FlowAbort,v.world);
          REXLOG_WARN("KK co-op catch-up: placement timed out; session stopped, host not reloaded");
        }
        if (condition) PublishLife(v,false,now);
        return;
      }
    }
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
      catchup_offer=false; catchup_request=0;
      // The original death track still runs/fades. Suppress every repeated
      // native reset until the private load or coordinated recovery owns it.
      hfr::Store32(base,0x82CCAA80,0); hfr::Store32(base,0x82CCBFD4,0);
      return;
    }
    const bool eligible=!v.host && condition && !dead && PartnerAliveHere(v,link,now) &&
        !reset && !private_load && !private_request && !pending_move && !move_request.load() &&
        !observer::Active() && !observer::InCutscene(base) && !play::StartLevelLoading() &&
        !play::CoverScreen() && !play::PauseMenuOpen() && !play::GamePausePageUp();
    const uint32_t accepted=catchup_request.exchange(0);
    const auto guard_action=WorldGuardPending();
    if (eligible && guard_action==desync::Tier::Reload) {
      BeginCatchup(v,now,"persistent world digest drift",false,true);
      return;
    }
    if (eligible && guard_action==desync::Tier::Move) {
      move_request=v.world.generation; catchup_notice_until=now+4;
      WorldGuardStarted(desync::Tier::Move);
      REXLOG_INFO("KK co-op desync: automatic guest move queued generation={} level={}",v.world.generation,v.world.level);
      return;
    }
    const bool immediate=eligible &&
        (catchup.Offer(StoryAnyBehind(),true,now) || (catchup.Available() && StoryBehind())) && CatchupSafe(base,now);
    if (immediate && !catchup_offer.load())
      REXLOG_INFO("KK co-op catch-up: checkpoint offer generation {} guest checkpoint {} host checkpoint {}; no nearby enemy or cutscene",
          v.world.generation,catchup.WorldCheckpoint(),catchup.Target());
    catchup_offer=immediate;
    const bool automatic=eligible && catchup.Available() && StoryCatchupReady();
    if (automatic || (accepted==v.world.generation && immediate)) {
      BeginCatchup(v,now,automatic ? "persistent lag after soft move" : "checkpoint offer accepted",!automatic);
      return;
    }
  }
#if defined(KK_DEV_TOOLS)
  if (DevFollowEnabled()) {
    if (dev_follow_generation!=v.world.generation) {
      dev_follow={}; dev_follow_generation=v.world.generation;
    }
    const auto link=EnemyLinkSnapshot();
    const bool allowed=!v.host && v.policy==ChapterPolicy::Together && link.connected &&
        !private_load && !private_request && !observer::Active() && !play::StartLevelLoading() &&
        !play::CoverScreen() && !play::PauseMenuOpen() && !play::GamePausePageUp();
    const auto local=allowed ? ReadJackPose([base](uint32_t a){return hfr::Load32(base,a);},
        [base](uint32_t a){return hfr::Load16(base,a);},[base](uint32_t a){return hfr::LoadF(base,a);},now) : PoseRead{};
    dev_follow_pad=dev_follow.Tick(now,local.pose,allowed ? link.partner : std::optional<PlayerPose>{});
    if (local.pose && link.partner) {
      dev_follow_at=now;
      if (now>=dev_follow_log) {
        dev_follow_log=now+5;
        REXLOG_INFO("KK dev follow: generation {} level {}/{} distance {:.2f} LS {},{} local {:.2f},{:.2f},{:.2f} host {:.2f},{:.2f},{:.2f}",
            v.world.generation,local.pose->level,link.partner->level,dev_follow_pad.distance,
            dev_follow_pad.x,dev_follow_pad.y,local.pose->x,local.pose->y,local.pose->z,
            link.partner->x,link.partner->y,link.partner->z);
      }
      if (dev_follow_pad.move && !pending_move && !move_request.load()) {
        move_offer=true;
        REXLOG_INFO("KK dev follow: far for over 10 s, accepting existing Move to partner path, distance {:.2f}",dev_follow_pad.distance);
        MoveToPartner(false);
      }
    }
  }
#endif
  // sub_82857F28 calls the frame-time hook, then consumes this reset flag.
  // Intercept here, before its re-init, not later in uni_go after consumption.
  if (!reset) return;
  hfr::Store32(base,0x82CCAA80,0); hfr::Store32(base,0x82CCBFD4,0);
  Send(MessageType::ReloadRequest,v.world);
  REXLOG_INFO("KK co-op flow: death/reload fallback, both reload the host checkpoint");
}
void RejoinBackToMenu() { rejoin_cancel=true; }
#if defined(KK_DEV_TOOLS)
bool SessionDevFollowPad(uint32_t user,uint8_t* state) {
  if (!DevFollowEnabled() || user!=0 || dev_follow_at<0 || Now()-dev_follow_at>.5) return false;
  std::memset(state,0,16);
  const uint32_t packet=std::byteswap(uint32_t(Now()*20));
  const auto x=std::byteswap(dev_follow_pad.x), y=std::byteswap(dev_follow_pad.y);
  std::memcpy(state,&packet,4); std::memcpy(state+8,&x,2); std::memcpy(state+10,&y,2);
  return true;
}
bool SessionDevObjectReload(uint8_t* base) {
  const auto v=SessionFlowSnapshot(); const auto now=Now(); const auto link=EnemyLinkSnapshot();
  const auto condition=LocalPlayerCondition(base);
  static double object_reload_log=0;
  if (now>=object_reload_log) {
    object_reload_log=now+2;
    REXLOG_INFO("KK dev object reload guards: launched={} active={} host={} playing={} down={} private={}/{} catchup={} condition={} partner={} safe={}",
        launched,v.active,v.host,v.phase==FlowPhase::Playing,recovery.Downed(),private_load,private_request,
        int(catchup_phase),int(condition.value_or(PlayerCondition::Dead)),PartnerAliveHere(v,link,now),CatchupSafe(base,now));
  }
  if (!launched || !v.active || v.host || v.policy!=ChapterPolicy::Together ||
      v.phase!=FlowPhase::Playing || recovery.Downed() || private_load || private_request ||
      catchup_phase!=CatchupPhase::None || !condition || *condition==PlayerCondition::Dead ||
      !PartnerAliveHere(v,link,now) || !CatchupSafe(base,now)) return false;
  // Test only: exercise the existing living-guest catch-up load even when the
  // checkpoint number has not changed. Its timeout ends the session and never
  // enters the shared death/reload fallback.
  catchup_from_offer=true; catchup_target=v.checkpoint; catchup_phase=CatchupPhase::Requested;
  catchup_deadline=now+revive::kLoadTimeout; private_request=true;
  catchup_offer=move_offer=false; catchup_request=0; move_request=0; pending_move=0;
  catchup_notice_until=now+6;
  REXLOG_INFO("KK dev object reload: private guest recovery requested generation={} level={} checkpoint={}; host continues",
      v.world.generation,v.world.level,v.checkpoint);
  return true;
}
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
  if (catchup_phase==CatchupPhase::Placing) {
    const auto v=SessionFlowSnapshot(); const auto link=EnemyLinkSnapshot();
    if (!REXCVAR_GET(kk_coop_enabled) || stopping || !v.active || v.host || v.phase!=FlowPhase::Playing ||
        v.world.generation!=recovery_generation) { CancelCatchup(); return; }
    if (!PartnerAliveHere(v,link,Now()) || !ReviveTeleport(ctx,base,*link.partner)) return;
    catchup_phase=CatchupPhase::None; private_load=false;
    move_offer=false; behind=BehindRule{}; catchup_notice_until=Now()+4;
    ++life_incarnation; life_number=0; life_sent=false; PublishLife(v,false,Now());
    REXLOG_INFO("KK co-op catch-up: guest placed beside host generation {} level {} checkpoint {}; host never entered a load barrier",
        v.world.generation,v.world.level,catchup_target);
    catchup_from_guard=false; WorldGuardCompleted(desync::Tier::Reload);
  }
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
      catchup_notice_until=Now()+4; WorldGuardCompleted(desync::Tier::Move);
      REXLOG_INFO("KK co-op story: soft fix applied, native teleport beside host; no reload");
    } else if (Now()>move_until) {
      WorldGuardCancelled(desync::Tier::Move);
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
