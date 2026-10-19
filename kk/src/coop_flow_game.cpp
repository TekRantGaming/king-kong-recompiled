#include "coop_runtime.h"
#include "coop_pose.h"
#include "coop_observer.h"
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
int progress_slot=-1;
std::atomic<int> session_cheats{-1};
std::atomic<bool> stopping{false};
std::atomic<bool> move_offer{false};
uint32_t loaded_generation=0, loading_generation=0, recorded_checkpoint=0;
uint64_t applied_progress=0;
BehindRule behind;
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
void Send(MessageType type, FlowEvent f) { Message m; m.type=type; m.flow=f; SubmitFlowMessage(std::move(m)); }
void RecordProgress(const FlowView& v) {
  if (v.host || v.completed==applied_progress) return;
  if (play::ApplyTogetherProgress(personal,progress_slot,v.completed)) applied_progress=v.completed;
  else { Send(MessageType::FlowAbort,v.world); REXLOG_ERROR("KK co-op flow: personal chapter unlock could not be recorded"); }
}
FlowView Wait(uint32_t generation, bool load_barrier) {
  const double until=Now()+125;
  double paint=0;
  for (;;) {
    auto v=SessionFlowSnapshot();
    if (stopping || !v.active || v.phase==FlowPhase::Failed || v.world.generation!=generation ||
        (load_barrier && v.phase==FlowPhase::Playing)) return v;
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
  behind=BehindRule{};
  recorded_checkpoint=v.world.start;
  REXLOG_INFO("KK co-op flow: load generation {} level {} start {}",v.world.generation,v.world.level,v.world.start);
}
}
int SessionCheats() { return session_cheats.load(); }
bool MoveToPartnerOffered() { return move_offer.load(); }
void MoveToPartner() {
  const auto v=SessionFlowSnapshot();
  if (move_offer.exchange(false) && !v.host && v.phase==FlowPhase::Playing && v.policy==ChapterPolicy::Together)
    Send(MessageType::ReloadRequest,v.world);
}
void StopSessionFlow() { stopping=true; }
bool SessionInProcess() { return launched; }
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
  selected=r; requested=true; return true;
}
bool TakeSessionLaunch(play::StartRequest& r) {
  if (launched || !REXCVAR_GET(kk_coop_enabled)) return false;
  const auto v=SessionFlowSnapshot();
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
  if (v.world.generation>loaded_generation) { AskLoad(base,g,v); return false; }
  if (!v.host && v.policy==ChapterPolicy::Observer && observer::HoldWorld()) {
    Store32(base,g+4,0);
    v=Wait(v.world.generation,false);
    RecordProgress(v);
    if (v.world.generation>loaded_generation) { AskLoad(base,g,v); return false; }
    return true;
  }
  if (!v.host && v.checkpoint!=recorded_checkpoint && v.policy==ChapterPolicy::Together) {
    recorded_checkpoint=v.checkpoint; Store32(base,g+12,v.checkpoint);
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
      Send(level==v.world.level ? MessageType::ReloadRequest : MessageType::ChapterFinished,v.world);
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
    if (local.pose && link.partner && local.pose->level==link.partner->level &&
        PartnerDistance(*local.pose,*link.partner)<=100) move_offer=false;
  }
  return false;
}
void SessionAfterGo(uint8_t* base,uint32_t g,bool loaded) {
  if (!launched || !loaded || !loading_generation) return;
  auto v=SessionFlowSnapshot();
  if (v.world.generation!=loading_generation) return;
  if (hfr::Load32(base,g+0x18)!=v.world.level) { Send(MessageType::FlowAbort,v.world); return; }
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
  if (v.host && v.phase==FlowPhase::Playing && v.world.level==level && checkpoint<=10000)
    Send(MessageType::Checkpoint,{v.world.generation,level,checkpoint,v.completed});
}
void SessionReloadFrame(uint8_t* base) {
  if (!launched || !base) return;
  const auto v=SessionFlowSnapshot();
  if (!v.active || v.phase!=FlowPhase::Playing) return;
  // sub_82857F28 calls the frame-time hook, then consumes this reset flag.
  // Intercept here, before its re-init, not later in uni_go after consumption.
  if (hfr::Load32(base,0x82CCAA80)!=1 || hfr::Load32(base,0x82CCBFD4)>30) return;
  hfr::Store32(base,0x82CCAA80,0); hfr::Store32(base,0x82CCBFD4,0);
  if (!v.host && v.policy==ChapterPolicy::Observer) {
    observer::Fallback("Observer unavailable: local Jack died or local scripts requested a reset.");
    return;
  }
  Send(MessageType::ReloadRequest,v.world);
  REXLOG_INFO("KK co-op flow: death/reload fallback, both reload the host checkpoint");
}
}  // namespace kk::coop
